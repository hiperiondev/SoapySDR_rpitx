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
// profile files, the profile=<file> device argument / setting and the limits of numeric inputs and timeouts,
// the transceiver mode with a simulated SoapySDR receiver registered by this test (forwarding of the RX API,
// enumeration, receive muting while keyed) and the antenna relay GPIO on the simulated GPIO registers.

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
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Registry.hpp>

#include "SoapyRPITX.hpp"
#include "SoapyRPITX_Ptt.hpp"
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

// ---------------------------------------------------------------------------------------------------------------
// Simulated receiver: a SoapySDR driver registered as "rpitx_test_rx" that streams a constant CS16 / CF32 value.
// ---------------------------------------------------------------------------------------------------------------

constexpr int16_t kRxValue = 1000;

std::mutex g_rxLock;
SoapySDR::Kwargs g_rxMakeArgs;
int g_rxAlive = 0;

class FakeRx : public SoapySDR::Device {
public:
    FakeRx(const SoapySDR::Kwargs &args) {
        std::lock_guard<std::mutex> lock(g_rxLock);
        g_rxMakeArgs = args;
        if (args.count("fail"))
            throw std::runtime_error("simulated open failure");
        g_rxAlive++;
    }
    ~FakeRx() {
        std::lock_guard<std::mutex> lock(g_rxLock);
        g_rxAlive--;
    }
    std::string getDriverKey() const {
        return "rpitx_test_rx";
    }
    std::string getHardwareKey() const {
        return "FakeRx";
    }
    SoapySDR::Kwargs getHardwareInfo() const {
        return {{"tuner", "R820T"}};
    }
    size_t getNumChannels(const int direction) const {
        return direction == SOAPY_SDR_RX ? 1 : 0;
    }
    std::vector<std::string> getStreamFormats(const int, const size_t) const {
        return {SOAPY_SDR_CS16, SOAPY_SDR_CF32, SOAPY_SDR_CU8};
    }
    std::string getNativeStreamFormat(const int, const size_t, double &fullScale) const {
        fullScale = 32768;
        return SOAPY_SDR_CS16;
    }
    SoapySDR::Stream *setupStream(const int direction, const std::string &format, const std::vector<size_t> &, const SoapySDR::Kwargs &) {
        if (direction != SOAPY_SDR_RX)
            throw std::runtime_error("RX only");
        streamFormat = format;
        return reinterpret_cast<SoapySDR::Stream *>(&streamFormat);
    }
    void closeStream(SoapySDR::Stream *) {
        closed++;
    }
    int activateStream(SoapySDR::Stream *, const int, const long long, const size_t) {
        active = true;
        return 0;
    }
    int deactivateStream(SoapySDR::Stream *, const int, const long long) {
        active = false;
        return 0;
    }
    size_t getStreamMTU(SoapySDR::Stream *) const {
        return 512;
    }
    int readStream(SoapySDR::Stream *, void *const *buffs, const size_t numElems, int &flags, long long &timeNs, const long timeoutUs) {
        if (!active)
            return SOAPY_SDR_STREAM_ERROR;
        if (block) {
            // No data: the call blocks for its whole timeout, as a receiver without samples does.
            long seen = maxTimeoutUs.load();
            while (timeoutUs > seen && !maxTimeoutUs.compare_exchange_weak(seen, timeoutUs)) {
            }
            std::this_thread::sleep_for(std::chrono::microseconds(std::max(0L, timeoutUs)));
            return SOAPY_SDR_TIMEOUT;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        size_t n = std::min<size_t>(numElems, 256);
        if (streamFormat != SOAPY_SDR_CS16 && streamFormat != SOAPY_SDR_CU8 && streamFormat != SOAPY_SDR_CF32) {
            // Any other format (packed ones included): a recognisable byte pattern over the whole buffer.
            memset(buffs[0], 0x5A, n * SoapySDR::formatToSize(streamFormat));
        } else if (streamFormat == SOAPY_SDR_CS16) {
            int16_t *p = static_cast<int16_t *>(buffs[0]);
            for (size_t i = 0; i < 2 * n; i++)
                p[i] = kRxValue;
        } else if (streamFormat == SOAPY_SDR_CU8) {
            uint8_t *p = static_cast<uint8_t *>(buffs[0]);
            for (size_t i = 0; i < 2 * n; i++)
                p[i] = 200;
        } else {
            float *p = static_cast<float *>(buffs[0]);
            for (size_t i = 0; i < 2 * n; i++)
                p[i] = 0.25f;
        }
        flags = SOAPY_SDR_HAS_TIME;
        timeNs = 1234;
        return (int) n;
    }
    std::vector<std::string> listAntennas(const int, const size_t) const {
        return {"RX"};
    }
    std::string getAntenna(const int, const size_t) const {
        return "RX";
    }
    std::vector<std::string> listGains(const int, const size_t) const {
        return {"TUNER"};
    }
    bool hasGainMode(const int, const size_t) const {
        return true;
    }
    void setGainMode(const int, const size_t, const bool automatic) {
        agc = automatic;
    }
    bool getGainMode(const int, const size_t) const {
        return agc;
    }
    void setGain(const int, const size_t, const std::string &, const double value) {
        gain = value;
    }
    double getGain(const int, const size_t, const std::string &) const {
        return gain;
    }
    SoapySDR::Range getGainRange(const int, const size_t, const std::string &) const {
        return SoapySDR::Range(0, 49.6);
    }
    void setFrequency(const int, const size_t, const std::string &, const double frequency, const SoapySDR::Kwargs &) {
        freq = frequency;
    }
    double getFrequency(const int, const size_t, const std::string &) const {
        return freq;
    }
    std::vector<std::string> listFrequencies(const int, const size_t) const {
        return {"RF"};
    }
    SoapySDR::RangeList getFrequencyRange(const int, const size_t, const std::string &) const {
        return {SoapySDR::Range(24e6, 1766e6)};
    }
    void setSampleRate(const int, const size_t, const double r) {
        if (r < 225001 || r > 3200000)
            throw std::runtime_error("rate out of range");
        rate = r;
    }
    double getSampleRate(const int, const size_t) const {
        return rate;
    }
    SoapySDR::RangeList getSampleRateRange(const int, const size_t) const {
        return {SoapySDR::Range(225001, 300000), SoapySDR::Range(900001, 3200000)};
    }
    bool hasFrequencyCorrection(const int, const size_t) const {
        return true;
    }
    void setFrequencyCorrection(const int, const size_t, const double value) {
        ppm = value;
    }
    double getFrequencyCorrection(const int, const size_t) const {
        return ppm;
    }
    SoapySDR::ArgInfoList getSettingInfo() const {
        SoapySDR::ArgInfo a;
        a.key = "direct_samp";
        a.name = "Direct sampling";
        a.value = "0";
        return {a};
    }
    void writeSetting(const std::string &key, const std::string &value) {
        if (key == "block") {
            block = value == "1";
            return;
        }
        if (key != "direct_samp")
            throw std::runtime_error("unknown");
        directSamp = value;
    }
    std::string readSetting(const std::string &key) const {
        if (key == "max_timeout_us")
            return std::to_string(maxTimeoutUs.load());
        if (key != "direct_samp")
            throw std::runtime_error("unknown");
        return directSamp;
    }

    std::string streamFormat;
    std::atomic<bool> active{false};
    std::atomic<bool> block{false};
    std::atomic<long> maxTimeoutUs{0};
    int closed = 0;
    bool agc = false;
    double gain = 0, freq = 100e6, rate = 2.4e6, ppm = 0;
    std::string directSamp = "0";
};

SoapySDR::KwargsList findFakeRx(const SoapySDR::Kwargs &args) {
    SoapySDR::KwargsList out;
    for (const char *serial : {"RX0001", "RX0002"}) {
        auto it = args.find("serial");
        if (it != args.end() && it->second != serial)
            continue;
        out.push_back({{"label", std::string("Fake RTL ") + serial}, {"serial", serial}, {"tuner", "R820T"}});
    }
    return out;
}

SoapySDR::Device *makeFakeRx(const SoapySDR::Kwargs &args) {
    return new FakeRx(args);
}

SoapySDR::Registry registerFakeRx("rpitx_test_rx", &findFakeRx, &makeFakeRx, SOAPY_SDR_ABI_VERSION);

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

void testStaleHandle() {
    // A handle kept after closeStream() must not be accepted by a stream opened later, whatever address the
    // new stream gets (ABA).
    SoapyRPITX dev(SoapySDR::Kwargs{});
    dev.setSampleRate(SOAPY_SDR_TX, 0, kRate);
    for (int round = 0; round < 5; round++) {
        SoapySDR::Stream *old = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"rt_priority", "0"}});
        CHECK(old != nullptr);
        dev.closeStream(old);
        SoapySDR::Stream *fresh = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"rt_priority", "0"}});
        CHECK(fresh != nullptr && fresh != old);

        std::vector<std::complex<float>> buf(16, std::complex<float>(0.5f, 0.0f));
        const void *buffs[1] = {buf.data()};
        int flags = 0;
        CHECK(dev.writeStream(old, buffs, buf.size(), flags, 0, 1000) == SOAPY_SDR_STREAM_ERROR);
        CHECK(dev.getStreamMTU(old) == 0);
        CHECK(dev.activateStream(old) == SOAPY_SDR_STREAM_ERROR);
        CHECK(dev.getStreamMTU(fresh) != 0);
        dev.closeStream(old); // stale: must not close the new stream
        CHECK(dev.getStreamMTU(fresh) != 0);
        dev.closeStream(fresh);
    }
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

    // A file owned by another user is refused (needs root to create).
    if (geteuid() == 0) {
        const char *foreign = "test_profile_foreign.rpitx";
        writeFile(foreign, "ppm=1\n");
        CHECK(chown(foreign, 12345, 12345) == 0);
        CHECK(!SoapyRPITX_LoadProfile(foreign, p, err) && err.find("owned") != std::string::npos);
        std::remove(foreign);
    }

    // A directory writable by others lets anyone swap the file: refused unless it is sticky.
    const char *dir = "test_profile_dir";
    const std::string inside = std::string(dir) + "/profile.rpitx";
    rmdir(dir);
    CHECK(mkdir(dir, 0700) == 0);
    writeFile(inside.c_str(), "ppm=1\n");
    chmod(dir, 0777);
    CHECK(!SoapyRPITX_LoadProfile(inside, p, err) && err.find("writable by others") != std::string::npos);
    chmod(dir, 01777);
    CHECK(SoapyRPITX_LoadProfile(inside, p, err) && p.settings.size() == 1);
    chmod(dir, 0755);
    CHECK(SoapyRPITX_LoadProfile(inside, p, err) && p.settings.size() == 1);
    // A symbolic link in a directory component is not followed either.
    const char *dirLink = "test_profile_dirlink";
    std::remove(dirLink);
    CHECK(symlink(dir, dirLink) == 0);
    CHECK(!SoapyRPITX_LoadProfile(std::string(dirLink) + "/profile.rpitx", p, err));
    std::remove(dirLink);
    std::remove(inside.c_str());
    rmdir(dir);

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

