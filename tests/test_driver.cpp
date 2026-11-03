// Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
// Project Site: https://github.com/hiperiondev/SoapySDR_rpitx
//
// This is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// Test of the SoapySDR device layer (SoapyRPITX) on simulated hardware: settings parsing and ranges,
// frequency correction, stream argument validation, CS16/CS8/CF32 scaling, END_BURST draining,
// readStreamStatus and sensors. The build points the device tree, /dev/mem and /dev/vcio paths to files
// generated in the build directory (see CMakeLists.txt). Also covers the profile file format, the checks of
// profile files, the profile=<file> device argument / setting and the limits of numeric inputs and timeouts.

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.hpp>

#include "SoapyRPITX.hpp"
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

template <typename F>
bool throws(F f) {
    try {
        f();
    } catch (const std::runtime_error &) {
        return true;
    }
    return false;
}

size_t carrierOn(const std::vector<MockSlot> &s) {
    size_t n = 0;
    for (const MockSlot &x : s)
        n += x.carrierOn ? 1 : 0;
    return n;
}

// Most frequent pad level among the slots with the carrier on.
uint32_t dominantPad(const std::vector<MockSlot> &s) {
    size_t count[8] = {0};
    for (const MockSlot &x : s)
        if (x.carrierOn)
            count[x.pad & 7]++;
    uint32_t best = 0;
    for (uint32_t i = 1; i < 8; i++)
        if (count[i] > count[best])
            best = i;
    return best;
}

void testSettings() {
    SoapySDR::Kwargs args;
    args["amplitude_mode"] = "constant";
    args["ppm"] = "1.5";
    args["not_a_setting"] = "1"; // reported with a warning, not fatal
    SoapyRPITX dev(args);

    CHECK(dev.getDriverKey() == "rpitx");
    CHECK(dev.readSetting("amplitude_mode") == "constant");
    CHECK(dev.readSetting("ppm") == "1.5");

    CHECK(throws([&] { dev.writeSetting("ppm", "300"); }));
    CHECK(throws([&] { dev.writeSetting("ppm", "abc"); }));
    CHECK(throws([&] { dev.writeSetting("harmonic", "0"); }));
    CHECK(throws([&] { dev.writeSetting("amplitude_mode", "loud"); }));
    CHECK(throws([&] { dev.writeSetting("iq_scale", "-1"); }));
    CHECK(throws([&] { dev.writeSetting("ppm_ntp", "maybe"); }));
    CHECK(throws([&] { dev.writeSetting("nope", "1"); }));
    CHECK(throws([&] { dev.readSetting("nope"); }));

    // Frequency correction API mirrors the ppm setting.
    CHECK(dev.hasFrequencyCorrection(SOAPY_SDR_TX, 0));
    dev.setFrequencyCorrection(SOAPY_SDR_TX, 0, -2.5);
    CHECK(dev.getFrequencyCorrection(SOAPY_SDR_TX, 0) == -2.5);
    CHECK(dev.readSetting("ppm") == "-2.5");
    CHECK(throws([&] { dev.setFrequencyCorrection(SOAPY_SDR_TX, 0, 500); }));
    CHECK(throws([&] { dev.setFrequencyCorrection(SOAPY_SDR_RX, 0, 1); }));

    CHECK(throws([&] { dev.setFrequency(SOAPY_SDR_TX, 0, "RF", 10.0); }));
    CHECK(throws([&] { dev.setSampleRate(SOAPY_SDR_TX, 0, 5000); }));
    dev.setFrequency(SOAPY_SDR_TX, 0, "RF", 145.5e6);
    CHECK(dev.getFrequency(SOAPY_SDR_TX, 0, "RF") == 145.5e6);
}

