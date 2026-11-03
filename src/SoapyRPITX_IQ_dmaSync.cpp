// Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
// Project Site: https://github.com/hiperiondev/SoapySDR_rpitx
//
// This is based on other projects:
//    librpitx (https://github.com/F5OEO/librpitx)
//        Copyright (C) 2018  Evariste COURJAUD F5OEO
//    SoapyPlutoSDR (https://github.com/pothosware/SoapyPlutoSDR)
//
//    please contact their authors for more information.
//
// This is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// This software is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this software; see the file COPYING.  If not, write to
// the Free Software Foundation, Inc., 51 Franklin Street,
// Boston, MA 02110-1301, USA.

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <stdexcept>

#include <SoapySDR/Logger.hpp>

#include "util.hpp"
#include "SoapyRPITX_IQ_dmaSync.hpp"

namespace {

// Control blocks per ring slot: PAD, PLLC_FRAC, GPFSEL0 and the PCM pacing write.
constexpr uint32_t kCbPerSample = 4;
// User-memory words per ring slot: PLLC_FRAC, PAD, GPFSEL0.
constexpr uint32_t kRegPerSample = 3;
// Feeder thread period.
constexpr auto kFeederPeriod = std::chrono::microseconds(1000);
// Maximum blocking slice of a producer waiting for FIFO space.
constexpr auto kProducerSlice = std::chrono::milliseconds(2);
// Maximum time a control thread waits for the feeder to take a retune request.
constexpr auto kRetuneTimeout = std::chrono::seconds(3);
// Consecutive polls without a valid DMA position before the engine is considered stopped.
constexpr int kBadPollLimit = 5;
// Bound of the phase error carried between samples, in radians.
constexpr double kMaxPhaseCarry = 0.5;
// Signals handled when signal cleanup is requested.
const int kCleanupSignals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};

std::atomic<bool> g_hardwareClaimed{false};
std::atomic<SoapyRPITX_IQ_dmaSync *> g_activeTransmitter{nullptr};
std::once_flag g_exitHandlerOnce;
struct sigaction g_previousActions[NSIG];
bool g_signalsInstalled = false;

void exitHandler() {
    SoapyRPITX_IQ_dmaSync *tx = g_activeTransmitter.exchange(nullptr);
    if (tx != nullptr)
        tx->EmergencyStop();
}

// Turns the carrier off only when the signal is going to terminate the process (default action); a
// program that handles or ignores the signal keeps control of the transmitter.
void cleanupSignalHandler(int sig, siginfo_t *info, void *context) {
    const struct sigaction &prev = g_previousActions[sig];
    bool siginfo = (prev.sa_flags & SA_SIGINFO) != 0;

    if (!siginfo && prev.sa_handler == SIG_IGN)
        return;
    if (!siginfo && prev.sa_handler == SIG_DFL) {
        SoapyRPITX_IQ_dmaSync *tx = g_activeTransmitter.load();
        if (tx != nullptr)
            tx->SignalSafeStop();
        sigaction(sig, &prev, nullptr);
        raise(sig);
        return;
    }
    if (siginfo)
        prev.sa_sigaction(sig, info, context);
    else
        prev.sa_handler(sig);
}

void installSignalHandlers() {
    if (g_signalsInstalled)
        return;
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = cleanupSignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    for (int sig : kCleanupSignals)
        sigaction(sig, &sa, &g_previousActions[sig]);
    g_signalsInstalled = true;
}

void uninstallSignalHandlers() {
    if (!g_signalsInstalled)
        return;
    for (int sig : kCleanupSignals) {
        struct sigaction current;
        if (sigaction(sig, nullptr, &current) == 0 && (current.sa_flags & SA_SIGINFO) && current.sa_sigaction == cleanupSignalHandler)
            sigaction(sig, &g_previousActions[sig], nullptr);
    }
    g_signalsInstalled = false;
}

uint32_t clampRingSize(uint32_t ringSize) {
    return std::min<uint32_t>(std::max<uint32_t>(ringSize, RPITX_MIN_RING_SIZE), RPITX_MAX_RING_SIZE);
}