int rxAlive() {
    std::lock_guard<std::mutex> lock(g_rxLock);
    return g_rxAlive;
}

SoapySDR::Kwargs rxMakeArgs() {
    std::lock_guard<std::mutex> lock(g_rxLock);
    return g_rxMakeArgs;
}

// Reads one block from the RX stream and returns its first I value (or INT32_MIN on error).
int32_t readFirstI(SoapyRPITX &dev, SoapySDR::Stream *rx) {
    std::vector<int16_t> buf(2 * 256, -1);
    void *buffs[] = {buf.data()};
    int flags = 0;
    long long timeNs = 0;
    int n = dev.readStream(rx, buffs, 256, flags, timeNs, 100000);
    if (n <= 0)
        return INT32_MIN;
    CHECK(flags == SOAPY_SDR_HAS_TIME && timeNs == 1234);
    for (int i = 1; i < 2 * n; i++)
        if (buf[i] != buf[0])
            return INT32_MIN;
    return buf[0];
}

void testReceiverForwarding() {
    {
        // Without a receiver the device stays TX only.
        SoapyRPITX dev(SoapySDR::Kwargs{});
        CHECK(dev.getNumChannels(SOAPY_SDR_RX) == 0);
        CHECK(!dev.getFullDuplex(SOAPY_SDR_TX, 0));
        CHECK(dev.listAntennas(SOAPY_SDR_RX, 0).empty());
        CHECK(throws([&] { dev.setFrequency(SOAPY_SDR_RX, 0, 100e6); }));
        CHECK(throws([&] { dev.writeSetting("rx.direct_samp", "1"); }));
        CHECK(dev.getHardwareInfo().count("rx_driver") == 0);
    }

    CHECK(throws([] { SoapyRPITX dev(SoapySDR::Kwargs{{"rx", "rpitx"}}); }));
    CHECK(throws([] { SoapyRPITX dev(SoapySDR::Kwargs{{"rx", "no_such_driver"}}); }));
    CHECK(throws([] { SoapyRPITX dev(SoapySDR::Kwargs{{"rx", "rpitx_test_rx"}, {"rx.fail", "1"}}); }));
    CHECK(rxAlive() == 0);
    {
        SoapyRPITX dev(SoapySDR::Kwargs{{"rx", "none"}});
        CHECK(dev.getNumChannels(SOAPY_SDR_RX) == 0);
    }

    {
        SoapyRPITX dev(SoapySDR::Kwargs{{"rx", "rpitx_test_rx"}, {"rx.serial", "RX0002"}, {"rx.direct_samp", "2"}});
        CHECK(rxAlive() == 1);
        SoapySDR::Kwargs made = rxMakeArgs();
        CHECK(made["driver"] == "rpitx_test_rx" && made["serial"] == "RX0002" && made["direct_samp"] == "2" && made.count("rx") == 0);

        CHECK(dev.getNumChannels(SOAPY_SDR_RX) == 1 && dev.getNumChannels(SOAPY_SDR_TX) == 1);
        CHECK(dev.getFullDuplex(SOAPY_SDR_RX, 0) && dev.getFullDuplex(SOAPY_SDR_TX, 0));
        SoapySDR::Kwargs info = dev.getHardwareInfo();
        CHECK(info["rx_driver"] == "rpitx_test_rx" && info["rx_hardware"] == "FakeRx" && info["rx.tuner"] == "R820T");

        // Control API forwarded for RX, TX unchanged.
        dev.setFrequency(SOAPY_SDR_RX, 0, 145.5e6);
        CHECK(dev.getFrequency(SOAPY_SDR_RX, 0) == 145.5e6);
        CHECK(dev.getFrequency(SOAPY_SDR_RX, 0, "RF") == 145.5e6);
        dev.setFrequency(SOAPY_SDR_TX, 0, 144.8e6);
        CHECK(dev.getFrequency(SOAPY_SDR_TX, 0) == 144.8e6 && dev.getFrequency(SOAPY_SDR_RX, 0) == 145.5e6);
        CHECK(dev.getFrequencyRange(SOAPY_SDR_RX, 0).front().minimum() == 24e6);
        CHECK(dev.listAntennas(SOAPY_SDR_RX, 0) == std::vector<std::string>{"RX"});
        CHECK(dev.getAntenna(SOAPY_SDR_RX, 0) == "RX" && dev.getAntenna(SOAPY_SDR_TX, 0) == RPITX_ANTENNA_NAME);
        CHECK(dev.listGains(SOAPY_SDR_RX, 0) == std::vector<std::string>{"TUNER"});
        dev.setGain(SOAPY_SDR_RX, 0, "TUNER", 30);
        CHECK(dev.getGain(SOAPY_SDR_RX, 0, "TUNER") == 30);
        CHECK(dev.hasGainMode(SOAPY_SDR_RX, 0) && !dev.hasGainMode(SOAPY_SDR_TX, 0));
        dev.setGainMode(SOAPY_SDR_RX, 0, true);
        CHECK(dev.getGainMode(SOAPY_SDR_RX, 0));
        dev.setGain(SOAPY_SDR_TX, 0, 3);
        CHECK(dev.getGain(SOAPY_SDR_TX, 0) == 3);
        CHECK(throws([&] { dev.setSampleRate(SOAPY_SDR_RX, 0, 100000); }));
        dev.setSampleRate(SOAPY_SDR_RX, 0, 240000);
        CHECK(dev.getSampleRate(SOAPY_SDR_RX, 0) == 240000 && dev.getSampleRateRange(SOAPY_SDR_RX, 0).size() == 2);
        CHECK(dev.hasFrequencyCorrection(SOAPY_SDR_RX, 0));
        dev.setFrequencyCorrection(SOAPY_SDR_RX, 0, 55);
        CHECK(dev.getFrequencyCorrection(SOAPY_SDR_RX, 0) == 55);
        dev.setFrequencyCorrection(SOAPY_SDR_TX, 0, 1.5);
        CHECK(dev.getFrequencyCorrection(SOAPY_SDR_TX, 0) == 1.5);
        CHECK(dev.getStreamFormats(SOAPY_SDR_RX, 0).size() == 3);
        double fullScale = 0;
        CHECK(dev.getNativeStreamFormat(SOAPY_SDR_RX, 0, fullScale) == SOAPY_SDR_CS16 && fullScale == 32768);

        // Receiver settings with the rx. prefix.
        CHECK(dev.readSetting("rx.direct_samp") == "0");
        dev.writeSetting("rx.direct_samp", "1");
        CHECK(dev.readSetting("rx.direct_samp") == "1");
        bool listed = false;
        for (const auto &a : dev.getSettingInfo())
            listed = listed || a.key == "rx.direct_samp";
        CHECK(listed);
        CHECK(throws([&] { dev.writeSetting("rx.nope", "1"); }));

        // RX and TX streams at the same time; handles are distinct and not interchangeable.
        CHECK(throws([&] { dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, {1}); }));
        SoapySDR::Stream *rx = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16);
        CHECK(rx != nullptr);
        CHECK(throws([&] { dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16); }));
        SoapySDR::Stream *tx = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"rt_priority", "0"}});
        CHECK(tx != nullptr && tx != rx);
        CHECK(dev.getStreamMTU(rx) == 512);
        CHECK(dev.activateStream(rx) == 0);
        CHECK(readFirstI(dev, rx) == kRxValue);
        std::vector<std::complex<float>> buf(16);
        const void *wbuffs[] = {buf.data()};
        int flags = 0;
        CHECK(dev.writeStream(rx, wbuffs, buf.size(), flags, 0, 1000) == SOAPY_SDR_STREAM_ERROR);
        std::vector<int16_t> rbuf(32);
        void *rbuffs[] = {rbuf.data()};
        long long timeNs = 0;
        CHECK(dev.readStream(tx, rbuffs, 16, flags, timeNs, 1000) == SOAPY_SDR_STREAM_ERROR);
        CHECK(dev.deactivateStream(rx) == 0);
        dev.closeStream(rx);
        CHECK(dev.readStream(rx, rbuffs, 16, flags, timeNs, 1000) == SOAPY_SDR_STREAM_ERROR);
        dev.closeStream(tx);

        // An RX stream left open is closed with the device.
        SoapySDR::Stream *again = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16);
        CHECK(again != nullptr && again != rx);
        CHECK(dev.activateStream(again) == 0);
    }
    CHECK(rxAlive() == 0);
}

