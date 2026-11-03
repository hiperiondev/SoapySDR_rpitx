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

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

#include "SoapyRPITX.hpp"
#include "util.hpp"

#ifndef SOAPY_RPITX_VERSION
#define SOAPY_RPITX_VERSION "unknown"
#endif

namespace {

// Keys accepted both as device arguments and as settings.
const char *const kSettingKeys[] = {"ppm", "ppm_ntp", "harmonic", "amplitude_mode", "iq_scale", "debug_level"};
// Device arguments produced by enumeration and by SoapySDR itself; they are not settings.
const char *const kIdentificationKeys[] = {"driver", "label", "product", "serial", "manufacturer", "tuner", "type"};

bool isKnownArgument(const std::string &key) {
    for (const char *k : kSettingKeys)
        if (key == k)
            return true;
    for (const char *k : kIdentificationKeys)
        if (key == k)
            return true;
    return key.find(':') != std::string::npos; // namespaced keys (e.g. "remote:...") belong to other layers
}

// Lowest carrier the PLL divider search accepts (MIN_PLL_RATE / 4095, rounded up).
constexpr double kMinPllFrequency = 50e3;
// Highest carrier on BCM2835/6/7 and on BCM2711 (where the VCO runs at four times the output).
constexpr double kMaxPllFrequency = 1500e6;
constexpr double kMaxPllFrequencyPi4 = 1000e6;

double parseDouble(const std::string &key, const std::string &value) {
    std::istringstream in(value);
    in.imbue(std::locale::classic());
    double v;
    in >> v;
    if (in.fail() || !std::isfinite(v))
        throw std::runtime_error("rpitx: invalid value '" + value + "' for " + key);
    return v;
}

long parseInt(const std::string &key, const std::string &value) {
    try {
        size_t used = 0;
        long v = std::stol(value, &used, 0);
        if (used != value.size())
            throw std::invalid_argument(value);
        return v;
    } catch (const std::exception &) {
        throw std::runtime_error("rpitx: invalid integer '" + value + "' for " + key);
    }
}

bool parseBool(const std::string &key, const std::string &value) {
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    if (v == "true" || v == "1" || v == "yes" || v == "on")
        return true;
    if (v == "false" || v == "0" || v == "no" || v == "off")
        return false;
    throw std::runtime_error("rpitx: invalid boolean '" + value + "' for " + key);
}

std::string formatDouble(double v) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << v;
    return out.str();
}

} // namespace

SoapyRPITX::SoapyRPITX(const SoapySDR::Kwargs &args) {
    SoapyRPITX_BoardInfo board = SoapyRPITX_DetectBoard();
    if (!board.supported)
        throw std::runtime_error("rpitx: " + board.reason);
    if (!SoapyRPITX_HasHardwareAccess())
        throw std::runtime_error("rpitx: /dev/mem and the VideoCore mailbox are not accessible; run the application as root");

    model = board.model;
    serial = board.serial;
    pi4 = board.pi4;

    for (const auto &arg : args) {
        if (!isKnownArgument(arg.first))
            SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: unknown device argument '%s' ignored", arg.first.c_str());
    }
    for (const char *key : kSettingKeys) {
        auto it = args.find(key);
        if (it != args.end())
            writeSetting(key, it->second);
    }

    SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: opened %s (serial %s)", model.c_str(), serial.c_str());
}

SoapyRPITX::~SoapyRPITX(void) {
    std::lock_guard<std::mutex> lock(device_mutex);
    streamHandle.store(nullptr);
    tx_stream.reset();
}

// ******************** Identification API ********************

std::string SoapyRPITX::getDriverKey(void) const {
    return "rpitx";
}

std::string SoapyRPITX::getHardwareKey(void) const {
    return model;
}

SoapySDR::Kwargs SoapyRPITX::getHardwareInfo(void) const {
    SoapySDR::Kwargs info;

    info["driver_version"] = SOAPY_RPITX_VERSION;
    info["backend"] = "librpitx (vendored)";
    info["model"] = model;
    info["serial"] = serial;
    info["rf_output"] = RPITX_ANTENNA_NAME;
    info["origin"] = "https://github.com/hiperiondev/SoapySDR_rpitx";

    return info;
}

// ******************** Channels API ********************

size_t SoapyRPITX::getNumChannels(const int direction) const {
    return direction == SOAPY_SDR_TX ? 1 : 0;
}

bool SoapyRPITX::getFullDuplex(const int, const size_t) const {
    return false;
}

// ******************** Settings API ********************