size_t fifoCapacityFor(uint32_t fifoMs, uint32_t sampleRate) {
    uint64_t samples = (uint64_t) sampleRate * fifoMs / 1000;
    return (size_t) std::max<uint64_t>(samples, 1024);
}

} // namespace

uint32_t SoapyRPITX_DefaultRingSize(uint32_t sampleRate) {
    uint64_t samples = (uint64_t) sampleRate * RPITX_DEFAULT_RING_MS / 1000;
    samples = (samples + 511) / 512 * 512;
    return (uint32_t) std::min<uint64_t>(std::max<uint64_t>(samples, 4096), RPITX_MAX_RING_SIZE);
}

bool SoapyRPITX_RingFitsRate(uint32_t ringSize, uint32_t sampleRate) {
    return (uint64_t) ringSize * 1000 >= (uint64_t) sampleRate * RPITX_MIN_RING_MS;
}

SoapyRPITX_HardwareClaim::SoapyRPITX_HardwareClaim() {
    if (g_hardwareClaimed.exchange(true))
        throw std::runtime_error("rpitx: the transmitter hardware is already in use by another stream");

    try {
        session.reset(new SoapyRPITX_Session(RPITX_TX_GPIO));
        if (session->Recovered())
            SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: recovered the transmitter left running by a terminated %s", session->RecoveryReport().c_str());

        std::string consumer;
        if (SoapyRPITX_GpioLineClaimed(RPITX_TX_GPIO, consumer))
            throw std::runtime_error("rpitx: GPIO" + std::to_string(RPITX_TX_GPIO) + " is claimed by '" + consumer +
                                     "'; disable the overlay or program that uses it (for example w1-gpio)");
        if (SoapyRPITX_I2sEnabled())
            throw std::runtime_error("rpitx: the PCM/I2S controller is enabled in the device tree (sound card overlay or dtparam=i2s=on); "
                                     "it is needed as the DMA pacing clock");

        padgpio p;
        originalPads = p.gpioreg[PADS_GPIO_0] & 0x1F;
    } catch (...) {
        session.reset();
        g_hardwareClaimed.store(false);
        throw;
    }
}

SoapyRPITX_HardwareClaim::~SoapyRPITX_HardwareClaim() {
    session.reset();
    g_hardwareClaimed.store(false);
}

SoapyRPITX_IQ_dmaSync::SoapyRPITX_IQ_dmaSync(const SoapyRPITX_TxConfig &cfg) :
        SoapyRPITX_HardwareClaim(),
        bufferdma(cfg.dmaChannel, clampRingSize(cfg.ringSize), kCbPerSample, kRegPerSample, cfg.dmaForce),
        clkgpio(),
        pcmgpio(),
        tuneCfg(cfg.tune),
        pendingTune(cfg.tune),
        fifoMs(cfg.fifoMs),
        prerollMs(cfg.prerollMs),
        rtPriority(cfg.rtPriority),
        cpu(cfg.cpu),
        signalCleanup(cfg.signalCleanup),
        driveLevel(std::min(std::max(cfg.driveLevel, 0), 7)),
        iqScale(std::max(cfg.iqScale, 0.0f)),
        constantEnvelope(cfg.constantEnvelope),
        fifo(fifoCapacityFor(cfg.fifoMs, RPITX_MAX_SAMPLE_RATE)),
        tracker(clampRingSize(cfg.ringSize)),
        chunk(512),
        shadow(clampRingSize(cfg.ringSize)) {
    if (cfg.tune.harmonic < 1)
        throw std::runtime_error("rpitx: harmonic must be >= 1");
    if (cfg.tune.sampleRate < RPITX_MIN_SAMPLE_RATE || cfg.tune.sampleRate > RPITX_MAX_SAMPLE_RATE)
        throw std::runtime_error("rpitx: unsupported sample rate");
    if (!SoapyRPITX_RingFitsRate(tracker.size(), cfg.tune.sampleRate))
        throw std::runtime_error("rpitx: the DMA ring of " + std::to_string(tracker.size()) + " samples is shorter than " + std::to_string(RPITX_MIN_RING_MS) +
                                 " ms at " + std::to_string(cfg.tune.sampleRate) + " Hz");

    volatile uint32_t *fsel = clkgpio::gengpio.gpioreg;
    originalGpioFunction = (fsel[GPFSEL0] >> (3 * RPITX_TX_GPIO)) & 7;
    WriteSessionRecord();

    try {
        if (!session->Recovered() && (clkgpio::gpioreg[GPCLK_CNTL] & CM_ENAB))
            SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: GPCLK0 was already running; another program may be using it");

        // A clock output left on GPIO4 (e.g. by a process that was killed while transmitting) is switched off first.
        if (originalGpioFunction == fsel_alt0)
            clkgpio::gengpio.setmode(RPITX_TX_GPIO, fsel_input);

        uint32_t base = fsel[GPFSEL0] & ~(7u << (3 * RPITX_TX_GPIO));
        fselIdle = base | ((uint32_t) fsel_input << (3 * RPITX_TX_GPIO));
        fselOn = base | ((uint32_t) fsel_alt0 << (3 * RPITX_TX_GPIO));

        SetDmaAlgo();

        clkgpio::SetAdvancedPllMode(true);
        WriteSessionRecord();
        pcmgpio::SetPllNumber(clk_plld, 1);

        if (!ApplyTune(tuneCfg))
            throw std::runtime_error("rpitx: the requested frequency cannot be produced by the PLL");

        ResetRing();
    } catch (...) {
        RestoreGpio();
        throw;
    }

    std::call_once(g_exitHandlerOnce, [] { std::atexit(exitHandler); });
    g_activeTransmitter.store(this);
    if (signalCleanup)
        installSignalHandlers();
}