void testStreamArguments() {
    SoapyRPITX dev(SoapySDR::Kwargs{});
    CHECK(throws([&] { dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32); }));
    CHECK(throws([&] { dev.setupStream(SOAPY_SDR_TX, "CU8"); }));
    CHECK(throws([&] { dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {1}); }));
    CHECK(throws([&] { dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"fifo_ms", "abc"}}); }));
    CHECK(throws([&] { dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"fifo_ms", "1"}}); }));
    CHECK(throws([&] { dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"dma_force", "perhaps"}}); }));

    // A ring shorter than 10 ms is refused, at setup and when the rate is raised later.
    dev.setSampleRate(SOAPY_SDR_TX, 0, 96000);
    CHECK(throws([&] { dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"ring_size", "512"}}); }));
    dev.setSampleRate(SOAPY_SDR_TX, 0, kRate);
    SoapySDR::Stream *st = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"ring_size", "512"}});
    CHECK(st != nullptr);
    CHECK(throws([&] { dev.setSampleRate(SOAPY_SDR_TX, 0, 96000); }));
    CHECK(throws([&] { dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32); })); // already open
    dev.closeStream(st);

    // Stream argument description lists the new keys.
    bool force = false, cleanup = false;
    for (const auto &a : dev.getStreamArgsInfo(SOAPY_SDR_TX, 0)) {
        force = force || a.key == "dma_force";
        cleanup = cleanup || a.key == "signal_cleanup";
    }
    CHECK(force && cleanup);
}

// Sends a constant sample in the given format and returns the pad level used for it.
template <typename T>
uint32_t padFor(const char *format, T i, T q) {
    SoapyRPITX dev(SoapySDR::Kwargs{});
    dev.setSampleRate(SOAPY_SDR_TX, 0, kRate);
    dev.setGain(SOAPY_SDR_TX, 0, 7);
    MockDmaEngine engine(14, kRate);
    SoapySDR::Stream *st = dev.setupStream(SOAPY_SDR_TX, format, {}, {{"preroll_ms", "0"}, {"rt_priority", "0"}});
    CHECK(dev.activateStream(st) == 0);

    std::vector<T> buf(2 * 4800);
    for (size_t k = 0; k < 4800; k++) {
        buf[2 * k] = i;
        buf[2 * k + 1] = q;
    }
    const void *buffs[] = {buf.data()};
    int flags = SOAPY_SDR_END_BURST;
    CHECK(dev.writeStream(st, buffs, 4800, flags, 0, 1000000) == 4800);
    CHECK(dev.deactivateStream(st) == 0); // waits for the burst
    std::vector<MockSlot> played = engine.Take();
    CHECK(carrierOn(played) == 4800);
    dev.closeStream(st);
    return dominantPad(played);
}

void testFormats() {
    // Envelope mode, drive 7: magnitude 1.0 -> level 7, magnitude 0.5 -> level 3.
    CHECK(padFor<float>(SOAPY_SDR_CF32, 1.0f, 0.0f) == 7);
    CHECK(padFor<float>(SOAPY_SDR_CF32, 0.0f, 0.5f) == 3);
    CHECK(padFor<int16_t>(SOAPY_SDR_CS16, 16384, 0) == 3);
    CHECK(padFor<int16_t>(SOAPY_SDR_CS16, 0, -32768) == 7);
    CHECK(padFor<int8_t>(SOAPY_SDR_CS8, 64, 0) == 3);
    CHECK(padFor<int8_t>(SOAPY_SDR_CS8, -128, 0) == 7);
}

