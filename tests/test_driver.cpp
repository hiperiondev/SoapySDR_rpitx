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
// generated in the build directory (see CMakeLists.txt).

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
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

} // namespace

int main() {
    testSettings();
    testStreamArguments();
    testFormats();
    testStatusAndSensors();
    testParsing();
    testAmplitudeSettings();
    testEnvelopeStreamArguments();
    testCloseDuringDrain();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all driver tests passed\n");
    return 0;
}
