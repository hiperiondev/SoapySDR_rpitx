// Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
// Project Site: https://github.com/hiperiondev/SoapySDR_rpitx
//
// This is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// Integration test of SoapyRPITX_IQ_dmaSync on simulated hardware (tests/mock_hw.cpp): the real feeder
// thread, FIFO, ring accounting, pre-roll, idle padding, drain, retune (carrier off, replay of the ring),
// GPIO and pad restore, emergency stop, DMA restart, DMA channel selection, mailbox failures and the
// session lock and recovery are exercised against a DMA engine that plays the ring at the sample rate.

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <cmath>
#include <complex>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "SoapyRPITX_IQ_dmaSync.hpp"
#include "mailbox.hpp"
#include "mock_hw.hpp"

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                                                            \
    do {                                                                                                                                                       \
        if (!(cond)) {                                                                                                                                         \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                                                      \
            failures++;                                                                                                                                        \
        }                                                                                                                                                      \
    } while (0)

constexpr uint32_t kRate = 48000;

std::vector<std::complex<float>> tone(size_t n, size_t &phaseIndex) {
    std::vector<std::complex<float>> v(n);
    for (size_t i = 0; i < n; i++, phaseIndex++) {
        double ph = 2.5 * std::sin(2 * M_PI * 1000.0 * (double) phaseIndex / kRate);
        v[i] = std::complex<float>((float) std::cos(ph), (float) std::sin(ph));
    }
    return v;
}

// Number of carrier-off slots between the first and the last carrier-on slot.
size_t holes(const std::vector<MockSlot> &s) {
    size_t first = s.size(), last = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i].carrierOn) {
            first = std::min(first, i);
            last = i;
        }
    }
    size_t h = 0;
    for (size_t i = first; i < last; i++)
        if (!s[i].carrierOn)
            h++;
    return h;
}

size_t carrierOn(const std::vector<MockSlot> &s) {
    size_t n = 0;
    for (const MockSlot &x : s)
        n += x.carrierOn ? 1 : 0;
    return n;
}

