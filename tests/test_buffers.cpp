// Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
// Project Site: https://github.com/hiperiondev/SoapySDR_rpitx
//
// This is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// Host-side unit tests of the hardware-independent parts of the transmit path:
// - SpscFifo ordering and capacity under a concurrent producer and consumer, and its capacity limit;
// - DmaRingTracker driven by a simulated DMA engine and a feeder that uses the same policy as
//   SoapyRPITX_IQ_dmaSync::FeederLoop() (fill to 3/4, idle padding, scrubbing, resync on overrun);
// - dsp phase gating, phase continuity across gated samples and frequency estimation.

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#include "SoapyRPITX_Amplitude.hpp"
#include "SoapyRPITX_Buffers.hpp"
#include "dsp.hpp"

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                                                            \
    do {                                                                                                                                                       \
        if (!(cond)) {                                                                                                                                         \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                                                      \
            failures++;                                                                                                                                        \
        }                                                                                                                                                      \
    } while (0)

void testFifoBasic() {
    SpscFifo<int> f(4);
    int in[6] = {1, 2, 3, 4, 5, 6};
    CHECK(f.push(in, 6) == 4);
    CHECK(f.size() == 4);
    CHECK(f.space() == 0);
    int out[6] = {0};
    CHECK(f.pop(out, 3) == 3);
    CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3);
    CHECK(f.push(in + 4, 2) == 2);
    CHECK(f.pop(out, 6) == 3);
    CHECK(out[0] == 4 && out[1] == 5 && out[2] == 6);
    CHECK(f.size() == 0);
    f.push(in, 2);
    f.clear();
    CHECK(f.size() == 0);
}

void testFifoLimit() {
    SpscFifo<int> f(8);
    CHECK(f.capacity() == 8 && f.storage() == 8);
    int in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    f.setLimit(3);
    CHECK(f.push(in, 8) == 3);
    CHECK(f.space() == 0);
    // Lowering the limit below the fill level keeps the stored elements and blocks further pushes.
    f.setLimit(2);
    CHECK(f.size() == 3);
    CHECK(f.space() == 0);
    CHECK(f.push(in, 1) == 0);
    int out[8] = {0};
    CHECK(f.pop(out, 8) == 3 && out[2] == 3);
    f.setLimit(100); // clamped to the storage
    CHECK(f.capacity() == 8);
    CHECK(f.push(in, 8) == 8);
    f.setLimit(0); // clamped to 1
    CHECK(f.capacity() == 1);
}

void testFifoThreaded() {
    SpscFifo<uint32_t> f(1000);
    const uint32_t total = 2000000;
    bool ordered = true;

    std::thread consumer([&] {
        uint32_t expect = 0;
        std::vector<uint32_t> buf(333);
        while (expect < total) {
            size_t m = f.pop(buf.data(), buf.size());
            for (size_t i = 0; i < m; i++) {
                if (buf[i] != expect)
                    ordered = false;
                expect++;
            }
            if (m == 0)
                std::this_thread::yield();
        }
    });

    std::vector<uint32_t> chunk(257);
    uint32_t next = 0;
    while (next < total) {
        size_t n = std::min<size_t>(chunk.size(), total - next);
        for (size_t i = 0; i < n; i++)
            chunk[i] = next + (uint32_t) i;
        size_t done = 0;
        while (done < n) {
            size_t m = f.push(chunk.data() + done, n - done);
            done += m;
            if (m == 0)
                std::this_thread::yield();
        }
        next += (uint32_t) n;
    }
    consumer.join();
    CHECK(ordered);
}

// Ring simulation. Tags >= 0 are data sequence numbers, -1 is the idle sample.
struct RingSim {
    explicit RingSim(uint32_t n) :
            ring(n, -1),
            tracker(n) {
    }