SoapySDR::ArgInfoList SoapyRPITX::getSettingInfo(void) const {
    SoapySDR::ArgInfoList setArgs;

    SoapySDR::ArgInfo ppm;
    ppm.key = "ppm";
    ppm.value = "0";
    ppm.name = "PPM correction";
    ppm.description = "Manual crystal correction, added to the NTP estimate when ppm_ntp is enabled. Positive when the crystal runs slow "
                      "(the board transmits below the requested frequency).";
    ppm.units = "ppm";
    ppm.type = SoapySDR::ArgInfo::FLOAT;
    ppm.range = SoapySDR::Range(-200, 200);
    setArgs.push_back(ppm);

    SoapySDR::ArgInfo ntp;
    ntp.key = "ppm_ntp";
    ntp.value = "true";
    ntp.name = "PPM from NTP";
    ntp.description = "Use the kernel NTP frequency estimate as the base crystal correction.";
    ntp.type = SoapySDR::ArgInfo::BOOL;
    setArgs.push_back(ntp);

    SoapySDR::ArgInfo harmonic;
    harmonic.key = "harmonic";
    harmonic.value = "1";
    harmonic.name = "Harmonic";
    harmonic.description = "Transmit on this harmonic of the square-wave clock: the PLL runs at frequency / harmonic.";
    harmonic.type = SoapySDR::ArgInfo::INT;
    harmonic.range = SoapySDR::Range(1, 15, 1);
    setArgs.push_back(harmonic);

    SoapySDR::ArgInfo mode;
    mode.key = "amplitude_mode";
    mode.value = "envelope";
    mode.name = "Amplitude mode";
    mode.description = "envelope: pad drive follows |IQ| (8 levels, carrier off below the lowest one); "
                       "constant: full drive whenever a sample is present (best for FM/FSK).";
    mode.type = SoapySDR::ArgInfo::STRING;
    mode.options = {"envelope", "constant"};
    mode.optionNames = {"Envelope", "Constant"};
    setArgs.push_back(mode);

    SoapySDR::ArgInfo scale;
    scale.key = "iq_scale";
    scale.value = "1.0";
    scale.name = "IQ scale";
    scale.description = "Gain applied to |IQ| before amplitude quantisation (envelope mode).";
    scale.type = SoapySDR::ArgInfo::FLOAT;
    scale.range = SoapySDR::Range(0, 100);
    setArgs.push_back(scale);

    SoapySDR::ArgInfo debug;
    debug.key = "debug_level";
    debug.value = "0";
    debug.name = "librpitx debug level";
    debug.description = "Diagnostic output of librpitx on stderr (0 = errors only).";
    debug.type = SoapySDR::ArgInfo::INT;
    debug.range = SoapySDR::Range(0, 2, 1);
    setArgs.push_back(debug);

    return setArgs;
}

void SoapyRPITX::writeSetting(const std::string &key, const std::string &value) {
    std::lock_guard<std::mutex> lock(device_mutex);

    if (key == "ppm") {
        SetPpmLocked(parseDouble(key, value));
    } else if (key == "ppm_ntp") {
        SoapyRPITX_TuneConfig t = tune;
        t.ppmFromNtp = parseBool(key, value);
        ApplyTune(t);
    } else if (key == "harmonic") {
        long h = parseInt(key, value);
        if (h < 1 || h > 15)
            throw std::runtime_error("rpitx: harmonic out of range [1, 15]");
        SoapySDR::Range r = PllRange();
        if (tune.frequency < r.minimum() * (double) h || tune.frequency > r.maximum() * (double) h)
            throw std::runtime_error("rpitx: the current frequency cannot be reached with harmonic " + value);
        SoapyRPITX_TuneConfig t = tune;
        t.harmonic = (int) h;
        ApplyTune(t);
    } else if (key == "amplitude_mode") {
        if (value != "envelope" && value != "constant")
            throw std::runtime_error("rpitx: amplitude_mode must be 'envelope' or 'constant'");
        constantEnvelope = (value == "constant");
        if (tx_stream)
            tx_stream->transmitter().SetConstantEnvelope(constantEnvelope);
    } else if (key == "iq_scale") {
        double s = parseDouble(key, value);
        if (s < 0 || s > 100)
            throw std::runtime_error("rpitx: iq_scale out of range [0, 100]");
        iqScale = (float) s;
        if (tx_stream)
            tx_stream->transmitter().SetIqScale(iqScale);
    } else if (key == "debug_level") {
        long d = parseInt(key, value);
        debugLevel = (int) std::min<long>(std::max<long>(d, 0), 2);
        dbg_setlevel(debugLevel);
    } else {
        throw std::runtime_error("rpitx: unknown setting '" + key + "'");
    }
}

