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

#include "rpi.hpp"
#include "util.hpp"
#include "SoapyRPITX_IQ_dmaSync.hpp"

namespace {

// Feeder thread period.
constexpr auto kFeederPeriod = std::chrono::microseconds(1000);
// Maximum blocking slice of a producer waiting for FIFO space.
constexpr auto kProducerSlice = std::chrono::milliseconds(2);
// Maximum time a control thread waits for the feeder to take a retune request.
constexpr auto kRetuneTimeout = std::chrono::seconds(3);
// Largest phase correction applied in one slot, in radians.
constexpr double kMaxPhaseStep = 0.5;
// Bound of the phase error carried between slots, in radians.
constexpr double kMaxPhaseCarry = 2 * M_PI;
// Time the envelope must stay below the floor before the carrier is switched off.
constexpr double kFloorHangSeconds = 0.002;
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

// Slots kept between the executing slot and the slots the feeder writes behind the write position.
uint32_t guardSlots(uint32_t ringSize, uint32_t slotRate) {
    return std::min<uint32_t>(ringSize / 4, std::max<uint32_t>(32, slotRate / 500));
}

// Words held in the pacing FIFO.
uint32_t pacingLevelFor(SoapyRPITX_EnvelopeOutput out) {
    return out == SoapyRPITX_EnvelopeOutput::Pwm ? PWM_DREQ_LEVEL : PCM_TX_DREQ_LEVEL;
}

// Slots between the registers of a slot taking effect and its pacing word leaving the FIFO: the word enters
// behind the FIFO level when the next slot starts, and is output one frame after the words ahead of it.
double envelopeLatencyFor(SoapyRPITX_EnvelopeOutput out) {
    return out == SoapyRPITX_EnvelopeOutput::None ? 0.0 : (double) pacingLevelFor(out) + 1.0;
}

// Largest number of slots the envelope words may be written ahead of their sample.
uint32_t maxAdvanceFor(uint32_t ringSize) {
    return ringSize / 8;
}

bool advanceFits(const SoapyRPITX_AmplitudeConfig &a, uint32_t slotsPerSample, SoapyRPITX_EnvelopeOutput out, uint32_t ringSize) {
    int advance = 0;
    SoapyRPITX_SplitEnvelopeDelay(a.envelopeDelay * slotsPerSample, envelopeLatencyFor(out), advance);
    return (uint32_t) advance <= maxAdvanceFor(ringSize);
}

std::vector<unsigned> extraPinsFor(const SoapyRPITX_TxConfig &cfg) {
    std::vector<unsigned> pins;
    if (cfg.rfDac)
        pins.push_back(RPITX_TX2_GPIO);
    if (cfg.envelopeOutput == SoapyRPITX_EnvelopeOutput::Pcm) {
        pins.push_back(18);
        pins.push_back(19);
        pins.push_back(21);
    } else if (cfg.envelopeOutput == SoapyRPITX_EnvelopeOutput::Pwm) {
        pins.push_back((unsigned) cfg.pwmGpio);
    }
    return pins;
}

} // namespace

uint32_t SoapyRPITX_DefaultRingSize(uint32_t slotRate) {
    uint64_t samples = (uint64_t) slotRate * RPITX_DEFAULT_RING_MS / 1000;
    samples = (samples + 511) / 512 * 512;
    return (uint32_t) std::min<uint64_t>(std::max<uint64_t>(samples, 4096), RPITX_MAX_RING_SIZE);
}

bool SoapyRPITX_RingFitsRate(uint32_t ringSize, uint32_t slotRate) {
    return (uint64_t) ringSize * 1000 >= (uint64_t) slotRate * RPITX_MIN_RING_MS;
}

void SoapyRPITX_CheckTxConfig(const SoapyRPITX_TxConfig &cfg) {
    if (cfg.tune.harmonic < 1)
        throw std::runtime_error("rpitx: harmonic must be >= 1");
    if (cfg.slotsPerSample < 1 || cfg.slotsPerSample > RPITX_MAX_SLOTS_PER_SAMPLE)
        throw std::runtime_error("rpitx: slots per sample must be 1 to " + std::to_string(RPITX_MAX_SLOTS_PER_SAMPLE));
    if (cfg.tune.sampleRate < RPITX_MIN_SAMPLE_RATE || cfg.tune.sampleRate > RPITX_MAX_SAMPLE_RATE)
        throw std::runtime_error("rpitx: unsupported sample rate");
    const uint64_t slotRate = (uint64_t) cfg.tune.sampleRate * cfg.slotsPerSample;
    if (slotRate > RPITX_MAX_SAMPLE_RATE)
        throw std::runtime_error("rpitx: the slot rate " + std::to_string(slotRate) + " Hz (sample rate x slots per sample) exceeds " +
                                 std::to_string(RPITX_MAX_SAMPLE_RATE) + " Hz");
    const uint32_t ring = clampRingSize(cfg.ringSize);
    if (!SoapyRPITX_RingFitsRate(ring, (uint32_t) slotRate))
        throw std::runtime_error("rpitx: the DMA ring of " + std::to_string(ring) + " slots is shorter than " + std::to_string(RPITX_MIN_RING_MS) + " ms at " +
                                 std::to_string(slotRate) + " slots/s");
    if (cfg.envelopeOutput == SoapyRPITX_EnvelopeOutput::Pwm) {
        if (cfg.pwmGpio != 12 && cfg.pwmGpio != 18)
            throw std::runtime_error("rpitx: the PWM envelope output must be GPIO12 or GPIO18");
        if (cfg.pwmBits < 6 || cfg.pwmBits > 12)
            throw std::runtime_error("rpitx: PWM envelope resolution must be 6 to 12 bits");
    }
    std::string why = SoapyRPITX_CheckAmplitudeConfig(cfg.amplitude);
    if (!why.empty())
        throw std::runtime_error("rpitx: " + why);
    if (!advanceFits(cfg.amplitude, cfg.slotsPerSample, cfg.envelopeOutput, ring))
        throw std::runtime_error("rpitx: envelope_delay needs more look-ahead than the DMA ring allows (" + std::to_string(maxAdvanceFor(ring)) + " slots)");
}