    std::vector<int64_t> ring;
    DmaRingTracker tracker;
    uint64_t engine = 0;     // absolute position of the executing slot
    uint64_t lastPoll = 0;   // engine position at the previous feeder tick
    int64_t nextData = 0;    // next data tag produced by the host
    int64_t lastPlayed = -1; // last data tag played
    uint64_t late = 0;
    bool writeIntoActive = false;
    bool repeated = false; // a data tag was played twice or out of order
    bool gap = false;      // a data tag was skipped
    bool idleAfterStart = false;
    bool started = false;

    void write(uint64_t absPos, int64_t tag) {
        uint32_t slot = tracker.slotOf(absPos);
        if (started && slot == tracker.slotOf(engine))
            writeIntoActive = true;
        ring[slot] = tag;
    }

    // Engine plays one slot.
    void step() {
        int64_t tag = ring[tracker.slotOf(engine)];
        if (tag >= 0) {
            if (tag <= lastPlayed)
                repeated = true;
            else if (tag != lastPlayed + 1)
                gap = true;
            lastPlayed = tag;
        } else if (started && lastPlayed >= 0) {
            idleAfterStart = true;
        }
        engine++;
    }

    // One feeder tick; available = data samples the host has queued (-1 = unlimited).
    void tick(int64_t available) {
        const uint32_t n = tracker.size();
        const uint32_t target = n * 3 / 4;
        const uint32_t low = n / 8;
        const uint32_t guard = 32;

        if (tracker.update(tracker.slotOf(engine), (double) (engine - lastPoll))) {
            // Same policy as the transmitter: silence every slot except the executing one, then resync.
            late++;
            for (uint32_t i = 0; i < n; i++)
                if (i != tracker.slotOf(engine))
                    ring[i] = -1;
            tracker.resync(guard);
        }
        lastPoll = engine;
        CHECK(tracker.readPos() == engine);

        uint64_t b, e;
        tracker.scrubRange(b, e);
        for (uint64_t p = b; p < e; p++)
            write(p, -1);
        tracker.markScrubbed(e);

        uint32_t w = tracker.writable(target);
        bool fifoEmpty = false;
        if (available >= 0 && (int64_t) w >= available) {
            w = (uint32_t) available;
            fifoEmpty = true;
        }
        for (uint32_t i = 0; i < w; i++)
            write(tracker.writePos() + i, nextData++);
        tracker.advanceWrite(w);

        if (fifoEmpty && tracker.fill() < (int64_t) low) {
            uint32_t pad = tracker.writable(low);
            for (uint32_t i = 0; i < pad; i++)
                write(tracker.writePos() + i, -1);
            tracker.advanceWrite(pad);
        }
    }

    void start() {
        tracker.reset();
        uint32_t w = tracker.writable(tracker.size() * 3 / 4);
        for (uint32_t i = 0; i < w; i++)
            write(i, nextData++);
        tracker.advanceWrite(w);
        started = true;
        engine = 0;
        lastPoll = 0;
    }
};

void testRingSteady() {
    RingSim sim(4096);
    sim.start();
    std::mt19937 rng(1);
    std::uniform_int_distribution<int> jitter(20, 400); // feeder tick spacing in samples
    for (int t = 0; t < 20000; t++) {
        int steps = jitter(rng);
        for (int i = 0; i < steps; i++)
            sim.step();
        sim.tick(-1);
    }
    CHECK(sim.late == 0);
    CHECK(!sim.repeated);
    CHECK(!sim.gap);
    CHECK(!sim.idleAfterStart);
    CHECK(!sim.writeIntoActive);
    CHECK(sim.lastPlayed > 1000000);
}

void testRingShortStalls() {
    // Feeder stalls shorter than one ring: idle (silence) may be played, stale data never.
    RingSim sim(4096);
    sim.start();
    std::mt19937 rng(2);
    std::uniform_int_distribution<int> stall(0, 3600);
    for (int t = 0; t < 5000; t++) {
        int steps = (t % 50 == 49) ? stall(rng) : 48;
        for (int i = 0; i < steps; i++)
            sim.step();
        sim.tick(-1);
    }
    CHECK(!sim.repeated);
    CHECK(!sim.writeIntoActive);
    CHECK(sim.late > 0);
}