void testStatusAndSensors() {
    SoapyRPITX dev(SoapySDR::Kwargs{});
    std::vector<std::string> sensors = dev.listSensors();
    bool haveErrors = false, haveRate = false;
    for (const auto &k : sensors) {
        haveErrors = haveErrors || k == "dma_errors";
        haveRate = haveRate || k == "sample_rate";
    }
    CHECK(haveErrors && haveRate);
    CHECK(dev.readSensor("dma_running") == "false");
    CHECK(throws([&] { dev.readSensor("nope"); }));

    MockDmaEngine engine(14, kRate);
    SoapySDR::Stream *st = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"rt_priority", "0"}});
    CHECK(std::stod(dev.readSensor("sample_rate")) > kRate * 0.999);
    CHECK(dev.activateStream(st) == 0);

    size_t mask = 0;
    int flags = 0;
    long long timeNs = 0;
    CHECK(dev.readStreamStatus(st, mask, flags, timeNs, 10000) == SOAPY_SDR_TIMEOUT);

    // Starvation in the middle of a burst is reported as an underflow.
    std::vector<std::complex<float>> buf(4800, std::complex<float>(1.0f, 0.0f));
    const void *buffs[] = {buf.data()};
    flags = 0;
    CHECK(dev.writeStream(st, buffs, buf.size(), flags, 0, 1000000) == (int) buf.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(dev.readStreamStatus(st, mask, flags, timeNs, 100000) == SOAPY_SDR_UNDERFLOW);
    CHECK(mask == 1);

    // Activating the active stream again changes nothing and reports no event.
    CHECK(dev.activateStream(st) == 0);
    CHECK(dev.readStreamStatus(st, mask, flags, timeNs, 10000) == SOAPY_SDR_TIMEOUT);

    CHECK(dev.writeStream(nullptr, buffs, buf.size(), flags, 0, 1000) == SOAPY_SDR_STREAM_ERROR);
    flags = SOAPY_SDR_HAS_TIME;
    CHECK(dev.writeStream(st, buffs, buf.size(), flags, 0, 1000) == SOAPY_SDR_NOT_SUPPORTED);
    CHECK(dev.deactivateStream(st) == 0);
    flags = 0;
    CHECK(dev.writeStream(st, buffs, buf.size(), flags, 0, 1000) == SOAPY_SDR_STREAM_ERROR);
    dev.closeStream(st);
    CHECK(dev.getStreamMTU(st) == 0);
}

void testParsing() {
    SoapyRPITX dev(SoapySDR::Kwargs{});
    // Integers are decimal: a leading zero is not octal.
    dev.writeSetting("harmonic", "010");
    CHECK(dev.readSetting("harmonic") == "10");
    dev.writeSetting("harmonic", "1");
    CHECK(throws([&] { dev.writeSetting("harmonic", "0x2"); }));
    // Floats must be complete numbers.
    CHECK(throws([&] { dev.writeSetting("iq_scale", "2,5"); }));
    CHECK(throws([&] { dev.writeSetting("ppm", "1.5ppm"); }));
    dev.writeSetting("iq_scale", " 2.5 ");
    CHECK(dev.readSetting("iq_scale") == "2.5");
    // debug_level is range checked like the other settings.
    CHECK(throws([&] { dev.writeSetting("debug_level", "7"); }));
    dev.writeSetting("debug_level", "0");

    SoapySDR::Stream *st = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"fifo_ms", "0250"}, {"rt_priority", "0"}});
    CHECK(st != nullptr);
    dev.closeStream(st);
    CHECK(throws([&] { dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"ring_size", "0x1000"}}); }));
}