SoapyRPITX_IQ_dmaSync::~SoapyRPITX_IQ_dmaSync() {
    if (signalCleanup)
        uninstallSignalHandlers();
    SoapyRPITX_IQ_dmaSync *self = this;
    g_activeTransmitter.compare_exchange_strong(self, nullptr);

    Stop();
    StopDma();
    RestoreGpio();
}

void SoapyRPITX_IQ_dmaSync::WriteSessionRecord() {
    SoapyRPITX_SessionRecord r;
    r.dmaChannel = GetChannel();
    r.memHandle = GetMemoryHandle();
    r.pads = originalPads;
    r.gpioFunction = originalGpioFunction;
    r.pcmValid = true;
    r.pcmCtl = pcmgpio::savedCtl;
    r.pcmDiv = pcmgpio::savedDiv;
    r.clockTree = clkgpio::SavedClockTree();
    session->Write(r);
}

void SoapyRPITX_IQ_dmaSync::SetDmaAlgo() {
    dma_cb_t *cbp = cbarray;
    for (uint32_t samplecnt = 0; samplecnt < buffersize; samplecnt++) {
        SetEasyCB(cbp, samplecnt * registerbysample + 1, dma_pad, 1);
        cbp++;

        SetEasyCB(cbp, samplecnt * registerbysample, dma_pllc_frac, 1);
        cbp++;

        SetEasyCB(cbp, samplecnt * registerbysample + 2, dma_fsel, 1);
        cbp++;

        // Paced by the PCM FIFO DREQ: one slot per sample period. The written value is irrelevant.
        SetEasyCB(cbp, samplecnt * registerbysample, dma_pcm, 1);
        cbp++;
    }

    // Close the loop.
    cbp--;
    cbp->next = mem_virt_to_phys(cbarray);
}