void testRingLapLoss() {
    // A stall longer than one ring must be detected from the elapsed time even though the modular
    // position looks as if the engine had barely moved.
    RingSim sim(4096);
    sim.start();
    for (int t = 0; t < 100; t++) {
        for (int i = 0; i < 48; i++)
            sim.step();
        sim.tick(-1);
    }
    uint64_t lateBefore = sim.late;
    for (int i = 0; i < 4096 * 2 + 10; i++)
        sim.step();
    sim.tick(-1);
    CHECK(sim.late == lateBefore + 1);
    CHECK(sim.tracker.readPos() == sim.engine);

    // After recovery playback is ordered again. The slot executing at the time of the resync cannot be
    // rewritten, so it is skipped before checking.
    int64_t firstNew = sim.nextData - (int64_t) (sim.tracker.writePos() - (sim.tracker.readPos() + 32));
    sim.step();
    sim.repeated = false;
    sim.lastPlayed = firstNew - 1;
    int64_t mark = sim.nextData;
    for (int t = 0; t < 1000; t++) {
        for (int i = 0; i < 48; i++)
            sim.step();
        sim.tick(-1);
    }
    CHECK(!sim.repeated);
    CHECK(sim.lastPlayed > mark);
}

void testRingDoubleStall() {
    // Two feeder stalls in a row: after the first overrun the data played during the stall must not be
    // replayed when the feeder stalls again before rewriting the ring.
    RingSim sim(4096);
    sim.start();
    for (int t = 0; t < 100; t++) {
        for (int i = 0; i < 48; i++)
            sim.step();
        sim.tick(-1);
    }
    for (int i = 0; i < 4096 + 1000; i++)
        sim.step();
    sim.tick(0);
    CHECK(sim.late == 1);
    sim.repeated = false;
    for (int i = 0; i < 4000; i++)
        sim.step();
    CHECK(!sim.repeated);
}

void testRingHostStarvation() {
    // The host stops supplying data: the ring is padded with idle samples and the engine never overruns.
    RingSim sim(4096);
    sim.start();
    for (int t = 0; t < 3000; t++) {
        for (int i = 0; i < 48; i++)
            sim.step();
        int64_t avail = (t / 500) % 2 ? 0 : 30; // alternate between slower-than-real-time data and none
        sim.tick(avail);
    }
    CHECK(sim.late == 0);
    CHECK(!sim.repeated);
    CHECK(!sim.writeIntoActive);
}

void testDsp() {
    const uint32_t fs = 48000;
    dsp d(fs);

    // Tone at +1 kHz.
    for (int i = 0; i < 1000; i++) {
        double ph = 2 * M_PI * 1000.0 * i / fs;
        d.pushsample(std::complex<float>((float) cos(ph), (float) sin(ph)));
        if (i > 0)
            CHECK(std::fabs(d.frequency - 1000.0) < 1.0);
    }

    // Silence keeps the carrier at the centre frequency instead of producing noise.
    for (int i = 0; i < 100; i++) {
        d.pushsample(std::complex<float>(1e-6f * (float) ((i * 7919) % 13 - 6), 1e-6f * (float) ((i * 104729) % 11 - 5)));
        CHECK(d.frequency == 0.0);
    }

    // The first sample after silence applies the phase change from the last valid sample.
    double lastPhase = std::remainder(2 * M_PI * 1000.0 * 999 / fs, 2 * M_PI);
    d.pushsample(std::complex<float>(-1.0f, 0.0f));
    double expected = std::remainder(M_PI - lastPhase, 2 * M_PI) * fs / (2 * M_PI);
    CHECK(std::fabs(std::fabs(d.frequency) - std::fabs(expected)) < 1.0);

    // BPSK reversal through an exact zero: the carrier phase accumulated by the frequencies turns by pi.
    d.reset();
    const std::complex<float> bpsk[] = {{1, 0}, {1, 0}, {1, 0}, {0, 0}, {-1, 0}, {-1, 0}};
    double carrier = 0;
    for (const auto &x : bpsk) {
        d.pushsample(x);
        carrier += 2 * M_PI * d.frequency / fs;
    }
    CHECK(std::fabs(std::fabs(carrier) - M_PI) < 1e-6);

    // Negative frequency, near the band edge.
    d.reset();
    for (int i = 0; i < 100; i++) {
        double ph = -2 * M_PI * 20000.0 * i / fs;
        d.pushsample(std::complex<float>((float) cos(ph), (float) sin(ph)));
        if (i > 0)
            CHECK(std::fabs(d.frequency + 20000.0) < 5.0);
    }

    // NaN input is gated.
    d.pushsample(std::complex<float>(NAN, NAN));
    CHECK(std::isfinite(d.frequency));
}