void testAmplitudeSettings() {
    SoapySDR::Kwargs args;
    args["amplitude_quantizer"] = "sigma_delta";
    args["ampm_lut"] = "0,0,0,0,0,0,10,-90";
    SoapyRPITX dev(args);
    CHECK(dev.readSetting("amplitude_quantizer") == "sigma_delta");
    CHECK(dev.readSetting("ampm_lut") == "0,0,0,0,0,0,10,-90");

    dev.writeSetting("amplitude_lut", "0.05,0.2,0.45,0.6,0.75,0.85,0.95,1");
    CHECK(dev.readSetting("amplitude_lut") == "0.05,0.2,0.45,0.6,0.75,0.85,0.95,1");
    CHECK(throws([&] { dev.writeSetting("amplitude_lut", "1,2,3"); }));
    CHECK(throws([&] { dev.writeSetting("amplitude_lut", "1,2,3,4,5,6,7,"); }));
    CHECK(dev.readSetting("amplitude_lut") == "0.05,0.2,0.45,0.6,0.75,0.85,0.95,1");
    dev.writeSetting("amplitude_lut", "");
    CHECK(dev.readSetting("amplitude_lut").empty());
    CHECK(throws([&] { dev.writeSetting("amplitude_quantizer", "dither"); }));
    CHECK(throws([&] { dev.writeSetting("sigma_delta_order", "3"); }));
    CHECK(throws([&] { dev.writeSetting("envelope_delay", "99"); }));
    dev.writeSetting("envelope_delay", "1.25");
    CHECK(dev.readSetting("envelope_delay") == "1.25");
    dev.writeSetting("eer_lut", "0,0.6,0.9,1");
    CHECK(throws([&] { dev.writeSetting("eer_drive", "8"); }));
    CHECK(throws([&] { dev.writeSetting("eer_gain", "0"); }));
    dev.writeSetting("rfdac_lut", "0.6,0.3,0.95");
    CHECK(dev.readSetting("rfdac_lut") == "0.6,0.3,0.95");

    bool haveLut = false, haveEer = false;
    for (const auto &a : dev.getSettingInfo()) {
        haveLut = haveLut || a.key == "amplitude_lut";
        haveEer = haveEer || a.key == "eer_drive";
    }
    CHECK(haveLut && haveEer);
}

void testEnvelopeStreamArguments() {
    SoapyRPITX dev(SoapySDR::Kwargs{});
    CHECK(throws([&] { dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"envelope_output", "dac"}}); }));
    CHECK(throws([&] { dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"pwm_gpio", "13"}}); }));
    CHECK(throws([&] { dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"slots_per_sample", "5"}}); }));
    dev.setSampleRate(SOAPY_SDR_TX, 0, 96000);
    CHECK(throws([&] { dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"slots_per_sample", "4"}}); }));
    dev.setSampleRate(SOAPY_SDR_TX, 0, kRate);

    SoapySDR::Stream *st = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {},
                                           {{"envelope_output", "pwm"}, {"pwm_bits", "10"}, {"slots_per_sample", "2"}, {"rt_priority", "0"}});
    CHECK(st != nullptr);
    CHECK(std::fabs(std::stod(dev.readSensor("sample_rate")) - kRate) < kRate * 1e-3);
    CHECK(dev.readSensor("envelope_clip") == "0");
    // The slot rate of the open stream limits the sample rate.
    CHECK(throws([&] { dev.setSampleRate(SOAPY_SDR_TX, 0, 192000); }));
    dev.closeStream(st);

    bool haveEnv = false, haveSlots = false;
    for (const auto &a : dev.getStreamArgsInfo(SOAPY_SDR_TX, 0)) {
        haveEnv = haveEnv || a.key == "envelope_output";
        haveSlots = haveSlots || a.key == "slots_per_sample";
    }
    CHECK(haveEnv && haveSlots);
}

void testCloseDuringDrain() {
    // closeStream() stops the transmitter at once, even while deactivateStream() waits for the burst.
    SoapyRPITX dev(SoapySDR::Kwargs{});
    MockDmaEngine engine(14, kRate);
    SoapySDR::Stream *st = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"rt_priority", "0"}, {"fifo_ms", "3000"}});
    CHECK(dev.activateStream(st) == 0);
    std::vector<std::complex<float>> buf(2 * kRate, std::complex<float>(1.0f, 0.0f));
    const void *buffs[] = {buf.data()};
    int flags = SOAPY_SDR_END_BURST;
    CHECK(dev.writeStream(st, buffs, buf.size(), flags, 0, 1000000) == (int) buf.size());

    std::thread drain([&] { dev.deactivateStream(st); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto t0 = std::chrono::steady_clock::now();
    dev.closeStream(st);
    SoapySDR::Stream *again = nullptr;
    for (int i = 0; i < 50 && again == nullptr; i++) {
        try {
            again = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"rt_priority", "0"}});
        } catch (const std::runtime_error &) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    CHECK(again != nullptr);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
    drain.join();
    if (again != nullptr)
        dev.closeStream(again);
}