bool SoapyRPITX_IQ_dmaSync::ApplyTune(const SoapyRPITX_TuneConfig &tune) {
    if (tune.harmonic < 1 || !(tune.frequency > 0))
        return false;
    if (tune.sampleRate < RPITX_MIN_SAMPLE_RATE || tune.sampleRate > RPITX_MAX_SAMPLE_RATE)
        return false;
    if (!SoapyRPITX_RingFitsRate(tracker.size(), tune.sampleRate))
        return false;

    double previousPpm = clkgpio::Getppm();
    double basePpm = 0;
    if (tune.ppmFromNtp) {
        clkgpio::Setppm(0);
        clkgpio::SetppmFromNTP();
        basePpm = clkgpio::Getppm();
    }
    clkgpio::Setppm(basePpm + tune.ppm);

    uint64_t pllFrequency = (uint64_t) std::llround(tune.frequency / tune.harmonic);
    if (clkgpio::SetCenterFrequency(pllFrequency, (int) tune.sampleRate) != 0) {
        clkgpio::Setppm(previousPpm);
        return false;
    }
    if (!clkgpio::WaitPllcLock(2000))
        SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: PLLC did not report lock at %.0f Hz", tune.frequency);
    if (clkgpio::CrossesBoundary())
        SoapySDR_logf(SOAPY_SDR_WARNING,
                      "rpitx: at %.0f Hz the band of +/-%u Hz crosses a PLL integer step; deviations beyond the step are clipped "
                      "(a slightly different frequency or a lower sample rate avoids it)",
                      tune.frequency, tune.sampleRate);

    pcmgpio::Setppm(clkgpio::Getppm());
    pcmgpio::SetFrequency(tune.sampleRate);
    mydsp.samplerate = tune.sampleRate;
    mydsp.reset();
    phaseCarry = 0;
    carryToHz = (double) tune.sampleRate / (2.0 * M_PI);
    fifo.setLimit(fifoCapacityFor(fifoMs, tune.sampleRate));

    {
        std::lock_guard<std::mutex> lock(tuneMutex);
        tuneCfg = tune;
    }
    ComputeIdleWords();
    return true;
}

void SoapyRPITX_IQ_dmaSync::ComputeIdleWords() {
    idleFrac = (0x5Au << 24) | GetMasterFrac(0);
    idlePad = (0x5Au << 24) | originalPads;
}

void SoapyRPITX_IQ_dmaSync::WriteIdleSlot(uint32_t slot) {
    volatile uint32_t *tab = sampletab + (size_t) slot * registerbysample;
    tab[0] = idleFrac;
    tab[1] = idlePad;
    tab[2] = fselIdle;
    shadow[slot] = std::complex<float>(0.0f, 0.0f);
}

void SoapyRPITX_IQ_dmaSync::WriteDataSlot(uint32_t slot, std::complex<float> sample) {
    mydsp.pushsample(sample);
    shadow[slot] = sample;

    const int drive = driveLevel.load(std::memory_order_relaxed);
    int level;
    bool on;

    if (constantEnvelope.load(std::memory_order_relaxed)) {
        on = mydsp.amplitude >= mydsp.magnitude_gate;
        level = drive;
    } else {
        double a = mydsp.amplitude * (double) iqScale.load(std::memory_order_relaxed);
        if (!(a > 0))
            a = 0;
        if (a > 1)
            a = 1;
        // Drive levels 0..drive map to the upper part of the magnitude range; below it the carrier is off.
        level = (int) std::lround(a * (drive + 1)) - 1;
        on = level >= 0;
    }

    // The PLL realises a quantised frequency: the resulting phase error is added to the next sample.
    const double target = mydsp.frequency / tuneCfg.harmonic;
    const uint32_t frac = GetMasterFrac(target + phaseCarry * carryToHz);
    const double realised = GetMasterFracFrequency(frac);
    phaseCarry += (target - realised) / carryToHz;
    phaseCarry = std::min(std::max(phaseCarry, -kMaxPhaseCarry), kMaxPhaseCarry);

    volatile uint32_t *tab = sampletab + (size_t) slot * registerbysample;
    tab[0] = (0x5Au << 24) | frac;
    tab[1] = (0x5Au << 24) | (originalPads & PADS_HYST) | PADS_SLEW | ((uint32_t) (on ? level : 0) & PADS_DRIVE_MASK);
    tab[2] = on ? fselOn : fselIdle;
}

void SoapyRPITX_IQ_dmaSync::ResetRing() {
    for (uint32_t i = 0; i < buffersize; i++)
        WriteIdleSlot(i);
    tracker.reset();
    playedPos.store(0);
    lastDataPos.store(0);
    phaseCarry = 0;
}

void SoapyRPITX_IQ_dmaSync::StartDma() {
    // The engine restarts at slot 0, where ResetRing() put the read position and the pre-roll begins.
    bufferdma::start();
    lastPoll = clock::now();
    badPolls = 0;
    dmaRunning.store(true);
}