void testReceiverEnumeration() {
    SoapySDR::FindFunctions finders = SoapySDR::Registry::listFindFunctions();
    CHECK(finders.count("rpitx") == 1);
    if (finders.count("rpitx") == 0)
        return;
    SoapySDR::FindFunction find = finders["rpitx"];

    // The TX-only entry stays first; the transceiver entries name the receiver to open.
    SoapySDR::KwargsList plain = find(SoapySDR::Kwargs{});
    CHECK(!plain.empty() && plain.front().count("rx") == 0);

    SoapySDR::KwargsList trx = find(SoapySDR::Kwargs{{"rx", "rpitx_test_rx"}});
    CHECK(trx.size() == 2);
    if (trx.size() == 2) {
        CHECK(trx[0]["rx"] == "rpitx_test_rx" && trx[0]["rx.serial"] == "RX0001" && trx[1]["rx.serial"] == "RX0002");
        CHECK(trx[0]["label"].find("transceiver") != std::string::npos && trx[0]["tuner"] == "R820T");
    }
    SoapySDR::KwargsList one = find(SoapySDR::Kwargs{{"rx", "rpitx_test_rx"}, {"rx.serial", "RX0002"}});
    CHECK(one.size() == 1 && one.front()["rx.serial"] == "RX0002");
    CHECK(find(SoapySDR::Kwargs{{"rx", "none"}}).size() == 1);
    // A missing receiver still yields one entry: opening it reports the reason.
    SoapySDR::KwargsList missing = find(SoapySDR::Kwargs{{"rx", "no_such_driver"}});
    CHECK(missing.size() == 1 && missing.front()["rx"] == "no_such_driver");

    // An entry opens the transceiver it describes.
    if (trx.size() == 2) {
        SoapyRPITX dev(trx[1]);
        CHECK(dev.getNumChannels(SOAPY_SDR_RX) == 1 && rxMakeArgs()["serial"] == "RX0002");
    }
}