// The nearest state of the uncalibrated model reproduces level = round(a * (drive + 1)) - 1, off when negative.
void testAmplitudeModelLinear() {
    SoapyRPITX_AmplitudeConfig cfg;
    for (int drive = 0; drive <= 7; drive++) {
        SoapyRPITX_AmplitudeModel m(cfg, drive, false);
        CHECK(m.size() == (size_t) drive + 2);
        CHECK(std::fabs(m.state(m.top()).amplitude - 1.0) < 1e-12);
        for (int i = 0; i <= 1200; i++) {
            double a = i / 1000.0;
            double x = std::min(a, 1.0) * (drive + 1);
            if (std::fabs(x - std::floor(x) - 0.5) < 1e-9)
                continue; // exactly on a decision threshold
            long level = std::lround(x) - 1;
            const SoapyRPITX_AmpState &st = m.state(m.nearest(a));
            CHECK((st.pins != 0) == (level >= 0));
            if (level >= 0)
                CHECK(st.level == (int) level);
        }
    }
}

void testAmplitudeModelTables() {
    SoapyRPITX_AmplitudeConfig cfg;
    cfg.amplitudeLut = {0.05, 0.2, 0.45, 0.6, 0.75, 0.85, 0.95, 1.0};
    cfg.ampmLut = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.0};
    CHECK(SoapyRPITX_CheckAmplitudeConfig(cfg).empty());
    SoapyRPITX_AmplitudeModel m(cfg, 7, false);
    CHECK(m.state(m.nearest(0.44)).level == 2);
    CHECK(std::fabs(m.state(m.nearest(0.44)).phase - 0.3) < 1e-12);
    CHECK(m.state(m.nearest(0.02)).pins == 0);
    CHECK(m.state(m.nearest(0.03)).level == 0);

    // Lower drive: the strongest available level becomes full scale.
    SoapyRPITX_AmplitudeModel m3(cfg, 3, false);
    CHECK(m3.state(m3.top()).level == 3);
    CHECK(std::fabs(m3.state(m3.nearest(0.75)).amplitude - 0.75) < 1e-12);

    // Two clock pins: GPIO20 alone is the weakest weight.
    SoapyRPITX_AmplitudeConfig plain;
    SoapyRPITX_AmplitudeModel dual(plain, 7, true);
    CHECK(dual.size() == 1 + 8 * 3);
    CHECK(dual.state(dual.top()).pins == 3);
    const SoapyRPITX_AmpState &weak = dual.state(dual.nearest(1.0 / 24.0));
    CHECK(weak.pins == 2 && weak.level == 0);

    // Invalid tables.
    SoapyRPITX_AmplitudeConfig bad;
    bad.amplitudeLut = {1, 2, 3};
    CHECK(!SoapyRPITX_CheckAmplitudeConfig(bad).empty());
    bad = SoapyRPITX_AmplitudeConfig();
    bad.amplitudeLut = {0.1, 0.2, 0.2, 0.4, 0.5, 0.6, 0.7, 0.8};
    CHECK(!SoapyRPITX_CheckAmplitudeConfig(bad).empty());
    bad = SoapyRPITX_AmplitudeConfig();
    bad.eerLut = {0.5, 0.4};
    CHECK(!SoapyRPITX_CheckAmplitudeConfig(bad).empty());
    bad = SoapyRPITX_AmplitudeConfig();
    bad.envelopeDelay = 40;
    CHECK(!SoapyRPITX_CheckAmplitudeConfig(bad).empty());
    bad = SoapyRPITX_AmplitudeConfig();
    bad.sigmaDeltaOrder = 3;
    CHECK(!SoapyRPITX_CheckAmplitudeConfig(bad).empty());
}