void testStreaming() {
    SoapyRPITX_TxConfig cfg;
    cfg.tune.frequency = 144.8e6;
    cfg.tune.sampleRate = kRate;
    cfg.ringSize = 4096;
    cfg.fifoMs = 250;
    cfg.prerollMs = 100;
    cfg.rtPriority = 0;
    cfg.constantEnvelope = true;

    SoapyRPITX_IQ_dmaSync tx(cfg);
    MockDmaEngine engine(14, kRate);

    // Only one transmitter per process.
    bool second = false;
    try {
        SoapyRPITX_IQ_dmaSync other(cfg);
    } catch (const std::runtime_error &) {
        second = true;
    }
    CHECK(second);

    tx.Start();
    CHECK(tx.IsActive());

    // Writing faster than real time: the FIFO applies back-pressure, every call completes within its timeout.
    size_t phase = 0;
    const size_t total = kRate; // one second
    size_t written = 0;
    while (written < total) {
        auto block = tone(750, phase);
        size_t n = tx.Write(block.data(), block.size(), 1000000);
        CHECK(n == block.size());
        written += n;
    }
    tx.EndBurst();
    CHECK(tx.WaitDrained(2000000));

    SoapyRPITX_TxStats st = tx.GetStats();
    CHECK(st.samplesSent == total);
    CHECK(st.underflows == 0);
    CHECK(st.late == 0);
    CHECK(st.dmaRunning);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::vector<MockSlot> played = engine.Take();
    CHECK(carrierOn(played) == total);
    CHECK(holes(played) == 0);
    // After the burst the carrier is off (idle padding), not a replay of the ring, and the pads of the other
    // GPIOs are back to their original drive strength.
    CHECK(!played.empty() && !played.back().carrierOn);
    CHECK(!played.empty() && played.back().pad == (kDefaultPads & 7));

    // Host starvation in the middle of a burst is counted and produces silence, not stale audio.
    auto block = tone(2400, phase);
    tx.Write(block.data(), block.size(), 1000000);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    block = tone(2400, phase);
    tx.Write(block.data(), block.size(), 1000000);
    tx.EndBurst();
    CHECK(tx.WaitDrained(2000000));
    st = tx.GetStats();
    CHECK(st.underflows >= 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    played = engine.Take();
    CHECK(carrierOn(played) == 4800);

    // Non-blocking write on a full FIFO returns a partial count instead of hanging.
    std::vector<std::complex<float>> big(kRate * 2, std::complex<float>(1.0f, 0.0f));
    size_t n = tx.Write(big.data(), big.size(), 0);
    CHECK(n > 0 && n < big.size());

    // Retune while streaming.
    SoapyRPITX_TuneConfig t = tx.GetTune();
    t.frequency = 145.5e6;
    CHECK(tx.Retune(t) == SoapyRPITX_RetuneResult::Ok);
    CHECK(tx.GetTune().frequency == 145.5e6);

    // An unreachable frequency is rejected and the previous one stays.
    t.frequency = 5e9;
    CHECK(tx.Retune(t) == SoapyRPITX_RetuneResult::Refused);
    CHECK(tx.GetTune().frequency == 145.5e6);

    tx.Stop();
    CHECK(!tx.IsActive());
    CHECK(!tx.GetStats().dmaRunning);

    // GPIO4 back to its original function (input in the simulation).
    volatile uint32_t *gpio = MockRegisters(GENERAL_BASE);
    CHECK(gpio != nullptr && ((gpio[GPFSEL0] >> 12) & 7) == fsel_input);
    volatile uint32_t *padRegs = MockRegisters(PADS_GPIO);
    CHECK(padRegs != nullptr && (padRegs[PADS_GPIO_0] & 0x1F) == kDefaultPads);

    // Restart after stop.
    tx.Start();
    block = tone(9600, phase);
    CHECK(tx.Write(block.data(), block.size(), 1000000) == block.size());
    tx.EndBurst();
    CHECK(tx.WaitDrained(2000000));
    tx.Stop();

    // Writes on a stopped transmitter are rejected immediately.
    CHECK(tx.Write(block.data(), block.size(), 1000000) == 0);
}

SoapyRPITX_TxConfig baseConfig() {
    SoapyRPITX_TxConfig cfg;
    cfg.tune.frequency = 144.8e6;
    cfg.tune.sampleRate = kRate;
    cfg.prerollMs = 20;
    cfg.rtPriority = 0;
    cfg.constantEnvelope = true;
    return cfg;
}

bool throws(const SoapyRPITX_TxConfig &cfg) {
    try {
        SoapyRPITX_IQ_dmaSync tx(cfg);
    } catch (const std::runtime_error &) {
        return true;
    }
    return false;
}

void testRetuneCarrierOffAndReplay() {
    SoapyRPITX_IQ_dmaSync tx(baseConfig());
    MockDmaEngine engine(14, kRate);
    tx.Start();

    const size_t total = kRate / 2;
    size_t phase = 0;
    auto block = tone(total, phase);
    CHECK(tx.Write(block.data(), block.size(), 1000000) == total);
    tx.EndBurst();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Watch PLLC_CTRL while the PLL is reprogrammed: GPIO4 must not be on the clock output meanwhile.
    std::atomic<bool> stop{false};
    std::atomic<int> unsafe{0};
    std::thread monitor([&] {
        uint32_t last = MockPeek(CLK_BASE, PLLC_CTRL);
        while (!stop.load()) {
            uint32_t c = MockPeek(CLK_BASE, PLLC_CTRL);
            if (c != last) {
                last = c;
                if (((MockPeek(GENERAL_BASE, GPFSEL0) >> 12) & 7) == fsel_alt0)
                    unsafe++;
            }
        }
    });
    SoapyRPITX_TuneConfig t = tx.GetTune();
    t.frequency = 50.0e6;
    CHECK(tx.Retune(t) == SoapyRPITX_RetuneResult::Ok);
    stop.store(true);
    monitor.join();
    CHECK(unsafe.load() == 0);

    // Every sample of the burst is transmitted once (the slot executing at the retune may be sent twice).
    CHECK(tx.WaitDrained(3000000));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    size_t on = carrierOn(engine.Take());
    CHECK(on >= total && on <= total + 1);
    tx.Stop();
}

void testRefusedRetuneKeepsTransmitting() {
    SoapyRPITX_IQ_dmaSync tx(baseConfig());
    MockDmaEngine engine(14, kRate);
    tx.Start();

    std::vector<std::complex<float>> block(kRate / 2, std::complex<float>(1.0f, 0.0f));
    CHECK(tx.Write(block.data(), block.size(), 1000000) == block.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(tx.GetStats().dmaRunning);

    // The carrier must stay on the pin and PLLC must not be reprogrammed while refused requests arrive.
    std::atomic<bool> stop{false};
    std::atomic<int> disturbed{0};
    const uint32_t pllc = MockPeek(CLK_BASE, PLLC_CTRL);
    std::thread monitor([&] {
        while (!stop.load()) {
            if (((MockPeek(GENERAL_BASE, GPFSEL0) >> 12) & 7) != fsel_alt0 || MockPeek(CLK_BASE, PLLC_CTRL) != pllc)
                disturbed++;
        }
    });
    SoapyRPITX_TuneConfig t = tx.GetTune();
    const double frequency = t.frequency;
    for (double bad : {5e9, 1e3, -1.0}) {
        t.frequency = bad;
        CHECK(tx.Retune(t) == SoapyRPITX_RetuneResult::Refused);
    }
    t = tx.GetTune();
    t.sampleRate = 1; // out of range → pacing clock / slot rate refuses
    CHECK(tx.Retune(t) == SoapyRPITX_RetuneResult::PacingRefused);
    t = tx.GetTune();
    t.harmonic = 0;
    CHECK(tx.Retune(t) == SoapyRPITX_RetuneResult::Refused);
    stop.store(true);
    monitor.join();
    CHECK(disturbed.load() == 0);
    CHECK(tx.GetStats().dmaRunning);
    CHECK(tx.GetTune().frequency == frequency);

    // A reachable request still works afterwards.
    t = tx.GetTune();
    t.frequency = 50.0e6;
    CHECK(tx.Retune(t) == SoapyRPITX_RetuneResult::Ok);
    CHECK(tx.GetTune().frequency == 50.0e6);
    tx.Stop();
}

void testEmergencyStopThenDestroy() {
    {
        SoapyRPITX_IQ_dmaSync tx(baseConfig());
        MockDmaEngine engine(14, kRate);
        tx.Start();
        std::vector<std::complex<float>> b(kRate, std::complex<float>(1.0f, 0.0f));
        tx.Write(b.data(), b.size(), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        tx.EmergencyStop();
        CHECK(!tx.GetStats().dmaRunning);
        volatile uint32_t *gpio = MockRegisters(GENERAL_BASE);
        CHECK(((gpio[GPFSEL0] >> 12) & 7) == fsel_input);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(!tx.GetStats().dmaRunning);
    } // the destructor completes the shutdown (it must not terminate the process)
}

void testConstructorFailureRestores() {
    size_t live = MockLiveAllocations();
    SoapyRPITX_TxConfig cfg = baseConfig();
    cfg.tune.frequency = 5e9;
    CHECK(throws(cfg));
    volatile uint32_t *padRegs = MockRegisters(PADS_GPIO);
    CHECK(padRegs != nullptr && (padRegs[PADS_GPIO_0] & 0x1F) == kDefaultPads);
    CHECK(MockLiveAllocations() == live);

    // A refused VideoCore allocation is reported and releases everything.
    MockFailNextAlloc();
    CHECK(throws(baseConfig()));
    CHECK(MockLiveAllocations() == live);

    // A ring shorter than 10 ms is refused.
    cfg = baseConfig();
    cfg.tune.sampleRate = 96000;
    cfg.ringSize = 512;
    CHECK(throws(cfg));

    // The claim was released by every failure.
    CHECK(!throws(baseConfig()));
}

void testDmaChannelSelection() {
    // The kernel owns channel 14: an explicit request is refused, the default moves to 13.
    MockSetKernelDmaMask(1u << 14, true);
    SoapyRPITX_TxConfig cfg = baseConfig();
    cfg.dmaChannel = 14;
    CHECK(throws(cfg));
    {
        SoapyRPITX_IQ_dmaSync tx(baseConfig());
        CHECK(tx.GetChannel() == 13);
    }

    // Without the kernel mask the channel is not guessed: only an explicit channel is accepted.
    MockSetKernelDmaMask(0, false);
    CHECK(throws(baseConfig()));
    cfg = baseConfig();
    cfg.dmaChannel = 14;
    CHECK(!throws(cfg));
    MockSetKernelDmaMask(0, true);

    // An active channel is only reset when forced.
    volatile uint32_t *dmaRegs = MockRegisters(DMA_BASE);
    CHECK(dmaRegs != nullptr);
    dmaRegs[DMA_CS + 12 * DMA_CHANNEL_WORDS] = DMA_CS_ACTIVE;
    cfg.dmaChannel = 12;
    CHECK(throws(cfg));
    cfg.dmaForce = true;
    {
        SoapyRPITX_IQ_dmaSync tx(cfg);
        CHECK(tx.GetChannel() == 12);
        CHECK((dmaRegs[DMA_CS + 12 * DMA_CHANNEL_WORDS] & DMA_CS_ACTIVE) == 0);
    }

    // Default selection skips active channels.
    dmaRegs[DMA_CS + 14 * DMA_CHANNEL_WORDS] = DMA_CS_ACTIVE;
    {
        SoapyRPITX_IQ_dmaSync tx(baseConfig());
        CHECK(tx.GetChannel() == 13);
    }
    dmaRegs[DMA_CS + 14 * DMA_CHANNEL_WORDS] = 0;
}

void testSessionLockAndRecovery() {
    // Only one session at a time, even within the process (flock is per open file).
    {
        SoapyRPITX_Session first(RPITX_TX_GPIO);
        bool refused = false;
        try {
            SoapyRPITX_Session second(RPITX_TX_GPIO);
        } catch (const std::runtime_error &) {
            refused = true;
        }
        CHECK(refused);
        CHECK(throws(baseConfig()));
    }

    // A record left by a terminated process: the DMA channel is reset, GPIO4 is switched off and the
    // VideoCore memory is released when the next transmitter opens.
    volatile uint32_t *dmaRegs = MockRegisters(DMA_BASE);
    volatile uint32_t *gpio = MockRegisters(GENERAL_BASE);
    unsigned leaked = MockAllocate(8192);
    dmaRegs[DMA_CS + 11 * DMA_CHANNEL_WORDS] = DMA_CS_ACTIVE;
    gpio[GPFSEL0] = (gpio[GPFSEL0] & ~(7u << 12)) | ((uint32_t) fsel_alt0 << 12);

    SoapyRPITX_SessionRecord r;
    r.pid = 999999;
    r.dmaChannel = 11;
    r.memHandle = leaked;
    r.memBus = mem_lock(0, leaked);
    r.memSize = 8192;
    dmaRegs[DMA_CONBLK_AD + 11 * DMA_CHANNEL_WORDS] = r.memBus + 5 * 32; // still running our chain
    r.pads = kDefaultPads;
    r.gpioFunction = fsel_input;
    {
        std::ofstream out(SoapyRPITX_Session::LockPath(), std::ios::trunc);
        out << SoapyRPITX_FormatRecord(r);
    }
    SoapyRPITX_SessionRecord parsed;
    CHECK(SoapyRPITX_ParseRecord(SoapyRPITX_FormatRecord(r), parsed) && parsed.memHandle == leaked && parsed.dmaChannel == 11);

    // The EMMC2 pair of the clock tree survives the session record; a record without it (older version) still parses.
    {
        SoapyRPITX_SessionRecord t;
        t.pid = 4242;
        t.clockTree.valid = true;
        t.clockTree.pllcCtrl = (1u << 12) | 52;
        t.clockTree.emmc2Valid = true;
        t.clockTree.emmc2Ctl = 0x15; // PLLC_PER, enabled
        t.clockTree.emmc2Div = 0x4000;
        SoapyRPITX_SessionRecord back;
        CHECK(SoapyRPITX_ParseRecord(SoapyRPITX_FormatRecord(t), back) && back.clockTree.valid && back.clockTree.emmc2Valid &&
              back.clockTree.emmc2Ctl == 0x15 && back.clockTree.emmc2Div == 0x4000);
        t.clockTree.emmc2Valid = false;
        CHECK(SoapyRPITX_ParseRecord(SoapyRPITX_FormatRecord(t), back) && back.clockTree.valid && !back.clockTree.emmc2Valid);
    }

    {
        SoapyRPITX_IQ_dmaSync tx(baseConfig());
        CHECK(!MockAllocated(leaked));
        CHECK((dmaRegs[DMA_CS + 11 * DMA_CHANNEL_WORDS] & DMA_CS_ACTIVE) == 0);
        CHECK(((gpio[GPFSEL0] >> 12) & 7) == fsel_input);

        // While the transmitter exists its own record is in the lock file.
        std::ifstream in(SoapyRPITX_Session::LockPath());
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        SoapyRPITX_SessionRecord own;
        CHECK(SoapyRPITX_ParseRecord(text, own) && own.pid == (long) getpid() && own.dmaChannel == tx.GetChannel() && own.clockTree.valid);
    }

    // A clean close leaves an empty lock file.
    std::ifstream in(SoapyRPITX_Session::LockPath());
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.empty());
}

void testDmaRestartAndPads() {
    SoapyRPITX_TxConfig cfg = baseConfig();
    cfg.constantEnvelope = false;
    SoapyRPITX_IQ_dmaSync tx(cfg);
    MockDmaEngine engine(14, kRate);
    tx.Start();

    size_t phase = 0;
    auto block = tone(9600, phase);
    CHECK(tx.Write(block.data(), block.size(), 1000000) == block.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    // The engine stops by itself: the feeder notices it and restarts.
    MockPokeBits(DMA_BASE, DMA_CS + 14 * DMA_CHANNEL_WORDS, DMA_CS_ACTIVE, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(tx.GetStats().dmaErrors >= 1);

    block = tone(4800, phase);
    CHECK(tx.Write(block.data(), block.size(), 1000000) == block.size());
    tx.EndBurst();
    CHECK(tx.WaitDrained(3000000));
    CHECK(tx.GetStats().dmaRunning);

    // Data slots keep the input hysteresis of GPIO 0-27.
    std::vector<MockSlot> played = engine.Take();
    bool hyst = !played.empty();
    for (const MockSlot &x : played)
        hyst = hyst && x.hysteresis;
    CHECK(hyst);

    // The FIFO capacity follows the sample rate.
    size_t before = tx.FifoCapacity();
    SoapyRPITX_TuneConfig t = tx.GetTune();
    t.sampleRate = 2 * kRate;
    CHECK(tx.Retune(t) == SoapyRPITX_RetuneResult::Ok);
    CHECK(tx.FifoCapacity() == 2 * before);
    CHECK(std::fabs(tx.GetActualSampleRate() - 2 * kRate) < 2 * kRate * 1e-4);
    tx.Stop();
}

void testSignalCleanupHandlers() {
    struct sigaction before;
    sigaction(SIGTERM, nullptr, &before);
    {
        SoapyRPITX_TxConfig cfg = baseConfig();
        cfg.signalCleanup = true;
        SoapyRPITX_IQ_dmaSync tx(cfg);
        struct sigaction during;
        sigaction(SIGTERM, nullptr, &during);
        CHECK((during.sa_flags & SA_SIGINFO) != 0);
    }
    struct sigaction after;
    sigaction(SIGTERM, nullptr, &after);
    CHECK(after.sa_handler == before.sa_handler && (after.sa_flags & SA_SIGINFO) == (before.sa_flags & SA_SIGINFO));
}

void testShortBurstStarts() {
    // A burst shorter than the pre-roll still goes out once the pre-roll time has elapsed.
    SoapyRPITX_TxConfig cfg;
    cfg.tune.sampleRate = kRate;
    cfg.prerollMs = 100;
    cfg.rtPriority = 0;
    cfg.constantEnvelope = true;

    SoapyRPITX_IQ_dmaSync tx(cfg);
    MockDmaEngine engine(14, kRate);
    tx.Start();
    size_t phase = 0;
    auto block = tone(500, phase);
    CHECK(tx.Write(block.data(), block.size(), 100000) == block.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(tx.GetStats().samplesSent == 500);
    CHECK(tx.WaitDrained(500000));
    tx.Stop();
    CHECK(carrierOn(engine.Take()) == 500);
}

std::vector<std::complex<float>> level(size_t n, float mag, float phase = 0.0f) {
    return std::vector<std::complex<float>>(n, std::polar(mag, phase));
}

// Sends one burst, waits for it and returns the recorded frames.
std::vector<MockSlot> sendBurst(SoapyRPITX_IQ_dmaSync &tx, MockDmaEngine &engine, const std::vector<std::complex<float>> &samples) {
    engine.Take();
    tx.Start();
    CHECK(tx.Write(samples.data(), samples.size(), 5000000) == samples.size());
    tx.EndBurst();
    CHECK(tx.WaitDrained(5000000));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    tx.Stop();
    return engine.Take();
}

// Frames from the first to the last frame with a clock pin on.
std::vector<MockSlot> dataFrames(const std::vector<MockSlot> &s) {
    size_t first = s.size(), last = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i].carrierOn || s[i].carrier2On) {
            first = std::min(first, i);
            last = i;
        }
    }
    if (first > last)
        return {};
    return std::vector<MockSlot>(s.begin() + (std::ptrdiff_t) first, s.begin() + (std::ptrdiff_t) last + 1);
}

SoapyRPITX_TxConfig envelopeConfig() {
    SoapyRPITX_TxConfig cfg = baseConfig();
    cfg.constantEnvelope = false;
    cfg.prerollMs = 50;
    return cfg;
}

uint32_t mostFrequent(const std::vector<uint32_t> &v) {
    std::map<uint32_t, size_t> count;
    for (uint32_t x : v)
        count[x]++;
    uint32_t best = 0;
    size_t n = 0;
    for (const auto &c : count)
        if (c.second > n) {
            best = c.first;
            n = c.second;
        }
    return best;
}

void testPacingStart() {
    // The DMA start runs through idle slots only: every data sample lasts exactly one frame.
    SoapyRPITX_IQ_dmaSync tx(envelopeConfig());
    MockDmaEngine engine(14, kRate);
    volatile uint32_t *pcm = MockRegisters(PCM_BASE);
    CHECK(pcm != nullptr && ((pcm[PCM_DREQ_A] >> 8) & 0x7F) == PCM_TX_DREQ_LEVEL);
    for (int burst = 0; burst < 3; burst++) {
        std::vector<MockSlot> played = sendBurst(tx, engine, level(1000, 1.0f));
        CHECK(carrierOn(played) == 1000);
        CHECK(holes(played) == 0);
    }
}

void testEnvelopeDelayFrames() {
    // Step from 0.25 (level 1) to 1.0 (level 7): with the half-sample envelope delay one frame shows the
    // midpoint 0.625 (level 4); without delay the level jumps directly.
    SoapyRPITX_TxConfig cfg = envelopeConfig();
    SoapyRPITX_IQ_dmaSync tx(cfg);
    MockDmaEngine engine(14, kRate);
    std::vector<std::complex<float>> burst = level(2400, 0.25f);
    std::vector<std::complex<float>> high = level(2400, 1.0f);
    burst.insert(burst.end(), high.begin(), high.end());

    auto countPad = [](const std::vector<MockSlot> &s, uint32_t p) {
        size_t n = 0;
        for (const MockSlot &x : s)
            n += (x.carrierOn && x.pad == p) ? 1 : 0;
        return n;
    };
    std::vector<MockSlot> played = sendBurst(tx, engine, burst);
    CHECK(countPad(played, 4) == 1);
    CHECK(countPad(played, 1) >= 2398 && countPad(played, 7) >= 2398);

    cfg.amplitude.envelopeDelay = 0;
    CHECK(tx.SetAmplitudeConfig(cfg.amplitude));
    played = sendBurst(tx, engine, burst);
    CHECK(countPad(played, 4) == 0);

    // A delay that needs more look-ahead than the ring allows is refused.
    cfg.amplitude.envelopeDelay = -RPITX_MAX_ENVELOPE_DELAY;
    SoapyRPITX_AmplitudeConfig big = cfg.amplitude;
    CHECK(tx.SetAmplitudeConfig(big) == (32u <= tx.RingSize() / 8));
}

void testSigmaDeltaAverage() {
    SoapyRPITX_TxConfig cfg = envelopeConfig();
    cfg.amplitude.quantizer = SoapyRPITX_Quantizer::SigmaDelta;
    SoapyRPITX_IQ_dmaSync tx(cfg);
    MockDmaEngine engine(14, kRate);

    auto meanAmplitude = [](const std::vector<MockSlot> &s) {
        std::vector<MockSlot> d = dataFrames(s);
        double sum = 0;
        for (const MockSlot &x : d)
            sum += x.carrierOn ? (x.pad + 1) / 8.0 : 0.0;
        return d.empty() ? 0.0 : sum / (double) d.size();
    };
    CHECK(std::fabs(meanAmplitude(sendBurst(tx, engine, level(9600, 0.3f))) - 0.3) < 0.005);

    cfg.amplitude.quantizer = SoapyRPITX_Quantizer::Nearest;
    CHECK(tx.SetAmplitudeConfig(cfg.amplitude));
    CHECK(std::fabs(meanAmplitude(sendBurst(tx, engine, level(9600, 0.3f))) - 0.25) < 0.002);
}

void testAmpmCorrection() {
    // Level 7 is 90 degrees ahead of the other levels: entering level 7 adds a phase step of -90 degrees to the
    // carrier, i.e. the integral of the frequency offset over the step equals a quarter of a cycle.
    SoapyRPITX_TxConfig cfg = envelopeConfig();
    cfg.amplitude.ampmLut = {0, 0, 0, 0, 0, 0, 0, -M_PI / 2};
    SoapyRPITX_IQ_dmaSync tx(cfg);
    MockDmaEngine engine(14, kRate);

    std::vector<std::complex<float>> burst = level(4800, 0.5f);
    std::vector<std::complex<float>> high = level(4800, 1.0f);
    burst.insert(burst.end(), high.begin(), high.end());
    for (size_t i = 0; i < 4800; i++) {
        double ph = 2 * M_PI * 1000.0 * (double) i / kRate;
        burst.push_back(std::polar(1.0f, (float) ph));
    }
    std::vector<MockSlot> d = dataFrames(sendBurst(tx, engine, burst));
    CHECK(d.size() == burst.size());
    if (d.size() != burst.size())
        return;

    // The carried quantisation error dithers the fractional word: its mean is the exact centre frequency.
    double center = 0;
    for (size_t i = 100; i < 4700; i++)
        center += d[i].frac;
    center /= (4700 - 100);
    double step = 0;
    for (size_t i = 4700; i < 9500; i++)
        step += (double) d[i].frac - center;
    double tone = 0;
    for (size_t i = 9700; i < 14300; i++)
        tone += (double) d[i].frac - center;
    tone /= (14300 - 9700);
    // step / tone = (phase step / 2 pi) * rate / 1000 Hz = -0.25 * 48 = -12.
    CHECK(tone > 0 && std::fabs(step / tone + 12.0) < 0.3);
}

void testRfDac() {
    SoapyRPITX_TxConfig cfg = envelopeConfig();
    cfg.rfDac = true;
    SoapyRPITX_IQ_dmaSync tx(cfg);
    MockDmaEngine engine(14, kRate);

    // 1/24 of full scale: GPIO20 alone at level 0 (weights 2/3, 1/3, 1).
    std::vector<MockSlot> d = dataFrames(sendBurst(tx, engine, level(4800, 1.0f / 24.0f)));
    size_t only20 = 0;
    for (const MockSlot &x : d)
        only20 += (x.carrier2On && !x.carrierOn && x.pad == 0) ? 1 : 0;
    CHECK(only20 >= 4798);

    d = dataFrames(sendBurst(tx, engine, level(4800, 1.0f)));
    size_t both = 0;
    for (const MockSlot &x : d)
        both += (x.carrier2On && x.carrierOn && x.pad == 7) ? 1 : 0;
    CHECK(both >= 4798);

    volatile uint32_t *gpio = MockRegisters(GENERAL_BASE);
    CHECK((gpio[GPFSEL2] & 7) == fsel_input);
}

void testSlotsPerSample() {
    SoapyRPITX_TxConfig cfg = baseConfig();
    cfg.slotsPerSample = 2;
    cfg.amplitude.envelopeDelay = 0;
    SoapyRPITX_IQ_dmaSync tx(cfg);
    CHECK(tx.SlotsPerSample() == 2);
    CHECK(std::fabs(tx.GetActualSampleRate() - kRate) < kRate * 1e-4);
    MockDmaEngine engine(14, 2 * kRate);
    std::vector<MockSlot> played = sendBurst(tx, engine, level(4800, 1.0f));
    CHECK(carrierOn(played) == 9600);
    CHECK(tx.GetStats().samplesSent == 4800);
}

void testSlotRateLimit() {
    SoapyRPITX_TxConfig cfg = baseConfig();
    cfg.slotsPerSample = 2;
    cfg.tune.sampleRate = 192000;
    CHECK(throws(cfg));
    cfg.slotsPerSample = 5;
    cfg.tune.sampleRate = kRate;
    CHECK(throws(cfg));
    cfg = baseConfig();
    cfg.envelopeOutput = SoapyRPITX_EnvelopeOutput::Pwm;
    cfg.pwmGpio = 13;
    CHECK(throws(cfg));
}

// Frame of the largest frequency excursion and frame where the DAC word first exceeds a threshold.
void checkEerAlignment(const std::vector<MockSlot> &d, size_t step, uint32_t threshold) {
    size_t phaseFrame = 0;
    double best = -1;
    std::vector<uint32_t> before;
    for (size_t i = 100; i + 100 < step; i++)
        before.push_back(d[i].frac);
    const double center = mostFrequent(before);
    for (size_t i = step - 50; i < step + 50; i++) {
        double dev = std::fabs((double) d[i].frac - center);
        if (dev > best) {
            best = dev;
            phaseFrame = i;
        }
    }
    size_t envFrame = d.size();
    for (size_t i = step - 50; i < step + 50 && envFrame == d.size(); i++)
        if (d[i].dacValid && d[i].dac > threshold)
            envFrame = i;
    CHECK(envFrame == phaseFrame);
}

void testEerPcm() {
    SoapyRPITX_TxConfig cfg = envelopeConfig();
    cfg.envelopeOutput = SoapyRPITX_EnvelopeOutput::Pcm;
    {
        SoapyRPITX_IQ_dmaSync tx(cfg);
        volatile uint32_t *gpio = MockRegisters(GENERAL_BASE);
        CHECK(((gpio[GPFSEL1] >> 24) & 7) == fsel_alt0 && ((gpio[GPFSEL1] >> 27) & 7) == fsel_alt0 && ((gpio[GPFSEL2] >> 3) & 7) == fsel_alt0);
        volatile uint32_t *pcm = MockRegisters(PCM_BASE);
        CHECK((pcm[PCM_MODE_A] & PCM_MODE_FTXP) != 0);
        CHECK(((pcm[PCM_MODE_A] >> 10) & 0x3FF) == PCM_DAC_FRAME_LENGTH - 1);

        MockDmaEngine engine(14, kRate);
        // 0.25 at phase 0, then 1.0 at phase pi: the envelope step and the phase reversal leave together.
        std::vector<std::complex<float>> burst = level(2400, 0.25f);
        std::vector<std::complex<float>> high = level(2400, 1.0f, (float) M_PI);
        burst.insert(burst.end(), high.begin(), high.end());
        std::vector<MockSlot> d = dataFrames(sendBurst(tx, engine, burst));
        CHECK(d.size() == burst.size());
        if (d.size() == burst.size()) {
            CHECK(((d[1000].dac & 0xFFFF) == (uint32_t) std::lround(0.25 * 32767)) && (d[1000].dac >> 16) == (d[1000].dac & 0xFFFF));
            CHECK((d[4000].dac & 0xFFFF) == 32767);
            checkEerAlignment(d, 2400, (uint32_t) (0.4 * 32767) << 16);
        }
        SoapyRPITX_TxStats st = tx.GetStats();
        CHECK(st.envelopeLate == 0);
        CHECK(st.envelopeClips == 0);
    }
    volatile uint32_t *gpio = MockRegisters(GENERAL_BASE);
    CHECK(((gpio[GPFSEL1] >> 24) & 7) == fsel_input && ((gpio[GPFSEL2] >> 3) & 7) == fsel_input);
}

void testEerPwm() {
    SoapyRPITX_TxConfig cfg = envelopeConfig();
    cfg.envelopeOutput = SoapyRPITX_EnvelopeOutput::Pwm;
    cfg.pwmBits = 8;
    {
        SoapyRPITX_IQ_dmaSync tx(cfg);
        volatile uint32_t *gpio = MockRegisters(GENERAL_BASE);
        CHECK(((gpio[GPFSEL1] >> 24) & 7) == fsel_alt5);
        volatile uint32_t *p = MockRegisters(PWM_BASE);
        CHECK(p[PWM_RNG1] == 256 && (p[PWM_DMAC] & 0xFF) == PWM_DREQ_LEVEL && (p[PWM_DMAC] & PWM_DMAC_ENAB));
        CHECK(std::fabs(tx.GetActualSampleRate() - kRate) < kRate * 1e-3);

        MockDmaEngine engine(14, kRate);
        std::vector<std::complex<float>> burst = level(2400, 0.25f);
        std::vector<std::complex<float>> high = level(2400, 1.0f, (float) M_PI);
        burst.insert(burst.end(), high.begin(), high.end());
        std::vector<MockSlot> d = dataFrames(sendBurst(tx, engine, burst));
        CHECK(d.size() == burst.size());
        if (d.size() == burst.size()) {
            CHECK(d[1000].dac == 64);
            CHECK(d[4000].dac == 256);
            checkEerAlignment(d, 2400, 100);
        }
    }
    volatile uint32_t *gpio = MockRegisters(GENERAL_BASE);
    CHECK(((gpio[GPFSEL1] >> 24) & 7) == fsel_input);
    volatile uint32_t *p = MockRegisters(PWM_BASE);
    CHECK(p[PWM_CTL] == 0 && p[PWM_DMAC] == 0);
}

void testEndBurstThenShortBurst() {
    // A burst end signalled without samples does not let the next burst skip the pre-roll.
    SoapyRPITX_TxConfig cfg = baseConfig();
    cfg.prerollMs = 300;
    SoapyRPITX_IQ_dmaSync tx(cfg);
    MockDmaEngine engine(14, kRate);
    tx.Start();
    tx.EndBurst();
    auto b = level(480, 1.0f);
    CHECK(tx.Write(b.data(), b.size(), 100000) == b.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!tx.GetStats().dmaRunning);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(tx.GetStats().dmaRunning);
    CHECK(tx.WaitDrained(500000));
    tx.Stop();
}

void testSessionSecurity() {
    const std::string dir = RPITX_LOCK_DIR;
    { SoapyRPITX_Session s(RPITX_TX_GPIO); }

    // The lock directory must be private.
    chmod(dir.c_str(), 0777);
    bool refused = false;
    try {
        SoapyRPITX_Session s(RPITX_TX_GPIO);
    } catch (const std::runtime_error &) {
        refused = true;
    }
    CHECK(refused);
    chmod(dir.c_str(), 0700);

    // A symbolic link in place of the lock file is not followed.
    const std::string path = SoapyRPITX_Session::LockPath();
    unlink(path.c_str());
    CHECK(symlink((dir + "/target").c_str(), path.c_str()) == 0);
    refused = false;
    try {
        SoapyRPITX_Session s(RPITX_TX_GPIO);
    } catch (const std::runtime_error &) {
        refused = true;
    }
    CHECK(refused);
    std::ifstream target(dir + "/target");
    CHECK(!target.good());
    unlink(path.c_str());

    // A record naming a kernel DMA channel is discarded, not applied.
    MockSetKernelDmaMask(1u << 11, true);
    volatile uint32_t *dmaRegs = MockRegisters(DMA_BASE);
    dmaRegs[DMA_CS + 11 * DMA_CHANNEL_WORDS] = DMA_CS_ACTIVE;
    unsigned other = MockAllocate(4096);
    SoapyRPITX_SessionRecord r;
    r.pid = 4242;
    r.dmaChannel = 11;
    r.memHandle = other;
    {
        std::ofstream out(path, std::ios::trunc);
        out << SoapyRPITX_FormatRecord(r);
    }
    {
        SoapyRPITX_Session s(RPITX_TX_GPIO);
        CHECK(s.Discarded() && !s.Recovered());
        CHECK((dmaRegs[DMA_CS + 11 * DMA_CHANNEL_WORDS] & DMA_CS_ACTIVE) != 0);
        CHECK(MockAllocated(other));
    }
    mem_free(0, other);
    dmaRegs[DMA_CS + 11 * DMA_CHANNEL_WORDS] = 0;
    MockSetKernelDmaMask(0, true);

    // Recovery of the envelope pins and of the PWM block.
    volatile uint32_t *gpio = MockRegisters(GENERAL_BASE);
    gpio[GPFSEL2] = (gpio[GPFSEL2] & ~(7u << 3)) | ((uint32_t) fsel_alt0 << 3);
    r = SoapyRPITX_SessionRecord();
    r.pid = 4243;
    r.pinFunctions = {{21, fsel_output}};
    r.pwmValid = true;
    r.pwmRng = 32;
    r.pwmClkCtl = clk_osc;
    r.pwmClkDiv = 4 << 12;
    {
        std::ofstream out(path, std::ios::trunc);
        out << SoapyRPITX_FormatRecord(r);
    }
    {
        SoapyRPITX_Session s(RPITX_TX_GPIO);
        CHECK(s.Recovered());
        CHECK(((gpio[GPFSEL2] >> 3) & 7) == fsel_output);
        volatile uint32_t *p = MockRegisters(PWM_BASE);
        CHECK(p != nullptr && p[PWM_RNG1] == 32);
    }
    gpio[GPFSEL2] &= ~(7u << 3);
}

void testAllocationRecordedEarly() {
    // The DMA channel and the VideoCore memory are in the lock file before anything else is done with them.
    bool checked = false;
    MockOnNextVcMap([&] {
        std::ifstream in(SoapyRPITX_Session::LockPath());
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        SoapyRPITX_SessionRecord rec;
        checked = SoapyRPITX_ParseRecord(text, rec) && rec.memHandle != 0 && rec.dmaChannel >= 0;
    });
    { SoapyRPITX_IQ_dmaSync tx(baseConfig()); }
    CHECK(checked);
}

// Carrier-on slots of a burst in EER mode, where every data slot (even a zero sample) switches GPIO4 on.
SoapyRPITX_TxConfig eerCountingConfig() {
    SoapyRPITX_TxConfig cfg = envelopeConfig();
    cfg.envelopeOutput = SoapyRPITX_EnvelopeOutput::Pcm;
    return cfg;
}

void testRetuneBeforeDmaStartKeepsData() {
    // A retune before the DMA starts replays the queued samples, not the idle slots that precede them.
    SoapyRPITX_TxConfig cfg = eerCountingConfig();
    cfg.prerollMs = 1000;
    SoapyRPITX_IQ_dmaSync tx(cfg);
    MockDmaEngine engine(14, kRate);
    tx.Start();
    std::vector<std::complex<float>> burst = level(480, 0.5f);
    CHECK(tx.Write(burst.data(), burst.size(), 1000000) == burst.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!tx.GetStats().dmaRunning);
    SoapyRPITX_TuneConfig t = tx.GetTune();
    t.frequency = 145.0e6;
    CHECK(tx.Retune(t) == SoapyRPITX_RetuneResult::Ok);
    tx.EndBurst();
    CHECK(tx.WaitDrained(3000000));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    size_t on = carrierOn(engine.Take());
    CHECK(on >= burst.size() && on <= burst.size() + 1);
    tx.Stop();
}

void testRetuneAfterGapKeepsData() {
    // Idle slots written while the FIFO was empty are not replayed as data after a retune.
    SoapyRPITX_IQ_dmaSync tx(eerCountingConfig());
    MockDmaEngine engine(14, kRate);
    tx.Start();
    std::vector<std::complex<float>> first = level(2400, 0.5f);
    CHECK(tx.Write(first.data(), first.size(), 1000000) == first.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::vector<std::complex<float>> second = level(4800, 0.5f);
    CHECK(tx.Write(second.data(), second.size(), 1000000) == second.size());
    SoapyRPITX_TuneConfig t = tx.GetTune();
    t.frequency = 145.0e6;
    CHECK(tx.Retune(t) == SoapyRPITX_RetuneResult::Ok);
    tx.EndBurst();
    CHECK(tx.WaitDrained(3000000));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    size_t on = carrierOn(engine.Take());
    size_t total = first.size() + second.size();
    CHECK(on >= total && on <= total + 1);
    tx.Stop();
}

void testDrainDuringRetune() {
    // A retune from another thread while WaitDrained() waits does not end the wait before the replayed
    // samples are played.
    for (int round = 0; round < 3; round++) {
        SoapyRPITX_IQ_dmaSync tx(eerCountingConfig());
        MockDmaEngine engine(14, kRate);
        tx.Start();
        std::vector<std::complex<float>> burst = level(9600, 0.5f);
        CHECK(tx.Write(burst.data(), burst.size(), 1000000) == burst.size());
        tx.EndBurst();
        std::thread retuner([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(60 + 20 * round));
            SoapyRPITX_TuneConfig t = tx.GetTune();
            t.frequency = round % 2 ? 144.8e6 : 145.0e6;
            CHECK(tx.Retune(t) == SoapyRPITX_RetuneResult::Ok);
        });
        CHECK(tx.WaitDrained(3000000));
        size_t onAtDrain = carrierOn(engine.Take());
        retuner.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        size_t on = onAtDrain + carrierOn(engine.Take());
        CHECK(on >= burst.size() && on <= burst.size() + 1);
        CHECK(onAtDrain + 1 >= burst.size());
        tx.Stop();
    }
}

void testReleaseRecordedBeforeFree() {
    // The lock file stops naming the DMA channel and the VideoCore memory before the memory is released.
    bool checked = false;
    bool clear = false;
    MockOnNextVcFree([&](unsigned) {
        std::ifstream in(SoapyRPITX_Session::LockPath());
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        SoapyRPITX_SessionRecord rec;
        checked = SoapyRPITX_ParseRecord(text, rec);
        clear = checked && rec.memHandle == 0 && rec.dmaChannel < 0;
    });
    { SoapyRPITX_IQ_dmaSync tx(baseConfig()); }
    CHECK(checked && clear);
}

void testRecordFormat() {
    SoapyRPITX_SessionRecord r;
    r.pid = 77;
    r.dmaChannel = 7;
    r.dmaMaskKnown = false;
    r.memHandle = 5;
    const std::string text = SoapyRPITX_FormatRecord(r);
    SoapyRPITX_SessionRecord parsed;
    CHECK(SoapyRPITX_ParseRecord(text, parsed) && parsed.complete && !parsed.dmaMaskKnown && parsed.dmaChannel == 7);

    // Text after the end marker (the tail of a longer record) is ignored.
    CHECK(SoapyRPITX_ParseRecord(text + "dma_channel=3\nmem_handle=99\n", parsed) && parsed.complete && parsed.dmaChannel == 7 && parsed.memHandle == 5);

    // A record without the end marker is reported as incomplete; one without the mask flag keeps the default.
    CHECK(SoapyRPITX_ParseRecord("pid=1\ndma_channel=7\n", parsed) && !parsed.complete && parsed.dmaMaskKnown);

    // With an unreadable kernel mask, only a channel allocated under the same condition is accepted.
    std::string why;
    MockSetKernelDmaMask(0, false);
    r.dmaMaskKnown = true;
    CHECK(!SoapyRPITX_RecordPlausible(r, why));
    r.dmaMaskKnown = false;
    CHECK(SoapyRPITX_RecordPlausible(r, why));
    MockSetKernelDmaMask(0, true);
}

void testUnknownMaskRecovery() {
    MockSetKernelDmaMask(0, false);

    // A transmitter on an explicit channel records that the mask could not be read.
    SoapyRPITX_TxConfig cfg = baseConfig();
    cfg.dmaChannel = 7;
    {
        SoapyRPITX_IQ_dmaSync tx(cfg);
        std::ifstream in(SoapyRPITX_Session::LockPath());
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        SoapyRPITX_SessionRecord own;
        CHECK(SoapyRPITX_ParseRecord(text, own) && own.dmaChannel == 7 && !own.dmaMaskKnown);
    }

    // The record of such a transmitter left by a terminated process is recovered.
    volatile uint32_t *dmaRegs = MockRegisters(DMA_BASE);
    dmaRegs[DMA_CS + 7 * DMA_CHANNEL_WORDS] = DMA_CS_ACTIVE;
    unsigned leaked = MockAllocate(4096);
    SoapyRPITX_SessionRecord r;
    r.pid = 999998;
    r.dmaChannel = 7;
    r.dmaMaskKnown = false;
    r.memHandle = leaked;
    dmaRegs[DMA_CONBLK_AD + 7 * DMA_CHANNEL_WORDS] = mem_lock(0, leaked) + 64; // record without the block location: mailbox lookup
    r.pads = kDefaultPads;
    {
        std::ofstream out(SoapyRPITX_Session::LockPath(), std::ios::trunc);
        out << SoapyRPITX_FormatRecord(r);
    }
    {
        SoapyRPITX_Session s(RPITX_TX_GPIO);
        CHECK(s.Recovered() && !s.Discarded());
        CHECK((dmaRegs[DMA_CS + 7 * DMA_CHANNEL_WORDS] & DMA_CS_ACTIVE) == 0);
        CHECK(!MockAllocated(leaked));
    }
    MockSetKernelDmaMask(0, true);
}

void testRecoveryKeepsForeignDma() {
    // After a crash another program may own the DMA channel: it is reset only when idle or running our chain.
    volatile uint32_t *dmaRegs = MockRegisters(DMA_BASE);
    const std::string path = SoapyRPITX_Session::LockPath();
    const unsigned ch = 9;
    unsigned handle = MockAllocate(8192);
    const uint32_t bus = mem_lock(0, handle);

    SoapyRPITX_SessionRecord r;
    r.pid = 999990;
    r.dmaChannel = ch;
    r.memHandle = handle;
    r.memBus = bus;
    r.memSize = 8192;
    r.pads = kDefaultPads;

    // Active, control block outside our block (another program): left running.
    dmaRegs[DMA_CS + ch * DMA_CHANNEL_WORDS] = DMA_CS_ACTIVE;
    dmaRegs[DMA_CONBLK_AD + ch * DMA_CHANNEL_WORDS] = 0x7E000000u + 0x1000;
    {
        std::ofstream out(path, std::ios::trunc);
        out << SoapyRPITX_FormatRecord(r);
    }
    {
        SoapyRPITX_Session s(RPITX_TX_GPIO);
        CHECK(s.Recovered());
        CHECK((dmaRegs[DMA_CS + ch * DMA_CHANNEL_WORDS] & DMA_CS_ACTIVE) != 0);
        CHECK(s.RecoveryReport().find("now used by another program") != std::string::npos);
        CHECK(!MockAllocated(handle)); // the leaked memory is still released
    }

    // Just past the end of our block: still foreign.
    handle = MockAllocate(8192);
    r.memHandle = handle;
    r.memBus = mem_lock(0, handle);
    dmaRegs[DMA_CONBLK_AD + ch * DMA_CHANNEL_WORDS] = r.memBus + 8192;
    {
        std::ofstream out(path, std::ios::trunc);
        out << SoapyRPITX_FormatRecord(r);
    }
    {
        SoapyRPITX_Session s(RPITX_TX_GPIO);
        CHECK((dmaRegs[DMA_CS + ch * DMA_CHANNEL_WORDS] & DMA_CS_ACTIVE) != 0);
    }

    // Last control block of our block: ours, reset.
    handle = MockAllocate(8192);
    r.memHandle = handle;
    r.memBus = mem_lock(0, handle);
    dmaRegs[DMA_CONBLK_AD + ch * DMA_CHANNEL_WORDS] = r.memBus + 8192 - 32;
    {
        std::ofstream out(path, std::ios::trunc);
        out << SoapyRPITX_FormatRecord(r);
    }
    {
        SoapyRPITX_Session s(RPITX_TX_GPIO);
        CHECK((dmaRegs[DMA_CS + ch * DMA_CHANNEL_WORDS] & DMA_CS_ACTIVE) == 0);
    }

    // Idle channel: reset (harmless).
    handle = MockAllocate(8192);
    r.memHandle = handle;
    r.memBus = mem_lock(0, handle);
    dmaRegs[DMA_CS + ch * DMA_CHANNEL_WORDS] = 0;
    {
        std::ofstream out(path, std::ios::trunc);
        out << SoapyRPITX_FormatRecord(r);
    }
    {
        SoapyRPITX_Session s(RPITX_TX_GPIO);
        CHECK(s.RecoveryReport().find("reset") != std::string::npos && s.RecoveryReport().find("not reset") == std::string::npos);
    }

    // The block location survives the record text.
    SoapyRPITX_SessionRecord back;
    CHECK(SoapyRPITX_ParseRecord(SoapyRPITX_FormatRecord(r), back) && back.memBus == r.memBus && back.memSize == 8192);
}

void testIncompleteRecordKeepsMemory() {
    unsigned handle = MockAllocate(4096);
    SoapyRPITX_SessionRecord r;
    r.pid = 999997;
    r.memHandle = handle;
    r.pads = kDefaultPads;
    std::string text = SoapyRPITX_FormatRecord(r);
    text.erase(text.find("end=1"));
    {
        std::ofstream out(SoapyRPITX_Session::LockPath(), std::ios::trunc);
        out << text;
    }
    {
        SoapyRPITX_Session s(RPITX_TX_GPIO);
        CHECK(s.Recovered());
        CHECK(s.RecoveryReport().find("not released") != std::string::npos);
        CHECK(MockAllocated(handle));
    }
    mem_free(0, handle);
}

void testDeviceTreeStatus() {
    const std::string soc = std::string(RPITX_DT_ROOT) + "/soc";
    const std::string node = soc + "/i2s@7e203000";
    bool createdSoc = mkdir(soc.c_str(), 0755) == 0;
    CHECK(mkdir(node.c_str(), 0755) == 0);

    // A node without a status property is enabled.
    CHECK(SoapyRPITX_I2sEnabled());
    {
        std::ofstream st(node + "/status", std::ios::binary);
        st << "disabled" << '\0';
    }
    CHECK(!SoapyRPITX_I2sEnabled());
    {
        std::ofstream st(node + "/status", std::ios::binary);
        st << "okay" << '\0';
    }
    CHECK(SoapyRPITX_I2sEnabled());

    unlink((node + "/status").c_str());
    rmdir(node.c_str());
    if (createdSoc)
        rmdir(soc.c_str());
    CHECK(!SoapyRPITX_I2sEnabled());
}

} // namespace

int main() {
    testStreaming();
    testShortBurstStarts();
    testRetuneCarrierOffAndReplay();
    testRefusedRetuneKeepsTransmitting();
    testEmergencyStopThenDestroy();
    testConstructorFailureRestores();
    testDmaChannelSelection();
    testSessionLockAndRecovery();
    testDmaRestartAndPads();
    testSignalCleanupHandlers();
    testPacingStart();
    testEnvelopeDelayFrames();
    testSigmaDeltaAverage();
    testAmpmCorrection();
    testRfDac();
    testSlotsPerSample();
    testSlotRateLimit();
    testEerPcm();
    testEerPwm();
    testEndBurstThenShortBurst();
    testSessionSecurity();
    testAllocationRecordedEarly();
    testRetuneBeforeDmaStartKeepsData();
    testRetuneAfterGapKeepsData();
    testDrainDuringRetune();
    testReleaseRecordedBeforeFree();
    testRecordFormat();
    testUnknownMaskRecovery();
    testRecoveryKeepsForeignDma();
    testIncompleteRecordKeepsMemory();
    testDeviceTreeStatus();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all transmitter tests passed\n");
    return 0;
}