constexpr int kRelayPin = 17;
constexpr uint32_t kRelayBit = 1u << kRelayPin;
constexpr unsigned kGpset0 = 0x1C / 4;
constexpr unsigned kGpclr0 = 0x28 / 4;

void clearSetClr() {
    if (MockRegisters(GENERAL_BASE) == nullptr)
        return; // not mapped yet: nothing written
    MockPoke(GENERAL_BASE, kGpset0, 0);
    MockPoke(GENERAL_BASE, kGpclr0, 0);
}

// Waits until the ptt sensor reads @p keyed; returns false on timeout.
bool waitPtt(SoapyRPITX &dev, bool keyed, int timeoutMs) {
    for (int i = 0; i < timeoutMs; i++) {
        if ((dev.readSensor("ptt") == "true") == keyed)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void testRelaySettings() {
    CHECK(throws([] { SoapyRPITX dev(SoapySDR::Kwargs{{"relay_gpio", "4"}}); }));
    CHECK(throws([] { SoapyRPITX dev(SoapySDR::Kwargs{{"relay_gpio", "28"}}); }));
    CHECK(throws([] { SoapyRPITX dev(SoapySDR::Kwargs{{"relay_gpio", "1"}}); }));
    CHECK(throws([] { SoapyRPITX dev(SoapySDR::Kwargs{{"relay_tx_delay_ms", "5000"}}); }));
    CHECK(throws([] { SoapyRPITX dev(SoapySDR::Kwargs{{"relay_active_low", "maybe"}}); }));

    // Opening drives the RX level first and then makes the pin an output.
    MockPokeBits(GENERAL_BASE, GPFSEL1, 7u << 21, 0);
    clearSetClr();
    SoapyRPITX dev(SoapySDR::Kwargs{{"relay_gpio", "17"}, {"relay_hang_ms", "30"}, {"relay_tx_delay_ms", "40"}});
    CHECK(((MockPeek(GENERAL_BASE, GPFSEL1) >> 21) & 7) == fsel_output);
    CHECK((MockPeek(GENERAL_BASE, kGpclr0) & kRelayBit) != 0 && (MockPeek(GENERAL_BASE, kGpset0) & kRelayBit) == 0);
    CHECK(dev.readSetting("relay_gpio") == "17" && dev.readSetting("relay_hang_ms") == "30" && dev.readSetting("relay_active_low") == "false");
    CHECK(dev.getHardwareInfo()["relay_gpio"] == "GPIO17");
    CHECK(dev.readSensor("ptt") == "false" && dev.readSensor("ptt_count") == "0");

    // Polarity: active low drives the pin high in RX.
    clearSetClr();
    dev.writeSetting("relay_active_low", "true");
    CHECK((MockPeek(GENERAL_BASE, kGpset0) & kRelayBit) != 0);
    dev.writeSetting("relay_active_low", "false");

    // Streams that would use the relay pin are refused; the pin cannot change while a stream is open.
    dev.writeSetting("relay_gpio", "20");
    CHECK(((MockPeek(GENERAL_BASE, GPFSEL1) >> 21) & 7) == fsel_input); // released
    CHECK(throws([&] { dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"rf_dac", "true"}}); }));
    dev.writeSetting("relay_gpio", "17");
    SoapySDR::Stream *st = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"rt_priority", "0"}});
    CHECK(throws([&] { dev.writeSetting("relay_gpio", "22"); }));
    CHECK(throws([&] { dev.writeSetting("relay_active_low", "true"); }));
    dev.writeSetting("relay_hang_ms", "30"); // timing may change at any time
    dev.closeStream(st);
    dev.writeSetting("relay_gpio", "-1");
    CHECK(dev.getHardwareInfo()["relay_gpio"] == "none");
}