void testCloseWhileWriting() {
    // closeStream() while other threads are blocked in writeStream() (full FIFO) and readStreamStatus(): both
    // return and the stream is destroyed only afterwards (run under ThreadSanitizer / AddressSanitizer).
    for (int round = 0; round < 5; round++) {
        SoapyRPITX dev(SoapySDR::Kwargs{});
        MockDmaEngine engine(14, kRate);
        SoapySDR::Stream *st = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"rt_priority", "0"}, {"fifo_ms", "50"}});
        CHECK(dev.activateStream(st) == 0);

        std::atomic<bool> stop{false};
        std::atomic<int> writers{0};
        std::thread writer([&] {
            std::vector<std::complex<float>> buf(1024, std::complex<float>(0.5f, 0.0f));
            const void *buffs[] = {buf.data()};
            writers++;
            while (!stop.load()) {
                int flags = 0;
                int r = dev.writeStream(st, buffs, buf.size(), flags, 0, 2000000);
                if (r == SOAPY_SDR_STREAM_ERROR)
                    break;
            }
        });
        std::thread status([&] {
            size_t mask = 0;
            int flags = 0;
            long long t = 0;
            writers++;
            while (!stop.load()) {
                if (dev.readStreamStatus(st, mask, flags, t, 5000000) == SOAPY_SDR_STREAM_ERROR)
                    break;
            }
        });
        while (writers.load() < 2)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(50 + 20 * round)); // the writer is blocked on the full FIFO

        auto t0 = std::chrono::steady_clock::now();
        dev.closeStream(st);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
        stop.store(true);
        writer.join();
        status.join();

        // The handle is invalid now.
        int flags = 0;
        CHECK(dev.writeStream(st, nullptr, 0, flags, 0, 0) == SOAPY_SDR_STREAM_ERROR);
        size_t mask = 0;
        long long t = 0;
        CHECK(dev.readStreamStatus(st, mask, flags, t, 0) == SOAPY_SDR_STREAM_ERROR);
    }
}

} // namespace

void writeFile(const char *name, const std::string &text) {
    std::ofstream f(name);
    f << text;
}

void testProfileParser() {
    SoapyRPITX_Profile p;
    std::string err;
    std::istringstream good("# comment\n\n  ppm = 1.5 \namplitude_lut=0.1,0.2\nstream.fifo_ms=300\ngain=3\nppm=2\nx=a=b\n");
    CHECK(SoapyRPITX_ParseProfile(good, p, err));
    CHECK(p.settings.size() == 3);
    CHECK(p.settings[0].first == "ppm" && p.settings[0].second == "2"); // the last value wins, first position kept
    CHECK(p.settings[1].second == "0.1,0.2");
    CHECK(p.settings[2].first == "x" && p.settings[2].second == "a=b");
    CHECK(p.stream.size() == 1 && p.stream["fifo_ms"] == "300");
    CHECK(p.hasGain && p.gain == "3");
    std::istringstream bad("ppm=1\njunk\n");
    CHECK(!SoapyRPITX_ParseProfile(bad, p, err) && err.find("line 2") != std::string::npos);
    std::istringstream emptyKey("=1\n");
    CHECK(!SoapyRPITX_ParseProfile(emptyKey, p, err));
    std::istringstream emptyStream("stream.=1\n");
    CHECK(!SoapyRPITX_ParseProfile(emptyStream, p, err));
    CHECK(!SoapyRPITX_LoadProfile("no_such_profile.rpitx", p, err));
}