void testSigmaDelta() {
    SoapyRPITX_AmplitudeConfig cfg;
    SoapyRPITX_AmplitudeModel m(cfg, 7, false);
    for (int order = 1; order <= 2; order++) {
        for (double target : {0.0, 0.03, 0.3, 0.71, 1.0, 1.5}) {
            SoapyRPITX_AmplitudeQuantizer q;
            double sum = 0;
            const int n = 20000;
            for (int i = 0; i < n; i++)
                sum += m.state(q.quantize(m, target, true, order)).amplitude;
            double mean = sum / n;
            CHECK(std::fabs(mean - std::min(target, 1.0)) < 0.002);
        }
    }
    // Nearest selection keeps the quantisation error.
    SoapyRPITX_AmplitudeQuantizer q;
    CHECK(std::fabs(m.state(q.quantize(m, 0.3, false, 2)).amplitude - 0.25) < 1e-12);
}

void testEnvelopeDelay() {
    SoapyRPITX_EnvelopeDelay d;
    d.reset();
    for (int i = 1; i <= 10; i++)
        d.push(i);
    CHECK(d.at(0) == 10);
    CHECK(d.at(1) == 9);
    CHECK(std::fabs(d.at(0.5) - 9.5) < 1e-12);
    CHECK(std::fabs(d.at(2.25) - 7.75) < 1e-12);
    CHECK(d.at(50) == 0);

    int advance = -1;
    CHECK(std::fabs(SoapyRPITX_SplitEnvelopeDelay(0.5, 0, advance) - 0.5) < 1e-12 && advance == 0);
    CHECK(std::fabs(SoapyRPITX_SplitEnvelopeDelay(0.5, 17, advance) - 0.5) < 1e-12 && advance == 17);
    CHECK(std::fabs(SoapyRPITX_SplitEnvelopeDelay(-2.3, 0, advance) - 0.7) < 1e-9 && advance == 3);
    CHECK(std::fabs(SoapyRPITX_SplitEnvelopeDelay(-2, 0, advance)) < 1e-12 && advance == 2);
}

void testEerCode() {
    SoapyRPITX_AmplitudeConfig cfg;
    bool clipped = false;
    SoapyRPITX_AmplitudeModel linear(cfg, 7, false);
    CHECK(std::fabs(linear.eerCode(0.4, clipped) - 0.4) < 1e-12 && !clipped);
    CHECK(linear.eerCode(1.2, clipped) == 1.0 && clipped);

    cfg.eerLut = {0.0, 0.5, 0.8, 1.0};
    cfg.eerAmpmLut = {0.0, 0.3, 0.6, 0.9};
    SoapyRPITX_AmplitudeModel m(cfg, 7, false);
    CHECK(std::fabs(m.eerCode(0.5, clipped) - 1.0 / 3.0) < 1e-12);
    CHECK(std::fabs(m.eerCode(0.9, clipped) - (2.0 / 3.0 + 0.5 / 3.0)) < 1e-12);
    CHECK(std::fabs(m.eerPhase(0.5) - 0.45) < 1e-12);

    cfg = SoapyRPITX_AmplitudeConfig();
    cfg.eerGain = 0.5;
    cfg.eerOffset = 0.1;
    SoapyRPITX_AmplitudeModel g(cfg, 7, false);
    CHECK(std::fabs(g.eerCode(0.8, clipped) - 0.5) < 1e-12 && !clipped);
}