void testRelaySequencing() {
    clearSetClr();
    SoapyRPITX dev(SoapySDR::Kwargs{{"relay_gpio", "17"}, {"relay_hang_ms", "30"}, {"relay_tx_delay_ms", "40"}});
    dev.setSampleRate(SOAPY_SDR_TX, 0, kRate);
    MockDmaEngine engine(14, kRate);
    SoapySDR::Stream *st = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"rt_priority", "0"}, {"preroll_ms", "0"}});
    CHECK(dev.activateStream(st) == 0);
    CHECK(dev.readSensor("ptt") == "false");

    // The first write switches the relay to TX and waits for it before queueing.
    clearSetClr();
    std::vector<std::complex<float>> buf(kRate / 10, std::complex<float>(1.0f, 0.0f));
    const void *buffs[] = {buf.data()};
    int flags = SOAPY_SDR_END_BURST;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(dev.writeStream(st, buffs, buf.size(), flags, 0, 1000000) == (int) buf.size());
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(40));
    CHECK((MockPeek(GENERAL_BASE, kGpset0) & kRelayBit) != 0);
    CHECK(dev.readSensor("ptt") == "true" && dev.readSensor("ptt_count") == "1");

    // Back to RX once the burst has been played and the hang time has elapsed.
    CHECK(waitPtt(dev, false, 2000));
    CHECK((MockPeek(GENERAL_BASE, kGpclr0) & kRelayBit) != 0);
    CHECK(carrierOn(engine.Take()) == buf.size());

    // A second burst keys again; deactivating unkeys at once.
    clearSetClr();
    flags = 0;
    CHECK(dev.writeStream(st, buffs, buf.size(), flags, 0, 1000000) == (int) buf.size());
    CHECK(dev.readSensor("ptt") == "true" && dev.readSensor("ptt_count") == "2");
    CHECK(dev.deactivateStream(st) == 0);
    CHECK(dev.readSensor("ptt") == "false" && (MockPeek(GENERAL_BASE, kGpclr0) & kRelayBit) != 0);

    // Closing a keyed stream returns the relay to RX.
    CHECK(dev.activateStream(st) == 0);
    flags = 0;
    CHECK(dev.writeStream(st, buffs, buf.size(), flags, 0, 1000000) == (int) buf.size());
    clearSetClr();
    dev.closeStream(st);
    CHECK(dev.readSensor("ptt") == "false" && (MockPeek(GENERAL_BASE, kGpclr0) & kRelayBit) != 0);
}

void testRelayLowBank() {
    // A relay pin in GPFSEL0 keeps its output function while the DMA rewrites that register.
    clearSetClr();
    SoapyRPITX dev(SoapySDR::Kwargs{{"relay_gpio", "5"}, {"relay_hang_ms", "0"}, {"relay_tx_delay_ms", "0"}});
    CHECK(((MockPeek(GENERAL_BASE, GPFSEL0) >> 15) & 7) == fsel_output);
    MockDmaEngine engine(14, kRate);
    SoapySDR::Stream *st = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"rt_priority", "0"}, {"preroll_ms", "0"}});
    CHECK(dev.activateStream(st) == 0);
    std::vector<std::complex<float>> buf(kRate / 20, std::complex<float>(1.0f, 0.0f));
    const void *buffs[] = {buf.data()};
    int flags = SOAPY_SDR_END_BURST;
    CHECK(dev.writeStream(st, buffs, buf.size(), flags, 0, 1000000) == (int) buf.size());
    CHECK((MockPeek(GENERAL_BASE, kGpset0) & (1u << 5)) != 0);
    CHECK(dev.deactivateStream(st) == 0);
    CHECK(((MockPeek(GENERAL_BASE, GPFSEL0) >> 15) & 7) == fsel_output);
    CHECK(carrierOn(engine.Take()) == buf.size());
    dev.closeStream(st);
}