void testProfileDevice() {
    const char *name = "test_profile.rpitx";
    writeFile(name, "# rpitx profile\nppm=2.5\niq_scale=0.8\namplitude_lut=0.1,0.2,0.3,0.4,0.5,0.6,0.7,1\nunknown_key=1\n"
                    "stream.slots_per_sample=4\nstream.bogus=1\ngain=3\n");
    SoapySDR::Kwargs args;
    args["profile"] = name;
    args["iq_scale"] = "0.9"; // an explicit device argument takes precedence over the profile
    SoapyRPITX dev(args);
    CHECK(dev.readSetting("ppm") == "2.5");
    CHECK(dev.readSetting("iq_scale") == "0.9");
    CHECK(dev.readSetting("amplitude_lut") == "0.1,0.2,0.3,0.4,0.5,0.6,0.7,1");
    CHECK(dev.readSetting("profile") == name);
    CHECK(dev.getGain(SOAPY_SDR_TX, 0, "PAD") == 3);

    // The profile's slots_per_sample is the default stream argument: 60 kS/s x 4 exceeds the slot rate limit.
    dev.setSampleRate(SOAPY_SDR_TX, 0, 60000);
    CHECK(throws([&] { dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32); }));
    SoapySDR::Stream *st = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"slots_per_sample", "1"}});
    CHECK(st != nullptr);
    dev.closeStream(st);

    // An empty profile forgets the stream defaults.
    dev.writeSetting("profile", "");
    CHECK(dev.readSetting("profile").empty());
    st = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32);
    CHECK(st != nullptr);
    dev.closeStream(st);

    // Loading at run time applies the settings at once.
    writeFile(name, "ppm=-3\ngain=5\n");
    dev.writeSetting("profile", name);
    CHECK(dev.readSetting("ppm") == "-3");
    CHECK(dev.getGain(SOAPY_SDR_TX, 0, "PAD") == 5);

    // Errors: missing file, malformed line, refused value (reported with the profile name), gain out of range.
    CHECK(throws([&] { dev.writeSetting("profile", "no_such_profile.rpitx"); }));
    writeFile(name, "ppm=1\nnot a key value line\n");
    CHECK(throws([&] { dev.writeSetting("profile", name); }));
    writeFile(name, "iq_scale=500\n");
    bool named = false;
    try {
        dev.writeSetting("profile", name);
    } catch (const std::runtime_error &e) {
        named = std::string(e.what()).find("profile 'test_profile.rpitx'") != std::string::npos;
    }
    CHECK(named);
    writeFile(name, "gain=9\n");
    CHECK(throws([&] { dev.writeSetting("profile", name); }));

    // A refused profile changes nothing: not the settings before the bad line, nor the gain, name or stream defaults.
    dev.writeSetting("profile", "");
    writeFile(name, "ppm=-3\ngain=5\n");
    dev.writeSetting("profile", name);
    const std::string before = dev.readSetting("ppm");
    const double gainBefore = dev.getGain(SOAPY_SDR_TX, 0, "PAD");
    writeFile(name, "ppm=11\nharmonic=3\ngain=1\nstream.fifo_ms=300\niq_scale=500\n");
    CHECK(throws([&] { dev.writeSetting("profile", name); }));
    writeFile(name, "ppm=11\nharmonic=3\ngain=1\nstream.fifo_ms=notanumber\n"); // bad stream value: refused at load
    CHECK(throws([&] { dev.writeSetting("profile", name); }));
    writeFile(name, "ppm=11\nharmonic=3\ngain=1\nstream.slots_per_sample=9\n");
    CHECK(throws([&] { dev.writeSetting("profile", name); }));
    CHECK(dev.readSetting("ppm") == before);
    CHECK(dev.readSetting("harmonic") == "1");
    CHECK(dev.getGain(SOAPY_SDR_TX, 0, "PAD") == gainBefore);
    CHECK(dev.readSetting("profile") == name); // the profile loaded before the failed attempts
    // The same file without the bad line applies as a whole.
    writeFile(name, "ppm=11\nharmonic=3\ngain=1\n");
    dev.writeSetting("profile", name);
    CHECK(dev.readSetting("ppm") == "11" && dev.readSetting("harmonic") == "3");
    CHECK(dev.getGain(SOAPY_SDR_TX, 0, "PAD") == 1);
    dev.writeSetting("harmonic", "1");
    dev.writeSetting("ppm", "-3");
    dev.setGain(SOAPY_SDR_TX, 0, 5);
    SoapySDR::Kwargs badArgs;
    badArgs["profile"] = "no_such_profile.rpitx";
    CHECK(throws([&] { SoapyRPITX d(badArgs); }));

    bool listed = false;
    for (const auto &a : dev.getSettingInfo())
        listed = listed || a.key == "profile";
    CHECK(listed);
    std::remove(name);
}