void SoapyRPITX_IQ_dmaSync::StopDma() {
    bufferdma::stop();
    dmaRunning.store(false);
}

void SoapyRPITX_IQ_dmaSync::CarrierOff() {
    clkgpio::gengpio.setmode(RPITX_TX_GPIO, fsel_input);
    pads.gpioreg[PADS_GPIO_0] = (0x5Au << 24) | originalPads;
}

void SoapyRPITX_IQ_dmaSync::RestoreGpio() {
    uint32_t function = originalGpioFunction == fsel_alt0 ? (uint32_t) fsel_input : originalGpioFunction;
    clkgpio::gengpio.setmode(RPITX_TX_GPIO, function);
    pads.gpioreg[PADS_GPIO_0] = (0x5Au << 24) | originalPads;
}

void SoapyRPITX_IQ_dmaSync::Start() {
    if (active.load())
        return;
    if (feeder.joinable())
        feeder.join();

    // Sample the other GPIO 0-9 functions now: the DMA rewrites the whole GPFSEL0 register.
    uint32_t base = clkgpio::gengpio.gpioreg[GPFSEL0] & ~(7u << (3 * RPITX_TX_GPIO));
    fselIdle = base | ((uint32_t) fsel_input << (3 * RPITX_TX_GPIO));
    fselOn = base | ((uint32_t) fsel_alt0 << (3 * RPITX_TX_GPIO));

    fifo.clear();
    consumedTotal.store(fifo.pushedTotal());
    replay.clear();
    replayPos = 0;
    replayLeft.store(0);
    endBurst.store(false);
    underflows.store(0);
    late.store(0);
    dmaErrors.store(0);
    samplesSent.store(0);
    inBurst = false;
    firstQueuedValid = false;
    badPolls = 0;
    mydsp.reset();
    ResetRing();

    running.store(true);
    feederExited.store(false);
    active.store(true);
    feeder = std::thread(&SoapyRPITX_IQ_dmaSync::FeederLoop, this);
}

void SoapyRPITX_IQ_dmaSync::JoinFeeder() {
    running.store(false);
    if (feeder.joinable())
        feeder.join();
}

void SoapyRPITX_IQ_dmaSync::Stop() {
    bool wasActive = active.exchange(false);
    if (!wasActive && !feeder.joinable())
        return;

    JoinFeeder();
    StopDma();
    RestoreGpio();
    ResetRing();
    fifo.clear();
    replay.clear();
    replayPos = 0;
    replayLeft.store(0);
    spaceCv.notify_all();

    {
        std::lock_guard<std::mutex> lock(tuneMutex);
        tunePending = false;
    }
    tuneCv.notify_all();
}

bool SoapyRPITX_IQ_dmaSync::IsActive() const {
    return active.load();
}

size_t SoapyRPITX_IQ_dmaSync::Write(const std::complex<float> *samples, size_t n, long timeoutUs) {
    if (!active.load() || n == 0)
        return 0;

    const auto deadline = clock::now() + std::chrono::microseconds(timeoutUs > 0 ? timeoutUs : 0);
    size_t done = 0;

    while (done < n) {
        size_t m = fifo.push(samples + done, n - done);
        if (m > 0) {
            done += m;
            endBurst.store(false);
        }
        if (done == n || timeoutUs <= 0 || !active.load())
            break;

        auto now = clock::now();
        if (now >= deadline)
            break;

        waitingProducers.fetch_add(1);
        {
            std::unique_lock<std::mutex> lock(spaceMutex);
            auto slice = std::min<clock::duration>(deadline - now, kProducerSlice);
            spaceCv.wait_for(lock, slice);
        }
        waitingProducers.fetch_sub(1);
    }

    return done;
}

void SoapyRPITX_IQ_dmaSync::EndBurst() {
    endBurst.store(true);
}