void testReceiverMute() {
    for (bool mute : {true, false}) {
        SoapyRPITX dev(SoapySDR::Kwargs{{"rx", "rpitx_test_rx"},
                                        {"relay_gpio", "17"},
                                        {"relay_hang_ms", "20"},
                                        {"relay_tx_delay_ms", "0"},
                                        {"rx_mute_tx", mute ? "true" : "false"},
                                        {"rx_mute_tail_ms", "0"}});
        CHECK(dev.readSetting("rx_mute_tx") == (mute ? "true" : "false"));
        dev.setSampleRate(SOAPY_SDR_TX, 0, kRate);
        MockDmaEngine engine(14, kRate);
        SoapySDR::Stream *rx = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16);
        SoapySDR::Stream *tx = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"rt_priority", "0"}, {"preroll_ms", "0"}});
        CHECK(dev.activateStream(rx) == 0 && dev.activateStream(tx) == 0);
        CHECK(readFirstI(dev, rx) == kRxValue);

        std::vector<std::complex<float>> buf(kRate / 5, std::complex<float>(1.0f, 0.0f));
        const void *buffs[] = {buf.data()};
        int flags = SOAPY_SDR_END_BURST;
        CHECK(dev.writeStream(tx, buffs, buf.size(), flags, 0, 1000000) == (int) buf.size());
        CHECK(readFirstI(dev, rx) == (mute ? 0 : kRxValue));
        CHECK(waitPtt(dev, false, 2000));
        CHECK(readFirstI(dev, rx) == kRxValue);
        dev.closeStream(tx);
        dev.closeStream(rx);
    }

    // Unsigned formats are muted to mid-scale.
    SoapyRPITX dev(SoapySDR::Kwargs{{"rx", "rpitx_test_rx"}, {"relay_hang_ms", "1000"}});
    MockDmaEngine engine(14, kRate);
    SoapySDR::Stream *rx = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CU8);
    SoapySDR::Stream *tx = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"rt_priority", "0"}, {"preroll_ms", "0"}});
    CHECK(dev.activateStream(rx) == 0 && dev.activateStream(tx) == 0);
    std::vector<std::complex<float>> buf(480, std::complex<float>(1.0f, 0.0f));
    const void *buffs[] = {buf.data()};
    int flags = 0;
    CHECK(dev.writeStream(tx, buffs, buf.size(), flags, 0, 1000000) == (int) buf.size());
    std::vector<uint8_t> u8(2 * 64, 0);
    void *rbuffs[] = {u8.data()};
    long long timeNs = 0;
    int n = dev.readStream(rx, rbuffs, 64, flags, timeNs, 100000);
    CHECK(n == 64 && u8[0] == 0x80 && u8[2 * 64 - 1] == 0x80);
    dev.closeStream(tx);
    dev.closeStream(rx);
}

void testRelayProfile() {
    const char *path = "test_profile_relay.rpitx";
    {
        std::ofstream out(path);
        out << "relay_gpio=22\nrelay_active_low=true\nrelay_hang_ms=250\nrx_mute_tx=false\n";
    }
    chmod(path, 0644);
    SoapyRPITX dev(SoapySDR::Kwargs{{"profile", path}, {"relay_hang_ms", "60"}});
    CHECK(dev.readSetting("relay_gpio") == "22" && dev.readSetting("relay_active_low") == "true");
    CHECK(dev.readSetting("relay_hang_ms") == "60"); // the device argument takes precedence
    CHECK(dev.readSetting("rx_mute_tx") == "false");
    {
        std::ofstream out(path);
        out << "relay_gpio=4\n";
    }
    CHECK(throws([&] { dev.writeSetting("profile", path); }));
    CHECK(dev.readSetting("relay_gpio") == "22");
    std::remove(path);
}

// The supervisor reads the write sequence before it evaluates the drain state: a write that began in between keeps
// the transmitter keyed, even with no hang time.
void testPttWriteSequence() {
    SoapyRPITX_Ptt ptt;
    SoapyRPITX_RelayConfig c;
    c.gpio = -1;
    c.hangMs = 0;
    c.txDelayMs = 0;
    ptt.Configure(c);

    ptt.BeginWrite();
    ptt.EndWrite();
    const uint64_t seq = ptt.WriteSeq();
    const bool drained = true; // evaluated by the supervisor after reading seq
    ptt.BeginWrite();          // a second burst is queued meanwhile
    ptt.EndWrite();
    ptt.Poll(seq, drained);
    CHECK(ptt.IsKeyed());

    // With a fresh sequence and a drained transmitter it unkeys.
    ptt.Poll(ptt.WriteSeq(), true);
    CHECK(!ptt.IsKeyed());

    // Mute tail: muted while keyed and for the tail after the unkeying.
    ptt.BeginWrite();
    ptt.EndWrite();
    CHECK(ptt.MutedNow(0));
    ptt.Release();
    CHECK(!ptt.IsKeyed());
    CHECK(!ptt.MutedNow(0));
    CHECK(ptt.MutedNow(200));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(!ptt.MutedNow(200));

    // Never keyed: never muted.
    SoapyRPITX_Ptt fresh;
    CHECK(!fresh.MutedNow(RPITX_RX_MUTE_MAX_TAIL_MS));
}