// Delay (in samples) that best maps the reference curve onto the measured values taken at slot centres.
double fitDelay(const std::vector<double> &measured, const std::vector<double> &reference) {
    double best = 0, bestErr = 1e300;
    for (double D = 0; D <= 2.0; D += 0.005) {
        double err = 0;
        for (size_t k = 40; k + 40 < measured.size(); k++) {
            double t = (double) k + 0.5 - D;
            size_t i = (size_t) std::floor(t);
            double f = t - (double) i;
            double r = reference[i] + f * (reference[i + 1] - reference[i]);
            err += (measured[k] - r) * (measured[k] - r);
        }
        if (err < bestErr) {
            bestErr = err;
            best = D;
        }
    }
    return best;
}

// The held envelope of slot k and the phase ramped by the frequency of slot k reach the antenna with the same
// delay when the envelope is delayed by half a sample.
void testEnvelopePhaseAlignment() {
    const uint32_t fs = 48000;
    const int n = 200;
    std::vector<double> amp(n + 2), ph(n + 2);
    for (int i = 0; i < n + 2; i++) {
        double x = std::min(std::max((i - 80) / 40.0, 0.0), 1.0);
        double c = 0.5 - 0.5 * std::cos(M_PI * x);
        amp[i] = 0.2 + 0.8 * c;
        ph[i] = 1.2 * c;
    }
    for (double d : {0.0, 0.5}) {
        dsp p(fs);
        SoapyRPITX_EnvelopeDelay delay;
        delay.reset();
        std::vector<double> env(n), phase(n);
        double carrier = 0;
        for (int k = 0; k < n; k++) {
            p.pushsample(std::complex<float>((float) (amp[k] * std::cos(ph[k])), (float) (amp[k] * std::sin(ph[k]))));
            delay.push(p.amplitude);
            env[k] = delay.at(d);
            double start = carrier;
            carrier += 2 * M_PI * p.frequency / fs;
            phase[k] = 0.5 * (start + carrier); // phase at the centre of the slot
        }
        double de = fitDelay(env, amp);
        double dp = fitDelay(phase, ph);
        if (d == 0.5)
            CHECK(std::fabs(de - dp) < 0.05);
        else
            CHECK(std::fabs((dp - de) - 0.5) < 0.05);
    }
}

} // namespace

void testRingStoppedEngine() {
    // With no expected progress (an engine that stopped by itself) only the modular distance counts, even
    // when the time since the previous poll is longer than half a ring.
    DmaRingTracker t(512);
    t.advanceWrite(400);
    CHECK(!t.update(100, 0.0));
    CHECK(t.readPos() == 100);
    CHECK(!t.update(100, 0.0));
    CHECK(t.readPos() == 100);
    // The same poll with an expected progress above half a ring is taken as a lap.
    DmaRingTracker u(512);
    u.advanceWrite(400);
    u.update(100, 0.0);
    u.update(100, 300.0);
    CHECK(u.readPos() == 100 + 512);
}

int main() {
    testFifoBasic();
    testFifoLimit();
    testFifoThreaded();
    testRingSteady();
    testRingShortStalls();
    testRingLapLoss();
    testRingDoubleStall();
    testRingHostStarvation();
    testRingStoppedEngine();
    testDsp();
    testAmplitudeModelLinear();
    testAmplitudeModelTables();
    testSigmaDelta();
    testEnvelopeDelay();
    testEerCode();
    testEnvelopePhaseAlignment();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