SoapyRPITX_HardwareClaim::SoapyRPITX_HardwareClaim(const SoapyRPITX_TxConfig &cfg) {
    SoapyRPITX_CheckTxConfig(cfg);
    if (g_hardwareClaimed.exchange(true))
        throw std::runtime_error("rpitx: the transmitter hardware is already in use by another stream");

    try {
        session.reset(new SoapyRPITX_Session(RPITX_TX_GPIO));
        if (session->Recovered())
            SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: recovered the transmitter left running by a terminated %s", session->RecoveryReport().c_str());
        if (session->Discarded())
            SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: discarded an implausible transmitter record (%s); nothing was restored",
                          session->RecoveryReport().c_str());

        std::vector<unsigned> pins = extraPinsFor(cfg);
        std::vector<unsigned> checked = pins;
        checked.insert(checked.begin(), RPITX_TX_GPIO);
        for (unsigned pin : checked) {
            std::string consumer;
            bool queried = false;
            if (SoapyRPITX_GpioLineClaimed(pin, consumer, queried))
                throw std::runtime_error("rpitx: GPIO" + std::to_string(pin) + " is claimed by '" + consumer +
                                         "'; disable the overlay or program that uses it (for example w1-gpio)");
            if (!queried)
                SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: cannot check whether GPIO%u is used by a driver (no GPIO character device answered)", pin);
        }
        if (cfg.envelopeOutput != SoapyRPITX_EnvelopeOutput::Pwm && SoapyRPITX_I2sEnabled())
            throw std::runtime_error("rpitx: the PCM/I2S controller is enabled in the device tree (sound card overlay or dtparam=i2s=on); "
                                     "it is needed as the DMA pacing clock");
        if (cfg.envelopeOutput == SoapyRPITX_EnvelopeOutput::Pwm && SoapyRPITX_PwmEnabled())
            throw std::runtime_error(
                    "rpitx: the PWM block is enabled in the device tree (pwm overlay or dtparam=audio=on); it is needed for the envelope output");

        padgpio p;
        originalPads = p.gpioreg[PADS_GPIO_0] & 0x1F;
        generalgpio g;
        auto functionOf = [&](unsigned pin) { return (g.gpioreg[pin / 10] >> (3 * (pin % 10))) & 7; };
        originalGpioFunction = functionOf(RPITX_TX_GPIO);
        for (unsigned pin : pins)
            originalPins.emplace_back(pin, functionOf(pin));

        record.pads = originalPads;
        record.gpioFunction = originalGpioFunction;
        record.pinFunctions = originalPins;
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

void SoapyRPITX_HardwareClaim::RecordDmaAllocation(int channel, unsigned memHandle, unsigned memBus, unsigned memSize) {
    bool known = false;
    if (channel >= 0)
        bcm_host_get_kernel_dma_mask(&known);
    record.dmaChannel = channel;
    record.dmaMaskKnown = channel < 0 || known;
    record.memHandle = memHandle;
    record.memBus = memBus;
    record.memSize = memSize;
    PublishRecord();
}

std::function<void(int, unsigned, unsigned, unsigned)> SoapyRPITX_HardwareClaim::AllocationRecorder() {
    return [this](int channel, unsigned memHandle, unsigned memBus, unsigned memSize) { RecordDmaAllocation(channel, memHandle, memBus, memSize); };
}

void SoapyRPITX_HardwareClaim::PublishRecord() {
    session->Write(record);
}

uint32_t SoapyRPITX_IQ_dmaSync::WordsPerSlot(const SoapyRPITX_TxConfig &cfg) {
    return 3 + (cfg.rfDac ? 1 : 0) + (cfg.envelopeOutput != SoapyRPITX_EnvelopeOutput::None ? 1 : 0);
}

uint32_t SoapyRPITX_IQ_dmaSync::CbsPerSlot(const SoapyRPITX_TxConfig &cfg) {
    return 4 + (cfg.rfDac ? 1 : 0);
}

SoapyRPITX_IQ_dmaSync::SoapyRPITX_IQ_dmaSync(const SoapyRPITX_TxConfig &cfg) :
        SoapyRPITX_HardwareClaim(cfg),
        bufferdma(cfg.dmaChannel, clampRingSize(cfg.ringSize), CbsPerSlot(cfg), WordsPerSlot(cfg), cfg.dmaForce, AllocationRecorder()),
        clkgpio(),
        pcmgpio(),
        tuneCfg(cfg.tune),
        pendingTune(cfg.tune),
        fifoMs(cfg.fifoMs),
        prerollMs(cfg.prerollMs),
        rtPriority(cfg.rtPriority),
        cpu(cfg.cpu),
        signalCleanup(cfg.signalCleanup),
        slotsPerSample(cfg.slotsPerSample),
        envelopeOutput(cfg.envelopeOutput),
        pwmGpio(cfg.pwmGpio),
        pwmRange(1u << std::min(std::max(cfg.pwmBits, 6), 12)),
        rfDac(cfg.rfDac),
        wordsPerSlot(WordsPerSlot(cfg)),
        wordFsel2(cfg.rfDac ? 3 : -1),
        wordEnv(cfg.envelopeOutput != SoapyRPITX_EnvelopeOutput::None ? (cfg.rfDac ? 4 : 3) : -1),
        pacingLevel(pacingLevelFor(cfg.envelopeOutput)),
        driveLevel(std::min(std::max(cfg.driveLevel, 0), 7)),
        iqScale(std::max(cfg.iqScale, 0.0f)),
        constantEnvelope(cfg.constantEnvelope),
        ampCfg(cfg.amplitude),
        fifo(fifoCapacityFor(cfg.fifoMs, RPITX_MAX_SAMPLE_RATE)),
        tracker(clampRingSize(cfg.ringSize)),
        chunk(512),
        shadow(clampRingSize(cfg.ringSize)),
        slotIsData(clampRingSize(cfg.ringSize), 0) {
    volatile uint32_t *gp = clkgpio::gengpio.gpioreg;
    clkgpio::SaveClockTree();
    WriteSessionRecord();

    try {
        if (!session->Recovered() && (clkgpio::gpioreg[GPCLK_CNTL] & CM_ENAB))
            SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: GPCLK0 was already running; another program may be using it");

        // A clock output left on the RF pins (e.g. by a process that was killed while transmitting) is switched off first.
        if (originalGpioFunction == fsel_alt0)
            clkgpio::gengpio.setmode(RPITX_TX_GPIO, fsel_input);
        for (const auto &pf : originalPins) {
            if (pf.first == RPITX_TX2_GPIO && rfDac && pf.second == fsel_alt5)
                clkgpio::gengpio.setmode(RPITX_TX2_GPIO, fsel_input);
        }

        uint32_t base = gp[GPFSEL0] & ~(7u << (3 * RPITX_TX_GPIO));
        fselIdle = base | ((uint32_t) fsel_input << (3 * RPITX_TX_GPIO));
        fselOn = base | ((uint32_t) fsel_alt0 << (3 * RPITX_TX_GPIO));
        uint32_t base2 = gp[GPFSEL2] & ~(7u << (3 * (RPITX_TX2_GPIO % 10)));
        fsel2Idle = base2 | ((uint32_t) fsel_input << (3 * (RPITX_TX2_GPIO % 10)));
        fsel2On = base2 | ((uint32_t) fsel_alt5 << (3 * (RPITX_TX2_GPIO % 10)));

        SetDmaAlgo();

        clkgpio::SetAdvancedPllMode(true);
        pcmgpio::SetPllNumber(clk_plld, 1);

        if (envelopeOutput == SoapyRPITX_EnvelopeOutput::Pcm) {
            clkgpio::gengpio.setmode(18, fsel_alt0); // PCM_CLK
            clkgpio::gengpio.setmode(19, fsel_alt0); // PCM_FS
            clkgpio::gengpio.setmode(21, fsel_alt0); // PCM_DOUT
        } else if (envelopeOutput == SoapyRPITX_EnvelopeOutput::Pwm) {
            clkgpio::gengpio.setmode((uint32_t) pwmGpio, pwmGpio == 18 ? fsel_alt5 : fsel_alt0); // PWM0
        }

        PublishModel();
        RefreshModel();

        if (!ApplyTune(tuneCfg))
            throw std::runtime_error("rpitx: the requested frequency or the envelope DAC rate cannot be produced");

        ResetRing();
    } catch (...) {
        RestoreGpio();
        RestoreEnvelopePins();
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
    RestoreEnvelopePins();
}

void SoapyRPITX_IQ_dmaSync::WriteSessionRecord() {
    record.pcmValid = envelopeOutput != SoapyRPITX_EnvelopeOutput::Pwm;
    record.pcmCtl = pcmgpio::savedCtl;
    record.pcmDiv = pcmgpio::savedDiv;
    record.pwmValid = envelopeOutput == SoapyRPITX_EnvelopeOutput::Pwm;
    record.pwmClkCtl = pwm.savedClkCtl;
    record.pwmClkDiv = pwm.savedClkDiv;
    record.pwmCtl = pwm.savedCtl;
    record.pwmRng = pwm.savedRng;
    record.pwmDmac = pwm.savedDmac;
    record.clockTree = clkgpio::SavedClockTree();
    PublishRecord();
}

void SoapyRPITX_IQ_dmaSync::SetDmaAlgo() {
    dma_cb_t *cbp = cbarray;
    const dma_common_reg pacing = envelopeOutput == SoapyRPITX_EnvelopeOutput::Pwm ? dma_pwm : dma_pcm;
    for (uint32_t slot = 0; slot < buffersize; slot++) {
        const uint32_t base = slot * registerbysample;
        SetEasyCB(cbp++, base + kWordPad, dma_pad, 1);
        SetEasyCB(cbp++, base + kWordFrac, dma_pllc_frac, 1);
        SetEasyCB(cbp++, base + kWordFsel0, dma_fsel, 1);
        if (wordFsel2 >= 0)
            SetEasyCB(cbp++, base + (uint32_t) wordFsel2, dma_fsel2, 1);
        // Paced by the PCM or PWM FIFO DREQ: one slot per slot period. The word is the envelope code with an
        // envelope DAC, a dummy value otherwise.
        SetEasyCB(cbp++, base + (wordEnv >= 0 ? (uint32_t) wordEnv : (uint32_t) kWordFrac), pacing, 1);
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
    const uint64_t rate64 = (uint64_t) tune.sampleRate * slotsPerSample;
    if (rate64 > RPITX_MAX_SAMPLE_RATE)
        return false;
    const uint32_t rate = (uint32_t) rate64;
    if (!SoapyRPITX_RingFitsRate(tracker.size(), rate))
        return false;

    // Every refusal below restores the three crystal corrections, so the clocks keep matching the hardware.
    const double previousClkPpm = clkgpio::Getppm();
    const double previousPcmPpm = pcmgpio::Getppm();
    const double previousPwmPpm = pwm.Getppm();
    auto rollback = [&] {
        clkgpio::Setppm(previousClkPpm);
        pcmgpio::Setppm(previousPcmPpm);
        pwm.Setppm(previousPwmPpm);
        return false;
    };

    double basePpm = 0;
    if (tune.ppmFromNtp) {
        clkgpio::Setppm(0);
        clkgpio::SetppmFromNTP();
        basePpm = clkgpio::Getppm();
    }
    clkgpio::Setppm(basePpm + tune.ppm);
    const double ppm = clkgpio::Getppm();

    // The pacing clock (and the envelope DAC) must be able to run at the slot rate before anything is reprogrammed.
    bool pacingFits;
    if (envelopeOutput == SoapyRPITX_EnvelopeOutput::Pwm) {
        pwm.Setppm(ppm);
        pacingFits = pwm.FrequencyFits(rate, pwmRange);
    } else {
        pcmgpio::Setppm(ppm);
        pacingFits = envelopeOutput == SoapyRPITX_EnvelopeOutput::Pcm ? pcmgpio::DacFrequencyFits(rate) : pcmgpio::FrequencyFits(rate);
    }
    if (!pacingFits)
        return rollback();

    // Instantaneous frequencies stay within +/- rate / 2, plus the largest phase correction of one slot.
    const int bandwidth = (int) std::ceil((0.5 + kMaxPhaseStep / (2.0 * M_PI)) * (double) rate);
    uint64_t pllFrequency = (uint64_t) std::llround(tune.frequency / tune.harmonic);
    if (clkgpio::SetCenterFrequency(pllFrequency, bandwidth) != 0)
        return rollback();
    if (!clkgpio::WaitPllcLock(2000))
        SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: PLLC did not report lock at %.0f Hz", tune.frequency);
    if (clkgpio::CrossesBoundary())
        SoapySDR_logf(SOAPY_SDR_WARNING,
                      "rpitx: at %.0f Hz the band of +/-%d Hz crosses a PLL integer step; deviations beyond the step are clipped "
                      "(a slightly different frequency or a lower sample rate avoids it)",
                      tune.frequency, bandwidth);

    int pacingError;
    if (envelopeOutput == SoapyRPITX_EnvelopeOutput::Pwm)
        pacingError = pwm.SetFrequency(rate, pwmRange);
    else if (envelopeOutput == SoapyRPITX_EnvelopeOutput::Pcm)
        pacingError = pcmgpio::SetDacFrequency(rate);
    else
        pacingError = pcmgpio::SetFrequency(rate);
    if (pacingError != 0) {
        // Checked above with the same clock source and correction: only reached when the hardware changed.
        // The carrier PLL goes back to the configuration still in effect, so that the hardware matches it.
        SoapySDR_logf(SOAPY_SDR_ERROR, "rpitx: the DMA pacing clock cannot run at %u Hz; the previous carrier frequency is restored", rate);
        rollback();
        if (slotRate > 0) {
            const int previousBandwidth = (int) std::ceil((0.5 + kMaxPhaseStep / (2.0 * M_PI)) * (double) slotRate);
            const uint64_t previousPll = (uint64_t) std::llround(tuneCfg.frequency / tuneCfg.harmonic);
            if (clkgpio::SetCenterFrequency(previousPll, previousBandwidth) != 0)
                SoapySDR_logf(SOAPY_SDR_ERROR, "rpitx: the previous carrier frequency %.0f Hz cannot be restored", tuneCfg.frequency);
        }
        return false;
    }

    slotRate = rate;
    mydsp.samplerate = rate;
    mydsp.reset();
    phaseCarry = 0;
    appliedPhase = 0;
    carryToHz = (double) rate / (2.0 * M_PI);
    fifo.setLimit(fifoCapacityFor(fifoMs, tune.sampleRate));
    const double frames = envelopeOutput == SoapyRPITX_EnvelopeOutput::Pwm ? pwm.GetFrameRate() : pcmgpio::GetFrameRate();
    actualSampleRate.store(frames / slotsPerSample);

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

void SoapyRPITX_IQ_dmaSync::PublishModel() {
    std::lock_guard<std::mutex> lock(modelMutex);
    auto m = std::make_shared<const SoapyRPITX_AmplitudeModel>(ampCfg, driveLevel.load(), rfDac);
    std::atomic_store(&published, m);
}

void SoapyRPITX_IQ_dmaSync::RefreshModel() {
    std::shared_ptr<const SoapyRPITX_AmplitudeModel> m = std::atomic_load(&published);
    if (m == model)
        return;
    model = m;

    envDelaySlots = SoapyRPITX_SplitEnvelopeDelay(m->config.envelopeDelay * slotsPerSample, envelopeLatencyFor(envelopeOutput), envAdvance);
    envAdvance = std::min<int>(envAdvance, (int) maxAdvanceFor(tracker.size()));
    envDelaySlots = std::min(envDelaySlots, (double) (SoapyRPITX_EnvelopeDelay::kLength - 2));

    double idle = std::min(std::max(m->config.eerIdle, 0.0), 1.0);
    if (envelopeOutput == SoapyRPITX_EnvelopeOutput::Pcm) {
        uint32_t c = (uint32_t) std::lround(idle * 32767.0);
        idleEnv = (c << 16) | c;
    } else if (envelopeOutput == SoapyRPITX_EnvelopeOutput::Pwm) {
        idleEnv = (uint32_t) std::lround(idle * (double) pwmRange);
    } else {
        idleEnv = 0;
    }
}

void SoapyRPITX_IQ_dmaSync::ResetAmplitudeState() {
    quantizer.reset();
    envHistory.reset();
    floorCount = 0;
    prevIq = std::complex<float>(0.0f, 0.0f);
}

void SoapyRPITX_IQ_dmaSync::WriteIdleSlot(uint32_t slot) {
    volatile uint32_t *tab = sampletab + (size_t) slot * registerbysample;
    tab[kWordFrac] = idleFrac;
    tab[kWordPad] = idlePad;
    tab[kWordFsel0] = fselIdle;
    if (wordFsel2 >= 0)
        tab[wordFsel2] = fsel2Idle;
    if (wordEnv >= 0)
        tab[wordEnv] = idleEnv;
    shadow[slot] = std::complex<float>(0.0f, 0.0f);
    slotIsData[slot] = 0;
}

void SoapyRPITX_IQ_dmaSync::UpdateBehindLimit() {
    if (!dmaRunning.load(std::memory_order_relaxed)) {
        behindLimit = 0;
        return;
    }
    double elapsed = std::chrono::duration<double>(clock::now() - lastPoll).count();
    behindLimit = tracker.readPos() + (uint64_t) std::max(0.0, elapsed * slotRate) + guardSlots(tracker.size(), slotRate);
}

void SoapyRPITX_IQ_dmaSync::WriteDataSlot(uint64_t pos, std::complex<float> sample) {
    const uint32_t slot = tracker.slotOf(pos);
    mydsp.pushsample(sample);
    shadow[slot] = sample;
    slotIsData[slot] = 1;
    envHistory.push(mydsp.amplitude);

    const SoapyRPITX_AmplitudeModel &m = *model;
    const SoapyRPITX_AmplitudeConfig &c = m.config;
    const bool constant = constantEnvelope.load(std::memory_order_relaxed);
    const double raw = envHistory.at(envDelaySlots);
    double a = raw * (double) iqScale.load(std::memory_order_relaxed);
    if (!(a > 0))
        a = 0;

    bool floored = false;
    if (!constant && c.envelopeFloor > 0) {
        const uint32_t hang = std::max<uint32_t>(1, (uint32_t) (kFloorHangSeconds * slotRate));
        if (a < c.envelopeFloor) {
            if (floorCount < hang)
                floorCount++;
        } else {
            floorCount = 0;
        }
        floored = floorCount >= hang;
    }

    // Words of the amplitude path (written envAdvance slots ahead) and of the phase path (this slot).
    uint32_t padWord, fsel0Word, fsel2Word = fsel2Idle, envWord = idleEnv;
    double phase = appliedPhase;
    const uint32_t hyst = (originalPads & PADS_HYST) | PADS_SLEW;

    if (wordEnv >= 0) {
        bool on;
        double code;
        if (constant) {
            on = raw >= mydsp.magnitude_gate;
            code = on ? 1.0 : c.eerIdle;
        } else {
            bool clipped = false;
            on = !floored;
            code = on ? m.eerCode(a, clipped) : c.eerIdle;
            if (clipped)
                envelopeClips.fetch_add(1, std::memory_order_relaxed);
            if (on)
                phase = m.eerPhase(code);
        }
        padWord = on ? ((0x5Au << 24) | hyst | ((uint32_t) c.eerDrive & PADS_DRIVE_MASK)) : idlePad;
        fsel0Word = on ? fselOn : fselIdle;
        if (rfDac)
            fsel2Word = on ? fsel2On : fsel2Idle;
        if (envelopeOutput == SoapyRPITX_EnvelopeOutput::Pcm) {
            uint32_t v = (uint32_t) std::lround(code * 32767.0);
            envWord = (v << 16) | v;
        } else {
            envWord = (uint32_t) std::lround(code * (double) pwmRange);
        }
    } else {
        size_t q;
        if (constant) {
            q = raw >= mydsp.magnitude_gate ? m.top() : 0;
        } else {
            if (a > 1.0)
                envelopeClips.fetch_add(1, std::memory_order_relaxed);
            if (floored) {
                quantizer.reset();
                q = 0;
            } else {
                q = quantizer.quantize(m, a, c.quantizer == SoapyRPITX_Quantizer::SigmaDelta, c.sigmaDeltaOrder);
            }
        }
        const SoapyRPITX_AmpState &st = m.state(q);
        padWord = st.pins != 0 ? ((0x5Au << 24) | hyst | ((uint32_t) st.level & PADS_DRIVE_MASK)) : idlePad;
        fsel0Word = (st.pins & 1u) ? fselOn : fselIdle;
        if (rfDac)
            fsel2Word = (st.pins & 2u) ? fsel2On : fsel2Idle;
        if (st.pins != 0)
            phase = st.phase;
    }

    // AM/PM: the phase change of the output stage is compensated on the carrier (the PLL runs at 1/harmonic).
    phaseCarry += (phase - appliedPhase) / tuneCfg.harmonic;
    appliedPhase = phase;

    // The PLL realises a quantised frequency: the resulting phase error is corrected on the next slots.
    const double target = mydsp.frequency / tuneCfg.harmonic;
    const double correction = std::min(std::max(phaseCarry, -kMaxPhaseStep), kMaxPhaseStep);
    const uint32_t frac = GetMasterFrac(target + correction * carryToHz);
    const double realised = GetMasterFracFrequency(frac);
    phaseCarry += (target - realised) / carryToHz;
    phaseCarry = std::min(std::max(phaseCarry, -kMaxPhaseCarry), kMaxPhaseCarry);

    volatile uint32_t *tab = sampletab + (size_t) slot * registerbysample;
    tab[kWordFrac] = (0x5Au << 24) | frac;

    const bool eer = wordEnv >= 0;
    if (eer) {
        tab[kWordPad] = padWord;
        tab[kWordFsel0] = fsel0Word;
        if (wordFsel2 >= 0)
            tab[wordFsel2] = fsel2Word;
    }

    volatile uint32_t *amp = tab;
    if (envAdvance > 0) {
        // This slot's amplitude words belong to a later sample: until it arrives they hold "off".
        if (eer) {
            tab[wordEnv] = idleEnv;
        } else {
            tab[kWordPad] = idlePad;
            tab[kWordFsel0] = fselIdle;
            if (wordFsel2 >= 0)
                tab[wordFsel2] = fsel2Idle;
        }
        if (pos < (uint64_t) envAdvance || pos - (uint64_t) envAdvance < behindLimit) {
            envelopeLate.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        amp = sampletab + (size_t) tracker.slotOf(pos - (uint64_t) envAdvance) * registerbysample;
    }
    if (eer) {
        amp[wordEnv] = envWord;
    } else {
        amp[kWordPad] = padWord;
        amp[kWordFsel0] = fsel0Word;
        if (wordFsel2 >= 0)
            amp[wordFsel2] = fsel2Word;
    }
}

void SoapyRPITX_IQ_dmaSync::ResetRing() {
    for (uint32_t i = 0; i < buffersize; i++)
        WriteIdleSlot(i);
    tracker.reset();
    lastDataPos = 0;
    phaseCarry = 0;
    idlePrefix = 0;
}

void SoapyRPITX_IQ_dmaSync::StartDma() {
    // The engine restarts at slot 0, where ResetRing() put the read position. The idle prefix written there
    // fills the pacing FIFO before the first data slot.
    if (envelopeOutput == SoapyRPITX_EnvelopeOutput::Pwm)
        pwm.StartTx();
    else
        pcmgpio::StartTx();
    bufferdma::start();
    lastPoll = clock::now();
    badPolls = 0;
    dmaRunning.store(true);
}

void SoapyRPITX_IQ_dmaSync::StopDma() {
    bufferdma::stop();
    if (envelopeOutput == SoapyRPITX_EnvelopeOutput::Pwm)
        pwm.StopTx();
    else
        pcmgpio::StopTx();
    dmaRunning.store(false);
}

void SoapyRPITX_IQ_dmaSync::CarrierOff() {
    clkgpio::gengpio.setmode(RPITX_TX_GPIO, fsel_input);
    if (rfDac)
        clkgpio::gengpio.setmode(RPITX_TX2_GPIO, fsel_input);
    pads.gpioreg[PADS_GPIO_0] = (0x5Au << 24) | originalPads;
}

void SoapyRPITX_IQ_dmaSync::RestoreGpio() {
    clkgpio::gengpio.setmode(RPITX_TX_GPIO, SoapyRPITX_RestoredFunction(RPITX_TX_GPIO, originalGpioFunction));
    for (const auto &pf : originalPins) {
        if (pf.first == RPITX_TX2_GPIO && rfDac)
            clkgpio::gengpio.setmode(RPITX_TX2_GPIO, SoapyRPITX_RestoredFunction(RPITX_TX2_GPIO, pf.second));
    }
    pads.gpioreg[PADS_GPIO_0] = (0x5Au << 24) | originalPads;
}

void SoapyRPITX_IQ_dmaSync::RestoreEnvelopePins() {
    for (const auto &pf : originalPins) {
        if (pf.first == RPITX_TX2_GPIO && rfDac)
            continue;
        clkgpio::gengpio.setmode(pf.first, pf.second);
    }
}

void SoapyRPITX_IQ_dmaSync::Start() {
    if (active.load())
        return;
    if (feeder.joinable())
        feeder.join();

    // Sample the other GPIO 0-9 (and 20-29) functions now: the DMA rewrites the whole GPFSEL0 (and GPFSEL2).
    volatile uint32_t *gp = clkgpio::gengpio.gpioreg;
    uint32_t base = gp[GPFSEL0] & ~(7u << (3 * RPITX_TX_GPIO));
    fselIdle = base | ((uint32_t) fsel_input << (3 * RPITX_TX_GPIO));
    fselOn = base | ((uint32_t) fsel_alt0 << (3 * RPITX_TX_GPIO));
    uint32_t base2 = gp[GPFSEL2] & ~(7u << (3 * (RPITX_TX2_GPIO % 10)));
    fsel2Idle = base2 | ((uint32_t) fsel_input << (3 * (RPITX_TX2_GPIO % 10)));
    fsel2On = base2 | ((uint32_t) fsel_alt5 << (3 * (RPITX_TX2_GPIO % 10)));

    fifo.clear();
    consumedTotal = fifo.pushedTotal();
    drainedPush.store(kNotDrained);
    replay.clear();
    replayIsData.clear();
    replayPos = 0;
    replayLeft.store(0);
    endBurst.store(false);
    underflows.store(0);
    late.store(0);
    dmaErrors.store(0);
    samplesSent.store(0);
    envelopeClips.store(0);
    envelopeLate.store(0);
    inBurst = false;
    firstQueuedValid = false;
    badPolls = 0;
    mydsp.reset();
    appliedPhase = 0;
    RefreshModel();
    ResetAmplitudeState();
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
    drainedPush.store(kNotDrained);
    StopDma();
    RestoreGpio();
    ResetRing();
    fifo.clear();
    replay.clear();
    replayIsData.clear();
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

    // A new burst starts: the previous burst end no longer applies to the samples queued from now on.
    endBurst.store(false);

    const auto deadline = SoapyRPITX_DeadlineAfter(timeoutUs);
    size_t done = 0;

    while (done < n) {
        done += fifo.push(samples + done, n - done);
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
    const auto deadline = SoapyRPITX_DeadlineAfter(timeoutUs);
    while (active.load()) {
        // The feeder publishes the push count up to which everything has been played; samples pushed later
        // make the counts differ.
        if (drainedPush.load() == fifo.pushedTotal())
            return true;
        if (clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

SoapyRPITX_RetuneResult SoapyRPITX_IQ_dmaSync::Retune(const SoapyRPITX_TuneConfig &tune) {
    if (!running.load()) {
        // Feeder not running: the DMA is stopped and the hardware can be reprogrammed directly.
        if (!ApplyTune(tune))
            return SoapyRPITX_RetuneResult::Refused;
        ResetRing();
        return SoapyRPITX_RetuneResult::Ok;
    }

    std::unique_lock<std::mutex> lock(tuneMutex);
    const uint64_t seq = ++tuneRequestSeq;
    pendingTune = tune;
    tunePending = true;

    tuneCv.wait_for(lock, kRetuneTimeout, [&] { return tuneTakenSeq >= seq || !running.load(); });
    if (tuneTakenSeq < seq) {
        // Not taken in time: withdraw the request, nothing was changed.
        tunePending = false;
        return SoapyRPITX_RetuneResult::Timeout;
    }
    tuneCv.wait(lock, [&] { return tuneDoneSeq >= seq || feederExited.load(); });
    return tuneDoneSeq >= seq && tuneResult ? SoapyRPITX_RetuneResult::Ok : SoapyRPITX_RetuneResult::Refused;
}

void SoapyRPITX_IQ_dmaSync::SetDriveLevel(int level) {
    driveLevel.store(std::min(std::max(level, 0), 7));
    PublishModel();
}

void SoapyRPITX_IQ_dmaSync::SetIqScale(float scale) {
    iqScale.store(scale > 0 ? scale : 0.0f);
}

void SoapyRPITX_IQ_dmaSync::SetConstantEnvelope(bool constant) {
    constantEnvelope.store(constant);
}

bool SoapyRPITX_IQ_dmaSync::SetAmplitudeConfig(const SoapyRPITX_AmplitudeConfig &amplitude) {
    if (!SoapyRPITX_CheckAmplitudeConfig(amplitude).empty() || !advanceFits(amplitude, slotsPerSample, envelopeOutput, tracker.size()))
        return false;
    {
        std::lock_guard<std::mutex> lock(modelMutex);
        ampCfg = amplitude;
    }
    PublishModel();
    return true;
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
    stats.envelopeClips = envelopeClips.load();
    stats.envelopeLate = envelopeLate.load();
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

uint32_t SoapyRPITX_IQ_dmaSync::SlotsPerSample() const {
    return slotsPerSample;
}

double SoapyRPITX_IQ_dmaSync::GetActualSampleRate() const {
    return actualSampleRate.load();
}

bool SoapyRPITX_IQ_dmaSync::IsPllLocked() {
    return clkgpio::IsPllcLocked();
}

void SoapyRPITX_IQ_dmaSync::SignalSafeStop() {
    dma_reg.gpioreg[DMA_CS + channel * DMA_CHANNEL_WORDS] = BCM2708_DMA_RESET;
    volatile uint32_t *fsel = clkgpio::gengpio.gpioreg;
    fsel[GPFSEL0] = (fsel[GPFSEL0] & ~(7u << (3 * RPITX_TX_GPIO))) | ((uint32_t) fsel_input << (3 * RPITX_TX_GPIO));
    if (rfDac)
        fsel[GPFSEL2] = (fsel[GPFSEL2] & ~(7u << (3 * (RPITX_TX2_GPIO % 10)))) | ((uint32_t) fsel_input << (3 * (RPITX_TX2_GPIO % 10)));
    pads.gpioreg[PADS_GPIO_0] = (0x5Au << 24) | originalPads;
    clkgpio::gpioreg[GPCLK_CNTL] = CM_PASSWD | ((uint32_t) clkgpio::Mash << CM_MASH_SHIFT) | (uint32_t) clkgpio::pllnumber;
    if (envelopeOutput == SoapyRPITX_EnvelopeOutput::Pcm)
        pcmgpio::gpioreg[PCM_CS_A] = pcmgpio::gpioreg[PCM_CS_A] & ~(PCM_CS_TXON | PCM_CS_DMAEN);
    else if (envelopeOutput == SoapyRPITX_EnvelopeOutput::Pwm)
        pwm.gpioreg[PWM_CTL] = 0;
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
    uint64_t fromFifo = 0;
    const uint32_t k = slotsPerSample;

    for (;;) {
        uint32_t w = tracker.writable(target);
        if (w == 0)
            break;
        UpdateBehindLimit();

        uint64_t pos = tracker.writePos();
        if (replayPos < replay.size()) {
            // Replayed entries are slot samples (one per slot) or idle slots of a gap inside a burst.
            const size_t m = std::min<size_t>(std::min<size_t>(w, chunk.size()), replay.size() - replayPos);
            for (size_t i = 0; i < m; i++) {
                if (replayIsData[replayPos + i]) {
                    WriteDataSlot(pos + i, replay[replayPos + i]);
                    lastDataPos = pos + i + 1;
                } else {
                    WriteIdleSlot(tracker.slotOf(pos + i));
                    if (i == 0 ? (replayPos > 0 && replayIsData[replayPos - 1]) : replayIsData[replayPos + i - 1])
                        ResetAmplitudeState();
                }
            }
            replayPos += m;
            tracker.advanceWrite((uint32_t) m);
            total += (uint32_t) m;

            if (replayPos == replay.size()) {
                replay.clear();
                replayIsData.clear();
                replayPos = 0;
            }
            replayLeft.store(replay.size() - replayPos);
            continue;
        }

        const size_t want = std::min<size_t>(w / k, chunk.size());
        if (want == 0)
            break;
        const size_t m = fifo.pop(chunk.data(), want);
        if (m == 0)
            break;

        for (size_t i = 0; i < m; i++) {
            const std::complex<float> x = chunk[i];
            if (k == 1) {
                WriteDataSlot(pos++, x);
            } else {
                // Linear interpolation from the previous sample: the last slot of the group is the sample itself.
                for (uint32_t j = 1; j <= k; j++)
                    WriteDataSlot(pos++, j == k ? x : prevIq + (x - prevIq) * ((float) j / (float) k));
            }
            prevIq = x;
        }
        tracker.advanceWrite((uint32_t) (m * k));
        total += (uint32_t) (m * k);
        fromFifo += m;

        lastDataPos = tracker.writePos();
        consumedTotal += m;
    }

    if (fromFifo > 0) {
        samplesSent.fetch_add(fromFifo);
        if (waitingProducers.load() > 0)
            spaceCv.notify_all();
    }
    return total;
}

uint32_t SoapyRPITX_IQ_dmaSync::PadIdle(uint32_t target) {
    uint32_t w = tracker.writable(target);
    uint64_t pos = tracker.writePos();
    for (uint32_t i = 0; i < w; i++)
        WriteIdleSlot(tracker.slotOf(pos + i));
    tracker.advanceWrite(w);
    return w;
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

bool SoapyRPITX_IQ_dmaSync::FeederRetune(const SoapyRPITX_TuneConfig &request, bool engineStopped) {
    // Nothing published as drained may change while the ring is rebuilt.
    drainedPush.store(kNotDrained);

    const bool wasRunning = dmaRunning.load();
    if (wasRunning) {
        // The engine is paused before its position is read, so it cannot execute more slots meanwhile.
        const bool stopped = engineStopped || !bufferdma::isrunning();
        bufferdma::pause();
        int slot = GetCurrentSample();
        if (slot >= 0) {
            auto now = clock::now();
            // An engine that stopped by itself cannot have completed laps since the last poll.
            const double expected = stopped ? 0.0 : std::chrono::duration<double>(now - lastPoll).count() * slotRate;
            tracker.update((uint32_t) slot, expected);
            lastPoll = now;
        }
    }
    StopDma();
    CarrierOff();

    // Keep the data slots that were written to the ring but not played: they are sent first after the retune.
    // The slot that was executing is included, so at most one sample is sent twice and none is lost. Idle
    // slots before the first data slot are dropped; idle slots between data slots are kept as idle.
    std::vector<std::complex<float>> pending;
    std::vector<uint8_t> pendingIsData;
    uint64_t first = tracker.readPos();
    const uint64_t end = lastDataPos;
    if (end > first && end - first <= tracker.size()) {
        while (first < end && !slotIsData[tracker.slotOf(first)])
            first++;
        pending.reserve((size_t) (end - first) + (replay.size() - replayPos));
        pendingIsData.reserve(pending.capacity());
        for (uint64_t p = first; p < end; p++) {
            const uint32_t slot = tracker.slotOf(p);
            pending.push_back(shadow[slot]);
            pendingIsData.push_back(slotIsData[slot]);
        }
    }
    pending.insert(pending.end(), replay.begin() + (std::ptrdiff_t) replayPos, replay.end());
    pendingIsData.insert(pendingIsData.end(), replayIsData.begin() + (std::ptrdiff_t) replayPos, replayIsData.end());
    replay.swap(pending);
    replayIsData.swap(pendingIsData);
    replayPos = 0;
    replayLeft.store(replay.size());

    bool ok = ApplyTune(request);
    ResetRing();
    inBurst = false;
    firstQueuedValid = false;
    badPolls = 0;
    return ok;
}

void SoapyRPITX_IQ_dmaSync::PublishDrainState() {
    // The push count is read after the FIFO was consumed in this period: equal counts mean that every sample
    // pushed so far is in the ring, and the ring position tells whether its last data slot has completed.
    const uint64_t pushed = fifo.pushedTotal();
    const bool drained = consumedTotal == pushed && replayLeft.load() == 0 && tracker.readPos() >= lastDataPos;
    drainedPush.store(drained ? pushed : kNotDrained);
}

void SoapyRPITX_IQ_dmaSync::RecoverDma() {
    dmaErrors.fetch_add(1);
    SoapySDR_logf(SOAPY_SDR_ERROR, "rpitx: the DMA engine stopped (CS 0x%08x, DEBUG 0x%08x); restarting it", GetStatus(), GetDebugStatus());
    const SoapyRPITX_TuneConfig current = tuneCfg;
    FeederRetune(current, true);
}

void SoapyRPITX_IQ_dmaSync::FeederLoop() {
    SetupFeederThread();

    auto next = clock::now();

    while (running.load()) {
        RefreshModel();

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
            bool ok = FeederRetune(request, false);
            {
                std::lock_guard<std::mutex> lock(tuneMutex);
                tuneResult = ok;
                tuneDoneSeq = seq;
            }
            tuneCv.notify_all();
        }

        const auto now = clock::now();
        const uint32_t n = tracker.size();
        const uint32_t rate = slotRate;
        const uint32_t k = slotsPerSample;
        const uint32_t guard = guardSlots(n, rate);
        const uint32_t target = n * 3 / 4;
        // Idle padding keeps enough slots ahead of the engine for the envelope words written ahead.
        const uint32_t low = std::min<uint32_t>(target, std::max<uint32_t>(std::max<uint32_t>(n / 8, rate / 200), (uint32_t) envAdvance + guard + 16));

        if (!dmaRunning.load()) {
            // Idle slots at the start of the ring fill the pacing FIFO when the engine starts (plus one slot that
            // lasts until the first frame boundary) and leave room for the envelope words of the first samples.
            if (tracker.writePos() == 0 && idlePrefix == 0) {
                idlePrefix = pacingLevel + 1 + (uint32_t) envAdvance;
                PadIdle(idlePrefix);
            }
            PumpData(target);

            const int64_t ringData = std::max<int64_t>(tracker.fill() - (int64_t) idlePrefix, 0);
            const uint64_t queued = (uint64_t) fifo.size() * k + replayLeft.load() + (uint64_t) ringData;
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

                ScrubCompleted();
                if (PumpData(target) > 0)
                    inBurst = true;

                if (fifo.size() == 0 && replayLeft.load() == 0 && tracker.fill() < (int64_t) low) {
                    if (inBurst) {
                        if (!endBurst.load())
                            underflows.fetch_add(1);
                        inBurst = false;
                    }
                    if (PadIdle(low) > 0)
                        ResetAmplitudeState();
                }
            }
        }

        PublishDrainState();

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