bool SoapyRPITX_IQ_dmaSync::WaitDrained(long timeoutUs) {
    const auto deadline = clock::now() + std::chrono::microseconds(timeoutUs > 0 ? timeoutUs : 0);
    while (active.load()) {
        // Samples are either in the FIFO (pushed, not consumed), in the replay buffer, or in the ring before
        // lastDataPos; the counters are read in the reverse order of their updates by the feeder.
        uint64_t consumed = consumedTotal.load();
        size_t replaying = replayLeft.load();
        uint64_t last = lastDataPos.load();
        if (fifo.pushedTotal() == consumed && replaying == 0 && playedPos.load() >= last)
            return true;
        if (clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool SoapyRPITX_IQ_dmaSync::Retune(const SoapyRPITX_TuneConfig &tune) {
    if (!running.load()) {
        // Feeder not running: the DMA is stopped and the hardware can be reprogrammed directly.
        bool ok = ApplyTune(tune);
        if (ok)
            ResetRing();
        return ok;
    }

    std::unique_lock<std::mutex> lock(tuneMutex);
    const uint64_t seq = ++tuneRequestSeq;
    pendingTune = tune;
    tunePending = true;

    tuneCv.wait_for(lock, kRetuneTimeout, [&] { return tuneTakenSeq >= seq || !running.load(); });
    if (tuneTakenSeq < seq) {
        // Not taken in time: withdraw the request, nothing was changed.
        tunePending = false;
        return false;
    }
    tuneCv.wait(lock, [&] { return tuneDoneSeq >= seq || feederExited.load(); });
    return tuneDoneSeq >= seq && tuneResult;
}

void SoapyRPITX_IQ_dmaSync::SetDriveLevel(int level) {
    driveLevel.store(std::min(std::max(level, 0), 7));
}

void SoapyRPITX_IQ_dmaSync::SetIqScale(float scale) {
    iqScale.store(scale > 0 ? scale : 0.0f);
}

void SoapyRPITX_IQ_dmaSync::SetConstantEnvelope(bool constant) {
    constantEnvelope.store(constant);
}

SoapyRPITX_TuneConfig SoapyRPITX_IQ_dmaSync::GetTune() const {
    std::lock_guard<std::mutex> lock(tuneMutex);
    return tuneCfg;
}

SoapyRPITX_TxStats SoapyRPITX_IQ_dmaSync::GetStats() {
    SoapyRPITX_TxStats stats;
    stats.underflows = underflows.load();
    stats.late = late.load();
    stats.dmaErrors = dmaErrors.load();
    stats.samplesSent = samplesSent.load();
    stats.fifoLevel = fifo.size();
    stats.fifoCapacity = fifo.capacity();
    stats.dmaRunning = dmaRunning.load() && bufferdma::isrunning();
    return stats;
}

size_t SoapyRPITX_IQ_dmaSync::FifoCapacity() const {
    return fifo.capacity();
}

uint32_t SoapyRPITX_IQ_dmaSync::RingSize() const {
    return tracker.size();
}

double SoapyRPITX_IQ_dmaSync::GetActualSampleRate() const {
    return pcmgpio::GetFrameRate();
}

bool SoapyRPITX_IQ_dmaSync::IsPllLocked() {
    return clkgpio::IsPllcLocked();
}

void SoapyRPITX_IQ_dmaSync::SignalSafeStop() {
    dma_reg.gpioreg[DMA_CS + channel * DMA_CHANNEL_WORDS] = BCM2708_DMA_RESET;
    volatile uint32_t *fsel = clkgpio::gengpio.gpioreg;
    fsel[GPFSEL0] = (fsel[GPFSEL0] & ~(7u << (3 * RPITX_TX_GPIO))) | ((uint32_t) fsel_input << (3 * RPITX_TX_GPIO));
    pads.gpioreg[PADS_GPIO_0] = (0x5Au << 24) | originalPads;
    clkgpio::gpioreg[GPCLK_CNTL] = CM_PASSWD | ((uint32_t) clkgpio::Mash << CM_MASH_SHIFT) | (uint32_t) clkgpio::pllnumber;
}

void SoapyRPITX_IQ_dmaSync::EmergencyStop() {
    // "active" stays set so that Stop() still joins the thread and completes the shutdown.
    running.store(false);
    for (int i = 0; i < 50 && !feederExited.load(); i++)
        usleep(1000);
    SignalSafeStop();
    StopDma();
    CarrierOff();
}

void SoapyRPITX_IQ_dmaSync::SetupFeederThread() {
    if (rtPriority > 0) {
        sched_param sp{};
        sp.sched_priority = std::min(rtPriority, sched_get_priority_max(SCHED_FIFO));
        int err = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (err != 0)
            SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: cannot set SCHED_FIFO priority %d for the feeder thread (error %d)", sp.sched_priority, err);
    }

    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        int err = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
        if (err != 0)
            SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: cannot pin the feeder thread to CPU %d (error %d)", cpu, err);
    }
}

uint32_t SoapyRPITX_IQ_dmaSync::PumpData(uint32_t target) {
    uint32_t total = 0;
    bool fromFifo = false;

    for (;;) {
        uint32_t w = tracker.writable(target);
        if (w == 0)
            break;

        const size_t want = std::min<size_t>(w, chunk.size());
        size_t m = 0;
        const bool replaying = replayPos < replay.size();
        if (replaying) {
            m = std::min(want, replay.size() - replayPos);
            std::copy(replay.begin() + (std::ptrdiff_t) replayPos, replay.begin() + (std::ptrdiff_t)(replayPos + m), chunk.begin());
            replayPos += m;
        } else {
            m = fifo.pop(chunk.data(), want);
            fromFifo = fromFifo || m > 0;
        }
        if (m == 0)
            break;

        uint64_t pos = tracker.writePos();
        for (size_t i = 0; i < m; i++)
            WriteDataSlot(tracker.slotOf(pos + i), chunk[i]);
        tracker.advanceWrite((uint32_t) m);
        total += (uint32_t) m;

        // Publish the new end of data before the samples stop being counted as queued, so that WaitDrained()
        // never sees them in neither place.
        lastDataPos.store(tracker.writePos());
        if (replaying) {
            if (replayPos == replay.size()) {
                replay.clear();
                replayPos = 0;
            }
            replayLeft.store(replay.size() - replayPos);
        } else {
            consumedTotal.fetch_add(m);
        }
    }

    if (total > 0) {
        samplesSent.fetch_add(total);
        if (fromFifo && waitingProducers.load() > 0)
            spaceCv.notify_all();
    }
    return total;
}

void SoapyRPITX_IQ_dmaSync::PadIdle(uint32_t target) {
    uint32_t w = tracker.writable(target);
    uint64_t pos = tracker.writePos();
    for (uint32_t i = 0; i < w; i++)
        WriteIdleSlot(tracker.slotOf(pos + i));
    tracker.advanceWrite(w);
}

void SoapyRPITX_IQ_dmaSync::ScrubCompleted() {
    uint64_t begin, end;
    tracker.scrubRange(begin, end);
    if (end - begin > tracker.size())
        begin = end - tracker.size();
    for (uint64_t p = begin; p < end; p++)
        WriteIdleSlot(tracker.slotOf(p));
    tracker.markScrubbed(end);
}

bool SoapyRPITX_IQ_dmaSync::FeederRetune(const SoapyRPITX_TuneConfig &request) {
    const bool wasRunning = dmaRunning.load();
    if (wasRunning) {
        int slot = GetCurrentSample();
        if (slot >= 0) {
            auto now = clock::now();
            tracker.update((uint32_t) slot, std::chrono::duration<double>(now - lastPoll).count() * tuneCfg.sampleRate);
            lastPoll = now;
        }
    }
    StopDma();
    CarrierOff();

    // Keep the samples that were written to the ring but not played: they are sent first after the retune.
    // The slot that was executing is included, so at most one sample is sent twice and none is lost.
    std::vector<std::complex<float>> pending;
    const uint64_t first = tracker.readPos();
    const uint64_t end = lastDataPos.load();
    if (end > first && end - first <= tracker.size()) {
        pending.reserve((size_t) (end - first) + (replay.size() - replayPos));
        for (uint64_t p = first; p < end; p++)
            pending.push_back(shadow[tracker.slotOf(p)]);
    }
    pending.insert(pending.end(), replay.begin() + (std::ptrdiff_t) replayPos, replay.end());
    replay.swap(pending);
    replayPos = 0;
    replayLeft.store(replay.size());

    bool ok = ApplyTune(request);
    ResetRing();
    inBurst = false;
    firstQueuedValid = false;
    badPolls = 0;
    return ok;
}

void SoapyRPITX_IQ_dmaSync::RecoverDma() {
    dmaErrors.fetch_add(1);
    SoapySDR_logf(SOAPY_SDR_ERROR, "rpitx: the DMA engine stopped (CS 0x%08x, DEBUG 0x%08x); restarting it", GetStatus(), GetDebugStatus());
    FeederRetune(tuneCfg);
}

void SoapyRPITX_IQ_dmaSync::FeederLoop() {
    SetupFeederThread();

    auto next = clock::now();

    while (running.load()) {
        // Retune request from the control thread.
        SoapyRPITX_TuneConfig request;
        uint64_t seq = 0;
        {
            std::lock_guard<std::mutex> lock(tuneMutex);
            if (tunePending) {
                request = pendingTune;
                seq = tuneRequestSeq;
                tuneTakenSeq = seq;
                tunePending = false;
            }
        }
        if (seq != 0) {
            tuneCv.notify_all();
            bool ok = FeederRetune(request);
            {
                std::lock_guard<std::mutex> lock(tuneMutex);
                tuneResult = ok;
                tuneDoneSeq = seq;
            }
            tuneCv.notify_all();
        }

        const auto now = clock::now();
        const uint32_t n = tracker.size();
        const uint32_t rate = tuneCfg.sampleRate;
        const uint32_t target = n * 3 / 4;
        const uint32_t low = std::min<uint32_t>(target, std::max<uint32_t>(n / 8, rate / 200));
        const uint32_t guard = std::min<uint32_t>(n / 4, std::max<uint32_t>(32, rate / 500));

        if (!dmaRunning.load()) {
            PumpData(target);

            size_t queued = fifo.size() + replayLeft.load() + (size_t) std::max<int64_t>(tracker.fill(), 0);
            if (queued > 0 && !firstQueuedValid) {
                firstQueuedTime = now;
                firstQueuedValid = true;
            }

            uint64_t preroll = (uint64_t) rate * prerollMs / 1000;
            bool waitedLongEnough = firstQueuedValid && (now - firstQueuedTime) >= std::chrono::milliseconds(prerollMs);
            if (running.load() && queued > 0 && (queued >= preroll || endBurst.load() || fifo.space() == 0 || waitedLongEnough)) {
                StartDma();
                inBurst = true;
                firstQueuedValid = false;
            }
        } else {
            int slot = GetCurrentSample();
            if (slot < 0 || !bufferdma::isrunning()) {
                if (++badPolls >= kBadPollLimit)
                    RecoverDma();
            } else {
                badPolls = 0;
                double elapsed = std::chrono::duration<double>(now - lastPoll).count();
                lastPoll = now;

                if (tracker.update((uint32_t) slot, elapsed * rate)) {
                    // The engine overtook the writer: silence the whole ring except the executing slot, then
                    // restart writing just ahead of the engine.
                    late.fetch_add(1);
                    const uint32_t executing = tracker.slotOf(tracker.readPos());
                    for (uint32_t i = 0; i < n; i++) {
                        if (i != executing)
                            WriteIdleSlot(i);
                    }
                    tracker.resync(guard);
                    SoapySDR_logf(SOAPY_SDR_DEBUG, "rpitx: DMA overran the feeder");
                }
                playedPos.store(tracker.readPos());

                ScrubCompleted();
                if (PumpData(target) > 0)
                    inBurst = true;

                if (fifo.size() == 0 && replayLeft.load() == 0 && tracker.fill() < (int64_t) low) {
                    if (inBurst) {
                        if (!endBurst.load())
                            underflows.fetch_add(1);
                        inBurst = false;
                    }
                    PadIdle(low);
                }
            }
        }

        next += kFeederPeriod;
        if (next < now)
            next = now + kFeederPeriod;

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(next.time_since_epoch()).count();
        timespec ts;
        ts.tv_sec = (time_t) (ns / 1000000000LL);
        ts.tv_nsec = (long) (ns % 1000000000LL);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
        }
    }

    feederExited.store(true);
    tuneCv.notify_all();
}