std::string SoapyRPITX::readSetting(const std::string &key) const {
    std::lock_guard<std::mutex> lock(device_mutex);

    if (key == "ppm")
        return formatDouble(tune.ppm);
    if (key == "ppm_ntp")
        return tune.ppmFromNtp ? "true" : "false";
    if (key == "harmonic")
        return std::to_string(tune.harmonic);
    if (key == "amplitude_mode")
        return constantEnvelope ? "constant" : "envelope";
    if (key == "iq_scale")
        return formatDouble(iqScale);
    if (key == "debug_level")
        return std::to_string(debugLevel);

    throw std::runtime_error("rpitx: unknown setting '" + key + "'");
}

void SoapyRPITX::SetPpmLocked(double ppm) {
    if (!(ppm >= -200 && ppm <= 200))
        throw std::runtime_error("rpitx: ppm out of range [-200, 200]");
    SoapyRPITX_TuneConfig t = tune;
    t.ppm = ppm;
    ApplyTune(t);
}

void SoapyRPITX::ApplyTune(const SoapyRPITX_TuneConfig &t) {
    if (tx_stream && !tx_stream->transmitter().Retune(t))
        throw std::runtime_error("rpitx: the PLL cannot produce " + formatDouble(t.frequency) + " Hz with the requested parameters");
    tune = t;
}

// ******************** Antenna API ********************

std::vector<std::string> SoapyRPITX::listAntennas(const int direction, const size_t) const {
    std::vector<std::string> options;

    if (direction == SOAPY_SDR_TX)
        options.push_back(RPITX_ANTENNA_NAME);

    return options;
}

void SoapyRPITX::setAntenna(const int direction, const size_t, const std::string &name) {
    if (direction != SOAPY_SDR_TX || name != RPITX_ANTENNA_NAME)
        throw std::runtime_error("rpitx: the only antenna is TX '" RPITX_ANTENNA_NAME "'");
}

std::string SoapyRPITX::getAntenna(const int direction, const size_t) const {
    return direction == SOAPY_SDR_TX ? RPITX_ANTENNA_NAME : "";
}

// ******************** Frontend corrections API ********************

bool SoapyRPITX::hasDCOffsetMode(const int, const size_t) const {
    return false;
}

bool SoapyRPITX::hasFrequencyCorrection(const int direction, const size_t) const {
    return direction == SOAPY_SDR_TX;
}

void SoapyRPITX::setFrequencyCorrection(const int direction, const size_t, const double value) {
    if (direction != SOAPY_SDR_TX)
        throw std::runtime_error("rpitx: only the TX direction has a frequency correction");
    std::lock_guard<std::mutex> lock(device_mutex);
    SetPpmLocked(value);
}

double SoapyRPITX::getFrequencyCorrection(const int direction, const size_t) const {
    std::lock_guard<std::mutex> lock(device_mutex);
    return direction == SOAPY_SDR_TX ? tune.ppm : 0.0;
}

// ******************** Gain API ********************

std::vector<std::string> SoapyRPITX::listGains(const int direction, const size_t) const {
    std::vector<std::string> options;
    if (direction == SOAPY_SDR_TX)
        options.push_back(RPITX_GAIN_NAME);
    return options;
}

bool SoapyRPITX::hasGainMode(const int, const size_t) const {
    return false;
}

void SoapyRPITX::setGainMode(const int, const size_t, const bool automatic) {
    if (automatic)
        SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: automatic gain is not available, request ignored");
}

bool SoapyRPITX::getGainMode(const int, const size_t) const {
    return false;
}

void SoapyRPITX::setGain(const int direction, const size_t, const double value) {
    if (direction != SOAPY_SDR_TX)
        return;

    std::lock_guard<std::mutex> lock(device_mutex);
    double v = std::isfinite(value) ? value : 0.0;
    driveLevel = (int) std::min(std::max(std::lround(v), 0L), 7L);
    if (tx_stream)
        tx_stream->transmitter().SetDriveLevel(driveLevel);
}

void SoapyRPITX::setGain(const int direction, const size_t channel, const std::string &name, const double value) {
    if (name != RPITX_GAIN_NAME)
        throw std::runtime_error("rpitx: unknown gain element '" + name + "'");
    setGain(direction, channel, value);
}

double SoapyRPITX::getGain(const int direction, const size_t) const {
    std::lock_guard<std::mutex> lock(device_mutex);
    return direction == SOAPY_SDR_TX ? (double) driveLevel : 0.0;
}

double SoapyRPITX::getGain(const int direction, const size_t channel, const std::string &) const {
    return getGain(direction, channel);
}

SoapySDR::Range SoapyRPITX::getGainRange(const int, const size_t) const {
    return SoapySDR::Range(0, 7, 1);
}

SoapySDR::Range SoapyRPITX::getGainRange(const int, const size_t, const std::string &) const {
    return SoapySDR::Range(0, 7, 1);
}

// ******************** Frequency API ********************