void testInputLimits() {
    SoapyRPITX dev(SoapySDR::Kwargs{});

    // Values beyond the range of int are refused, not truncated.
    CHECK(throws([&] { dev.writeSetting("sigma_delta_order", "4294967297"); }));
    CHECK(throws([&] { dev.writeSetting("eer_drive", "4294967303"); }));
    CHECK(dev.readSetting("sigma_delta_order") == "2" && dev.readSetting("eer_drive") == "7");

    // Any finite gain maps to the nearest level of the range.
    dev.setGain(SOAPY_SDR_TX, 0, 1e300);
    CHECK(dev.getGain(SOAPY_SDR_TX, 0) == 7);
    dev.setGain(SOAPY_SDR_TX, 0, -1e300);
    CHECK(dev.getGain(SOAPY_SDR_TX, 0) == 0);

    // Deadlines stay valid for any timeout.
    const auto now = std::chrono::steady_clock::now();
    const long long longest = std::min<long long>((long long) LONG_MAX, RPITX_MAX_TIMEOUT_US);
    CHECK(SoapyRPITX_DeadlineAfter(LONG_MAX) > now + std::chrono::microseconds(longest / 2));
    const auto immediate = SoapyRPITX_DeadlineAfter(-5);
    CHECK(immediate <= std::chrono::steady_clock::now());
}

void testProfileFileChecks() {
    SoapyRPITX dev(SoapySDR::Kwargs{});
    SoapyRPITX_Profile p;
    std::string err;

    // Devices are refused without reading them.
    auto t0 = std::chrono::steady_clock::now();
    CHECK(throws([&] { dev.writeSetting("profile", "/dev/zero"); }));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));

    // A named pipe is refused without blocking.
    const char *fifoName = "test_profile.fifo";
    std::remove(fifoName);
    CHECK(mkfifo(fifoName, 0600) == 0);
    CHECK(!SoapyRPITX_LoadProfile(fifoName, p, err) && err == "not a regular file");
    std::remove(fifoName);

    // Oversized and group-writable files are refused.
    const char *big = "test_profile_big.rpitx";
    writeFile(big, std::string(RPITX_PROFILE_MAX_BYTES + 1, '#'));
    CHECK(!SoapyRPITX_LoadProfile(big, p, err) && err.find("larger") != std::string::npos);
    std::remove(big);
    const char *shared = "test_profile_shared.rpitx";
    writeFile(shared, "ppm=1\n");
    chmod(shared, 0664);
    CHECK(!SoapyRPITX_LoadProfile(shared, p, err) && err.find("writable") != std::string::npos);
    chmod(shared, 0644);
    CHECK(SoapyRPITX_LoadProfile(shared, p, err) && p.settings.size() == 1);
    std::remove(shared);

    // A symbolic link is not followed.
    const char *target = "test_profile_target.rpitx";
    const char *link = "test_profile_link.rpitx";
    writeFile(target, "ppm=1\n");
    std::remove(link);
    CHECK(symlink(target, link) == 0);
    CHECK(!SoapyRPITX_LoadProfile(link, p, err));
    std::remove(link);
    std::remove(target);
}

int main() {
    testSettings();
    testStreamArguments();
    testFormats();
    testStatusAndSensors();
    testParsing();
    testAmplitudeSettings();
    testEnvelopeStreamArguments();
    testCloseDuringDrain();
    testCloseWhileWriting();
    testProfileParser();
    testProfileDevice();
    testInputLimits();
    testProfileFileChecks();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all driver tests passed\n");
    return 0;
}