// Samples stay muted for rx_mute_tail_ms after the unkeying.
void testReceiverMuteTail() {
    SoapyRPITX dev(SoapySDR::Kwargs{{"rx", "rpitx_test_rx"}, {"relay_hang_ms", "0"}, {"rx_mute_tail_ms", "400"}});
    CHECK(dev.readSetting("rx_mute_tail_ms") == "400");
    CHECK(throws([&] { dev.writeSetting("rx_mute_tail_ms", "10001"); }));
    CHECK(throws([&] { dev.writeSetting("rx_mute_tail_ms", "-1"); }));
    CHECK(dev.readSetting("rx_mute_tail_ms") == "400");
    bool listed = false;
    for (const auto &a : dev.getSettingInfo())
        listed = listed || (a.key == "rx_mute_tail_ms" && a.value == std::to_string(RPITX_RX_MUTE_DEFAULT_TAIL_MS));
    CHECK(listed);

    MockDmaEngine engine(14, kRate);
    SoapySDR::Stream *rx = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16);
    SoapySDR::Stream *tx = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"rt_priority", "0"}, {"preroll_ms", "0"}});
    CHECK(dev.activateStream(rx) == 0 && dev.activateStream(tx) == 0);
    std::vector<std::complex<float>> buf(kRate / 20, std::complex<float>(1.0f, 0.0f));
    const void *buffs[] = {buf.data()};
    int flags = SOAPY_SDR_END_BURST;
    CHECK(dev.writeStream(tx, buffs, buf.size(), flags, 0, 1000000) == (int) buf.size());
    CHECK(waitPtt(dev, false, 2000));
    CHECK(readFirstI(dev, rx) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(readFirstI(dev, rx) == kRxValue);
    dev.closeStream(tx);
    dev.closeStream(rx);
}

// Every RX format is muted over the whole buffer, packed formats included.
void testReceiverMutePackedFormats() {
    struct Case {
        const char *format;
        std::vector<uint8_t> silence; // one complex element
    };
    const Case cases[] = {
            {SOAPY_SDR_CS4, {0x00}},
            {SOAPY_SDR_CS8, {0x00, 0x00}},
            {SOAPY_SDR_CS12, {0x00, 0x00, 0x00}},
            {SOAPY_SDR_CU4, {0x88}},
            {SOAPY_SDR_CU12, {0x00, 0x08, 0x80}},
            {SOAPY_SDR_CU16, {0x00, 0x80, 0x00, 0x80}},
            {SOAPY_SDR_CF32, {0, 0, 0, 0, 0, 0, 0, 0}},
    };
    SoapyRPITX dev(SoapySDR::Kwargs{{"rx", "rpitx_test_rx"}, {"relay_hang_ms", "5000"}});
    MockDmaEngine engine(14, kRate);
    SoapySDR::Stream *tx = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"rt_priority", "0"}, {"preroll_ms", "0"}});
    CHECK(dev.activateStream(tx) == 0);
    std::vector<std::complex<float>> buf(480, std::complex<float>(1.0f, 0.0f));
    const void *buffs[] = {buf.data()};
    int flags = 0;
    CHECK(dev.writeStream(tx, buffs, buf.size(), flags, 0, 1000000) == (int) buf.size());
    CHECK(dev.readSensor("ptt") == "true");

    for (const Case &c : cases) {
        const size_t elem = SoapySDR::formatToSize(c.format);
        CHECK(elem == c.silence.size());
        SoapySDR::Stream *rx = dev.setupStream(SOAPY_SDR_RX, c.format);
        CHECK(dev.activateStream(rx) == 0);
        std::vector<uint8_t> data(64 * elem + 16, 0xEE);
        void *rbuffs[] = {data.data()};
        long long timeNs = 0;
        int n = dev.readStream(rx, rbuffs, 64, flags, timeNs, 100000);
        CHECK(n == 64);
        bool ok = true;
        for (size_t i = 0; i < 64 * elem; i++)
            ok = ok && data[i] == c.silence[i % elem];
        for (size_t i = 64 * elem; i < data.size(); i++)
            ok = ok && data[i] == 0xEE; // nothing written past the samples read
        if (!ok)
            std::fprintf(stderr, "format %s not muted correctly\n", c.format);
        CHECK(ok);
        dev.closeStream(rx);
    }
    dev.closeStream(tx);
}