SoapySDR::Range SoapyRPITX::PllRange() const {
    return SoapySDR::Range(kMinPllFrequency, pi4 ? kMaxPllFrequencyPi4 : kMaxPllFrequency);
}

void SoapyRPITX::setFrequency(const int direction, const size_t, const std::string &name, const double frequency, const SoapySDR::Kwargs &) {
    if (direction != SOAPY_SDR_TX)
        throw std::runtime_error("rpitx: only the TX direction can be tuned");
    if (name != "RF")
        throw std::runtime_error("rpitx: unknown frequency element '" + name + "'");

    std::lock_guard<std::mutex> lock(device_mutex);

    SoapySDR::Range r = PllRange();
    if (!(frequency >= r.minimum() * tune.harmonic && frequency <= r.maximum() * tune.harmonic))
        throw std::runtime_error("rpitx: frequency " + formatDouble(frequency) + " Hz is out of range");

    SoapyRPITX_TuneConfig t = tune;
    t.frequency = frequency;
    ApplyTune(t);
    SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: frequency %.0f Hz", frequency);
}

double SoapyRPITX::getFrequency(const int direction, const size_t, const std::string &) const {
    std::lock_guard<std::mutex> lock(device_mutex);
    return direction == SOAPY_SDR_TX ? tune.frequency : 0.0;
}

SoapySDR::ArgInfoList SoapyRPITX::getFrequencyArgsInfo(const int, const size_t) const {
    return SoapySDR::ArgInfoList();
}

std::vector<std::string> SoapyRPITX::listFrequencies(const int direction, const size_t) const {
    std::vector<std::string> names;
    if (direction == SOAPY_SDR_TX)
        names.push_back("RF");
    return names;
}

SoapySDR::RangeList SoapyRPITX::getFrequencyRange(const int direction, const size_t, const std::string &) const {
    if (direction != SOAPY_SDR_TX)
        return SoapySDR::RangeList();

    std::lock_guard<std::mutex> lock(device_mutex);
    SoapySDR::Range r = PllRange();
    return SoapySDR::RangeList(1, SoapySDR::Range(r.minimum() * tune.harmonic, r.maximum() * tune.harmonic));
}

// ******************** Sample Rate API ********************

void SoapyRPITX::setSampleRate(const int direction, const size_t, const double rate) {
    if (direction != SOAPY_SDR_TX)
        throw std::runtime_error("rpitx: only the TX direction has a sample rate");
    if (!(rate >= RPITX_MIN_SAMPLE_RATE && rate <= RPITX_MAX_SAMPLE_RATE))
        throw std::runtime_error("rpitx: sample rate " + formatDouble(rate) + " Hz is out of range");

    std::lock_guard<std::mutex> lock(device_mutex);
    SoapyRPITX_TuneConfig t = tune;
    t.sampleRate = (uint32_t) std::lround(rate);
    if (tx_stream && !SoapyRPITX_RingFitsRate(tx_stream->transmitter().RingSize(), t.sampleRate))
        throw std::runtime_error("rpitx: the DMA ring of the open stream (" + std::to_string(tx_stream->transmitter().RingSize()) +
                                 " samples) is shorter than " + std::to_string(RPITX_MIN_RING_MS) + " ms at " + std::to_string(t.sampleRate) +
                                 " Hz; close the stream or set ring_size");
    ApplyTune(t);
    SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: sample rate %u Hz", t.sampleRate);
}

double SoapyRPITX::getSampleRate(const int direction, const size_t) const {
    std::lock_guard<std::mutex> lock(device_mutex);
    return direction == SOAPY_SDR_TX ? (double) tune.sampleRate : 0.0;
}

std::vector<double> SoapyRPITX::listSampleRates(const int direction, const size_t) const {
    std::vector<double> options;
    if (direction == SOAPY_SDR_TX)
        options = {24000, 48000, 96000, 192000};
    return options;
}

SoapySDR::RangeList SoapyRPITX::getSampleRateRange(const int direction, const size_t) const {
    SoapySDR::RangeList results;
    if (direction == SOAPY_SDR_TX)
        results.push_back(SoapySDR::Range(RPITX_MIN_SAMPLE_RATE, RPITX_MAX_SAMPLE_RATE));
    return results;
}

void SoapyRPITX::setBandwidth(const int, const size_t, const double bw) {
    SoapySDR_logf(SOAPY_SDR_DEBUG, "rpitx: bandwidth %.0f Hz requested; the bandwidth follows the sample rate", bw);
}

double SoapyRPITX::getBandwidth(const int direction, const size_t channel) const {
    return getSampleRate(direction, channel);
}

std::vector<double> SoapyRPITX::listBandwidths(const int, const size_t) const {
    return std::vector<double>();
}
