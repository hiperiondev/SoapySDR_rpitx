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
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "SoapyRPITX_IQ_dmaSync.hpp"
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
    CHECK(tx.Retune(t));
    CHECK(tx.GetTune().frequency == 145.5e6);

    // An unreachable frequency is rejected and the previous one stays.
    t.frequency = 5e9;
    CHECK(!tx.Retune(t));
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
    volatile uint32_t *clk = MockRegisters(CLK_BASE);
    volatile uint32_t *gpio = MockRegisters(GENERAL_BASE);
    std::atomic<bool> stop{false};
    std::atomic<int> unsafe{0};
    std::thread monitor([&] {
        uint32_t last = clk[PLLC_CTRL];
        while (!stop.load()) {
            uint32_t c = clk[PLLC_CTRL];
            if (c != last) {
                last = c;
                if (((gpio[GPFSEL0] >> 12) & 7) == fsel_alt0)
                    unsafe++;
            }
        }
    });
    SoapyRPITX_TuneConfig t = tx.GetTune();
    t.frequency = 50.0e6;
    CHECK(tx.Retune(t));
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
    MockSetKernelDmaMask(0, false);

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
    r.pads = kDefaultPads;
    r.gpioFunction = fsel_input;
    {
        std::ofstream out(SoapyRPITX_Session::LockPath(), std::ios::trunc);
        out << SoapyRPITX_FormatRecord(r);
    }
    SoapyRPITX_SessionRecord parsed;
    CHECK(SoapyRPITX_ParseRecord(SoapyRPITX_FormatRecord(r), parsed) && parsed.memHandle == leaked && parsed.dmaChannel == 11);

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
    volatile uint32_t *dmaRegs = MockRegisters(DMA_BASE);
    dmaRegs[DMA_CS + 14 * DMA_CHANNEL_WORDS] &= ~(uint32_t) DMA_CS_ACTIVE;
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
    CHECK(tx.Retune(t));
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

} // namespace

int main() {
    testStreaming();
    testShortBurstStarts();
    testRetuneCarrierOffAndReplay();
    testEmergencyStopThenDestroy();
    testConstructorFailureRestores();
    testDmaChannelSelection();
    testSessionLockAndRecovery();
    testDmaRestartAndPads();
    testSignalCleanupHandlers();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all transmitter tests passed\n");
    return 0;
}