// closeStream() ends a write waiting for the relay delay and a read blocked in the receiver promptly.
void testCloseInterruptsWaits() {
    {
        SoapyRPITX dev(SoapySDR::Kwargs{{"relay_gpio", "17"}, {"relay_tx_delay_ms", "1000"}});
        MockDmaEngine engine(14, kRate);
        SoapySDR::Stream *tx = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"rt_priority", "0"}});
        CHECK(dev.activateStream(tx) == 0);
        std::atomic<int> ret{1};
        std::thread writer([&] {
            std::vector<std::complex<float>> buf(480, std::complex<float>(1.0f, 0.0f));
            const void *buffs[] = {buf.data()};
            int flags = 0;
            ret.store(dev.writeStream(tx, buffs, buf.size(), flags, 0, 100000));
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto t0 = std::chrono::steady_clock::now();
        dev.closeStream(tx);
        const auto took = std::chrono::steady_clock::now() - t0;
        writer.join();
        CHECK(took < std::chrono::milliseconds(300));
        CHECK(ret.load() == SOAPY_SDR_STREAM_ERROR);
    }
    {
        SoapyRPITX dev(SoapySDR::Kwargs{{"rx", "rpitx_test_rx"}});
        SoapySDR::Stream *rx = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16);
        CHECK(dev.activateStream(rx) == 0);
        dev.writeSetting("rx.block", "1");
        std::atomic<int> ret{1};
        std::thread reader([&] {
            std::vector<int16_t> data(2 * 64);
            void *rbuffs[] = {data.data()};
            int flags = 0;
            long long timeNs = 0;
            ret.store(dev.readStream(rx, rbuffs, 64, flags, timeNs, 10000000));
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const auto t0 = std::chrono::steady_clock::now();
        dev.closeStream(rx);
        const auto took = std::chrono::steady_clock::now() - t0;
        reader.join();
        CHECK(took < std::chrono::milliseconds(300));
        CHECK(ret.load() == SOAPY_SDR_STREAM_ERROR);
        CHECK(std::stol(dev.readSetting("rx.max_timeout_us")) <= 100000);

        // A sliced read still waits for its whole timeout when nothing arrives.
        SoapySDR::Stream *rx2 = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16);
        CHECK(dev.activateStream(rx2) == 0);
        std::vector<int16_t> data(2 * 64);
        void *rbuffs[] = {data.data()};
        int flags = 0;
        long long timeNs = 0;
        const auto t1 = std::chrono::steady_clock::now();
        CHECK(dev.readStream(rx2, rbuffs, 64, flags, timeNs, 250000) == SOAPY_SDR_TIMEOUT);
        const auto waited = std::chrono::steady_clock::now() - t1;
        CHECK(waited >= std::chrono::milliseconds(240) && waited < std::chrono::milliseconds(600));
        dev.writeSetting("rx.block", "0");
        dev.closeStream(rx2);
    }
}

// Two threads polling the stream status: each event is reported once.
void testStatusReportedOnce() {
    SoapyRPITX dev(SoapySDR::Kwargs{});
    dev.setSampleRate(SOAPY_SDR_TX, 0, kRate);
    MockDmaEngine engine(14, kRate);
    SoapySDR::Stream *st = dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {}, {{"rt_priority", "0"}});
    CHECK(dev.activateStream(st) == 0);
    std::vector<std::complex<float>> buf(4800, std::complex<float>(1.0f, 0.0f));
    const void *buffs[] = {buf.data()};
    int flags = 0;
    CHECK(dev.writeStream(st, buffs, buf.size(), flags, 0, 1000000) == (int) buf.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(400)); // one starvation inside the burst
    std::atomic<int> reports{0};
    auto poll = [&] {
        size_t mask = 0;
        int f = 0;
        long long t = 0;
        for (int i = 0; i < 20; i++)
            if (dev.readStreamStatus(st, mask, f, t, 5000) == SOAPY_SDR_UNDERFLOW)
                reports++;
    };
    std::thread a(poll), b(poll);
    a.join();
    b.join();
    CHECK(reports.load() == 1);
    dev.closeStream(st);
}

// A profile in a directory of the user who started the process through sudo is accepted when nobody else can
// write it; without sudo it is refused.
void testProfileSudoUser() {
    if (geteuid() != 0) {
        std::printf("testProfileSudoUser skipped (needs root)\n");
        return;
    }
    const uid_t user = 4242;
    const std::string dir = std::string(RPITX_LOCK_DIR) + "_sudo_profile";
    const std::string file = dir + "/p.rpitx";
    std::remove(file.c_str());
    rmdir(dir.c_str());
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    writeFile(file.c_str(), "iq_scale=0.5\n");
    CHECK(chmod(file.c_str(), 0644) == 0);
    CHECK(chown(dir.c_str(), user, user) == 0 && chown(file.c_str(), user, user) == 0);

    const char *saved = getenv("SUDO_UID");
    const std::string savedValue = saved ? saved : "";
    SoapyRPITX_Profile p;
    std::string err;
    unsetenv("SUDO_UID");
    CHECK(!SoapyRPITX_LoadProfile(file, p, err) && err == "a directory of the path is owned by another user");
    setenv("SUDO_UID", "4242", 1);
    CHECK(SoapyRPITX_LoadProfile(file, p, err));
    // Another sudo user is not trusted.
    setenv("SUDO_UID", "4243", 1);
    CHECK(!SoapyRPITX_LoadProfile(file, p, err));
    setenv("SUDO_UID", "4242", 1);
    // Group-writable directory or file of the sudo user: refused.
    CHECK(chmod(dir.c_str(), 0775) == 0);
    CHECK(!SoapyRPITX_LoadProfile(file, p, err) && err == "a directory of the path is writable by others");
    CHECK(chmod(dir.c_str(), 0755) == 0);
    CHECK(chmod(file.c_str(), 0664) == 0);
    CHECK(!SoapyRPITX_LoadProfile(file, p, err) && err == "writable by group or others");
    CHECK(chmod(file.c_str(), 0644) == 0);
    // Malformed SUDO_UID values are ignored.
    for (const char *bad : {"", "abc", "0", "42x"}) {
        setenv("SUDO_UID", bad, 1);
        CHECK(!SoapyRPITX_LoadProfile(file, p, err));
    }

    if (saved)
        setenv("SUDO_UID", savedValue.c_str(), 1);
    else
        unsetenv("SUDO_UID");
    std::remove(file.c_str());
    rmdir(dir.c_str());
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
    testStaleHandle();
    testCloseWhileWriting();
    testProfileParser();
    testProfileDevice();
    testInputLimits();
    testProfileFileChecks();
    testReceiverForwarding();
    testReceiverEnumeration();
    testRelaySettings();
    testRelaySequencing();
    testRelayLowBank();
    testReceiverMute();
    testRelayProfile();
    testPttWriteSequence();
    testReceiverMuteTail();
    testReceiverMutePackedFormats();
    testCloseInterruptsWaits();
    testStatusReportedOnce();
    testProfileSudoUser();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all driver tests passed\n");
    return 0;
}
