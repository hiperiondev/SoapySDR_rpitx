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
const char *const kSettingKeys[] = {"ppm",
                                    "ppm_ntp",
                                    "harmonic",
                                    "amplitude_mode",
                                    "iq_scale",
                                    "debug_level",
                                    "amplitude_lut",
                                    "ampm_lut",
                                    "amplitude_quantizer",
                                    "sigma_delta_order",
                                    "envelope_delay",
                                    "envelope_floor",
                                    "rfdac_lut",
                                    "eer_lut",
                                    "eer_ampm_lut",
                                    "eer_gain",
                                    "eer_offset",
                                    "eer_idle",
                                    "eer_drive"};
// Transmit/receive switching keys, accepted as device arguments, settings and profile lines.
const char *const kPttKeys[] = {"relay_gpio", "relay_active_low", "relay_tx_delay_ms", "relay_hang_ms", "rx_mute_tx"};
// Device arguments produced by enumeration and by SoapySDR itself; they are not settings.
const char *const kIdentificationKeys[] = {"driver", "label", "product", "serial", "manufacturer", "tuner", "type"};

bool isSettingKey(const std::string &key) {
    for (const char *k : kSettingKeys)
        if (key == k)
            return true;
    return false;
}

bool isPttKey(const std::string &key) {
    for (const char *k : kPttKeys)
        if (key == k)
            return true;
    return false;
}

bool isRxKey(const std::string &key) {
    const std::string prefix = RPITX_RX_PREFIX;
    return key.size() > prefix.size() && key.compare(0, prefix.size(), prefix) == 0;
}

bool isKnownArgument(const std::string &key) {
    if (key == "profile" || key == RPITX_RX_ARG || isRxKey(key) || isPttKey(key))
        return true;
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
    in >> std::ws;
    if (!in.eof())
        throw std::runtime_error("rpitx: invalid value '" + value + "' for " + key);
    return v;
}

long parseInt(const std::string &key, const std::string &value) {
    try {
        size_t used = 0;
        long v = std::stol(value, &used, 10);
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

// Comma-separated list of numbers; an empty string is an empty list. Angles are given in degrees.
std::vector<double> parseList(const std::string &key, const std::string &value, double scale) {
    std::vector<double> out;
    std::string item;
    std::istringstream in(value);
    bool any = false;
    while (std::getline(in, item, ',')) {
        any = true;
        out.push_back(parseDouble(key, item) * scale);
    }
    if (any && value.back() == ',')
        throw std::runtime_error("rpitx: invalid value '" + value + "' for " + key);
    return out;
}

std::string formatList(const std::vector<double> &v, double scale) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) {
        if (i)
            s += ",";
        s += formatDouble(v[i] / scale);
    }
    return s;
}

constexpr double kDegree = M_PI / 180.0;

// Transmit/receive switching settings parsed but not yet applied.
struct PttPending {
    SoapyRPITX_RelayConfig relay;
    bool muteTx = true;
    bool touched = false;
};

void parsePttSetting(const std::string &key, const std::string &value, PttPending &p) {
    if (key == "relay_gpio") {
        long v = parseInt(key, value);
        if (v != -1 && (v < RPITX_RELAY_MIN_GPIO || v > RPITX_RELAY_MAX_GPIO))
            throw std::runtime_error("rpitx: relay_gpio must be -1 or a GPIO from " + std::to_string(RPITX_RELAY_MIN_GPIO) + " to " +
                                     std::to_string(RPITX_RELAY_MAX_GPIO));
        p.relay.gpio = (int) v;
    } else if (key == "relay_active_low") {
        p.relay.activeLow = parseBool(key, value);
    } else if (key == "relay_tx_delay_ms") {
        long v = parseInt(key, value);
        if (v < 0 || v > RPITX_RELAY_MAX_DELAY_MS)
            throw std::runtime_error("rpitx: relay_tx_delay_ms out of range [0, " + std::to_string(RPITX_RELAY_MAX_DELAY_MS) + "]");
        p.relay.txDelayMs = (uint32_t) v;
    } else if (key == "relay_hang_ms") {
        long v = parseInt(key, value);
        if (v < 0 || v > RPITX_RELAY_MAX_HANG_MS)
            throw std::runtime_error("rpitx: relay_hang_ms out of range [0, " + std::to_string(RPITX_RELAY_MAX_HANG_MS) + "]");
        p.relay.hangMs = (uint32_t) v;
    } else if (key == "rx_mute_tx") {
        p.muteTx = parseBool(key, value);
    } else {
        throw std::runtime_error("rpitx: unknown setting '" + key + "'");
    }
    p.touched = true;
}

SoapySDR::ArgInfo makeArg(const char *key, const char *value, const char *name, const char *description, SoapySDR::ArgInfo::Type type, const char *units = "") {
    SoapySDR::ArgInfo a;
    a.key = key;
    a.value = value;
    a.name = name;
    a.description = description;
    a.type = type;
    a.units = units;
    return a;
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
    ptt = std::make_shared<SoapyRPITX_Ptt>();

    for (const auto &arg : args) {
        if (!isKnownArgument(arg.first))
            SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: unknown device argument '%s' ignored", arg.first.c_str());
    }

    // The relay is put in the RX position before anything else can key the transmitter.
    PttPending pending;
    pending.relay = ptt->GetConfig();
    pending.muteTx = rxMuteTx.load();
    for (const char *key : kPttKeys) {
        auto it = args.find(key);
        if (it != args.end())
            parsePttSetting(key, it->second, pending);
    }
    if (pending.touched) {
        std::lock_guard<std::mutex> lock(device_mutex);
        ApplyPttLocked(pending.relay, pending.muteTx, false);
    }

    auto profile = args.find("profile");
    if (profile != args.end() && !profile->second.empty())
        LoadProfile(profile->second, &args);
    for (const char *key : kSettingKeys) {
        auto it = args.find(key);
        if (it != args.end())
            writeSetting(key, it->second);
    }

    auto rx = args.find(RPITX_RX_ARG);
    if (rx != args.end() && !rx->second.empty() && rx->second != "none")
        OpenReceiver(rx->second, args);

    SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: opened %s (serial %s)%s", model.c_str(), serial.c_str(),
                  rxDev ? (", transceiver with receiver " + rxDriver).c_str() : "");
}

SoapyRPITX::~SoapyRPITX(void) {
    std::lock_guard<std::mutex> lock(device_mutex);
    ReleaseTxStreamLocked();
    ReleaseRxStreamLocked();
    rxDev.reset();
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
    const int relay = ptt->GetConfig().gpio;
    info["relay_gpio"] = relay >= 0 ? "GPIO" + std::to_string(relay) : "none";

    if (rxDev) {
        info["rx_driver"] = rxDriver;
        info["rx_hardware"] = rxDev->getHardwareKey();
        for (const auto &kv : rxDev->getHardwareInfo())
            info[RPITX_RX_PREFIX + kv.first] = kv.second;
    }

    return info;
}

// ******************** Channels API ********************

size_t SoapyRPITX::getNumChannels(const int direction) const {
    if (direction == SOAPY_SDR_TX)
        return 1;
    return direction == SOAPY_SDR_RX && rxDev ? 1 : 0;
}

bool SoapyRPITX::getFullDuplex(const int, const size_t) const {
    return rxDev != nullptr;
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

    setArgs.push_back(makeArg("amplitude_lut", "", "Amplitude table",
                              "Relative RF amplitude of pad drive levels 0-7, 8 comma-separated increasing values measured at the operating band "
                              "(empty: (level + 1) / 8).",
                              SoapySDR::ArgInfo::STRING));
    setArgs.push_back(makeArg("ampm_lut", "", "AM/PM table", "Phase of pad drive levels 0-7 relative to the reference, 8 comma-separated values (empty: 0).",
                              SoapySDR::ArgInfo::STRING, "deg"));
    SoapySDR::ArgInfo quant = makeArg("amplitude_quantizer", "nearest", "Amplitude quantizer",
                                      "nearest: amplitude state nearest to |IQ|; sigma_delta: noise-shaped selection (quantisation noise moved away from "
                                      "the carrier, up to +/- slot rate / 2).",
                                      SoapySDR::ArgInfo::STRING);
    quant.options = {"nearest", "sigma_delta"};
    quant.optionNames = {"Nearest", "Sigma-delta"};
    setArgs.push_back(quant);
    SoapySDR::ArgInfo order = makeArg("sigma_delta_order", "2", "Sigma-delta order", "Order of the error feedback.", SoapySDR::ArgInfo::INT);
    order.range = SoapySDR::Range(1, 2, 1);
    setArgs.push_back(order);
    SoapySDR::ArgInfo delay = makeArg("envelope_delay", "0.5", "Envelope delay",
                                      "Delay of the envelope relative to the phase path (0.5 aligns the held envelope with the phase ramp of each "
                                      "sample; adjust with a two-tone test).",
                                      SoapySDR::ArgInfo::FLOAT, "samples");
    delay.range = SoapySDR::Range(-RPITX_MAX_ENVELOPE_DELAY, RPITX_MAX_ENVELOPE_DELAY);
    setArgs.push_back(delay);
    SoapySDR::ArgInfo floorArg = makeArg("envelope_floor", "0", "Envelope floor",
                                         "Envelope below which the carrier is switched off after 2 ms (0 disables it).", SoapySDR::ArgInfo::FLOAT);
    floorArg.range = SoapySDR::Range(0, 1);
    setArgs.push_back(floorArg);
    setArgs.push_back(makeArg("rfdac_lut", "", "RF-DAC table",
                              "Relative RF amplitude with GPIO4 only, GPIO20 only and both clock pins on (rf_dac stream argument), 3 values "
                              "(empty: 2/3, 1/3, 1).",
                              SoapySDR::ArgInfo::STRING));
    setArgs.push_back(makeArg("eer_lut", "", "EER table",
                              "RF amplitude measured at equally spaced envelope codes from 0 to full scale, 2-64 increasing values (empty: linear).",
                              SoapySDR::ArgInfo::STRING));
    setArgs.push_back(makeArg("eer_ampm_lut", "", "EER AM/PM table", "Phase measured at equally spaced envelope codes, 2-64 values (empty: 0).",
                              SoapySDR::ArgInfo::STRING, "deg"));
    SoapySDR::ArgInfo gain = makeArg("eer_gain", "1.0", "EER gain", "Gain applied to the envelope code.", SoapySDR::ArgInfo::FLOAT);
    gain.range = SoapySDR::Range(0, 10);
    setArgs.push_back(gain);
    SoapySDR::ArgInfo offset =
            makeArg("eer_offset", "0", "EER offset", "Offset added to the envelope code (fraction of full scale).", SoapySDR::ArgInfo::FLOAT);
    offset.range = SoapySDR::Range(-1, 1);
    setArgs.push_back(offset);
    SoapySDR::ArgInfo idle = makeArg("eer_idle", "0", "EER idle code", "Envelope code output while no sample is transmitted (fraction of full scale).",
                                     SoapySDR::ArgInfo::FLOAT);
    idle.range = SoapySDR::Range(0, 1);
    setArgs.push_back(idle);
    SoapySDR::ArgInfo drive =
            makeArg("eer_drive", "7", "EER drive", "Pad drive level of the clock pins while the EER output is active.", SoapySDR::ArgInfo::INT);
    drive.range = SoapySDR::Range(0, 7, 1);
    setArgs.push_back(drive);

    setArgs.push_back(makeArg("profile", "", "Profile file",
                              "Loads a key=value profile (for example written by rpitx_autotune.py): settings and gain at once, stream.* lines as "
                              "defaults of the next stream. Empty: forget the stream defaults.",
                              SoapySDR::ArgInfo::STRING));

    SoapySDR::ArgInfo relay = makeArg("relay_gpio", "-1", "Antenna relay GPIO",
                                      "BCM GPIO driving the T/R antenna relay, in the TX position while the transmitter is keyed (-1: no relay). "
                                      "Cannot change while a TX stream is open.",
                                      SoapySDR::ArgInfo::INT);
    relay.range = SoapySDR::Range(-1, RPITX_RELAY_MAX_GPIO, 1);
    setArgs.push_back(relay);
    setArgs.push_back(makeArg("relay_active_low", "false", "Relay active low",
                              "The relay is in the TX position with the pin low (most relay modules). Cannot change while a TX stream is open.",
                              SoapySDR::ArgInfo::BOOL));
    SoapySDR::ArgInfo relayDelay =
            makeArg("relay_tx_delay_ms", "20", "Relay TX delay",
                    "Time given to the relay to settle in the TX position before the first sample of a transmission is queued.", SoapySDR::ArgInfo::INT, "ms");
    relayDelay.range = SoapySDR::Range(0, RPITX_RELAY_MAX_DELAY_MS, 1);
    setArgs.push_back(relayDelay);
    SoapySDR::ArgInfo relayHang =
            makeArg("relay_hang_ms", "100", "Relay hang time",
                    "Time the transmitter stays keyed (relay in TX, receiver muted) after the last sample was played.", SoapySDR::ArgInfo::INT, "ms");
    relayHang.range = SoapySDR::Range(0, RPITX_RELAY_MAX_HANG_MS, 1);
    setArgs.push_back(relayHang);
    setArgs.push_back(makeArg("rx_mute_tx", "true", "Mute RX while keyed", "Replace the receive samples with silence while the transmitter is keyed.",
                              SoapySDR::ArgInfo::BOOL));

    if (rxDev) {
        for (SoapySDR::ArgInfo a : rxDev->getSettingInfo()) {
            a.key = RPITX_RX_PREFIX + a.key;
            a.name = "RX " + (a.name.empty() ? a.key : a.name);
            setArgs.push_back(a);
        }
    }

    return setArgs;
}

struct SoapyRPITX::PendingSettings {
    SoapyRPITX_TuneConfig tune;
    SoapyRPITX_AmplitudeConfig amp;
    bool constantEnvelope = false;
    float iqScale = 1.0f;
    int debugLevel = 0;
    int driveLevel = 7;
    bool tuneTouched = false;
    bool harmonicTouched = false;
    bool ampTouched = false;
    bool modeTouched = false;
    bool scaleTouched = false;
    bool debugTouched = false;
    bool gainTouched = false;
};

void SoapyRPITX::LoadProfile(const std::string &path, const SoapySDR::Kwargs *explicitArgs) {
    if (path.empty()) {
        std::lock_guard<std::mutex> lock(device_mutex);
        profilePath.clear();
        profileStream.clear();
        return;
    }
    SoapyRPITX_Profile p;
    std::string error;
    if (!SoapyRPITX_LoadProfile(path, p, error))
        throw std::runtime_error("rpitx: profile '" + path + "': " + error);

    auto context = [&path](const std::runtime_error &e) {
        std::string what = e.what();
        if (what.compare(0, 7, "rpitx: ") == 0)
            what = what.substr(7);
        return std::runtime_error("rpitx: profile '" + path + "': " + what);
    };

    SoapySDR::Kwargs stream;
    SoapySDR::ArgInfoList known = getStreamArgsInfo(SOAPY_SDR_TX, 0);
    for (const auto &kv : p.stream) {
        bool ok = false;
        for (const auto &info : known)
            ok = ok || info.key == kv.first;
        if (ok)
            stream[kv.first] = kv.second;
        else
            SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: profile '%s': unknown stream argument '%s' ignored", path.c_str(), kv.first.c_str());
    }

    std::lock_guard<std::mutex> lock(device_mutex);

    // Phase 1: parse and validate everything into a copy of the state; neither the hardware nor the device is touched.
    PendingSettings pending;
    pending.tune = tune;
    pending.amp = ampCfg;
    pending.constantEnvelope = constantEnvelope;
    pending.iqScale = iqScale;
    pending.debugLevel = debugLevel;
    pending.driveLevel = driveLevel;
    const SoapyRPITX_RelayConfig previousRelay = ptt->GetConfig();
    const bool previousMute = rxMuteTx.load();
    PttPending pendingPtt;
    pendingPtt.relay = previousRelay;
    pendingPtt.muteTx = previousMute;

    try {
        for (const auto &kv : p.settings) {
            if (isPttKey(kv.first)) {
                if (!(explicitArgs && explicitArgs->count(kv.first)))
                    parsePttSetting(kv.first, kv.second, pendingPtt);
                continue;
            }
            if (!isSettingKey(kv.first)) {
                SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: profile '%s': unknown setting '%s' ignored", path.c_str(), kv.first.c_str());
                continue;
            }
            if (explicitArgs && explicitArgs->count(kv.first))
                continue;
            ParseSetting(kv.first, kv.second, pending);
        }
        if (p.hasGain) {
            double g = parseDouble("gain", p.gain);
            if (!(g >= 0 && g <= 7))
                throw std::runtime_error("rpitx: gain out of range [0, 7]");
            pending.driveLevel = (int) std::lround(g);
            pending.gainTouched = true;
        }
        ValidateSettings(pending);
        if (pendingPtt.touched)
            ApplyPttLocked(pendingPtt.relay, pendingPtt.muteTx, true);

        // Dry run of the stream arguments with the parsers of setupStream().
        SoapyRPITX_TxConfig cfg;
        cfg.tune = pending.tune;
        std::string envOut;
        ParseStreamArgs(stream, cfg, envOut);

        // Phase 2: apply once; the switching settings are restored when the other settings are refused.
        if (pendingPtt.touched)
            ApplyPttLocked(pendingPtt.relay, pendingPtt.muteTx, false);
        try {
            ApplySettings(pending);
        } catch (...) {
            if (pendingPtt.touched) {
                try {
                    ApplyPttLocked(previousRelay, previousMute, false);
                } catch (const std::exception &restore) {
                    SoapySDR_logf(SOAPY_SDR_ERROR, "rpitx: could not restore the antenna relay settings: %s", restore.what());
                }
            }
            throw;
        }
    } catch (const std::runtime_error &e) {
        throw context(e);
    }

    profilePath = path;
    profileStream = stream;
    SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: profile '%s' loaded (%zu settings, %zu stream arguments%s)", path.c_str(), p.settings.size(), stream.size(),
                  p.hasGain ? ", gain" : "");
}

void SoapyRPITX::ParseSetting(const std::string &key, const std::string &value, PendingSettings &pending) const {
    if (key == "ppm") {
        double ppm = parseDouble(key, value);
        if (!(ppm >= -200 && ppm <= 200))
            throw std::runtime_error("rpitx: ppm out of range [-200, 200]");
        pending.tune.ppm = ppm;
        pending.tuneTouched = true;
    } else if (key == "ppm_ntp") {
        pending.tune.ppmFromNtp = parseBool(key, value);
        pending.tuneTouched = true;
    } else if (key == "harmonic") {
        long h = parseInt(key, value);
        if (h < 1 || h > 15)
            throw std::runtime_error("rpitx: harmonic out of range [1, 15]");
        pending.tune.harmonic = (int) h;
        pending.tuneTouched = true;
        pending.harmonicTouched = true;
    } else if (key == "amplitude_mode") {
        if (value != "envelope" && value != "constant")
            throw std::runtime_error("rpitx: amplitude_mode must be 'envelope' or 'constant'");
        pending.constantEnvelope = (value == "constant");
        pending.modeTouched = true;
    } else if (key == "iq_scale") {
        double s = parseDouble(key, value);
        if (s < 0 || s > 100)
            throw std::runtime_error("rpitx: iq_scale out of range [0, 100]");
        pending.iqScale = (float) s;
        pending.scaleTouched = true;
    } else if (key == "debug_level") {
        long d = parseInt(key, value);
        if (d < 0 || d > 2)
            throw std::runtime_error("rpitx: debug_level out of range [0, 2]");
        pending.debugLevel = (int) d;
        pending.debugTouched = true;
    } else {
        SoapyRPITX_AmplitudeConfig &a = pending.amp;
        if (key == "amplitude_lut") {
            a.amplitudeLut = parseList(key, value, 1.0);
        } else if (key == "ampm_lut") {
            a.ampmLut = parseList(key, value, kDegree);
        } else if (key == "amplitude_quantizer") {
            if (value == "nearest")
                a.quantizer = SoapyRPITX_Quantizer::Nearest;
            else if (value == "sigma_delta")
                a.quantizer = SoapyRPITX_Quantizer::SigmaDelta;
            else
                throw std::runtime_error("rpitx: amplitude_quantizer must be 'nearest' or 'sigma_delta'");
        } else if (key == "sigma_delta_order") {
            long v = parseInt(key, value);
            if (v < 1 || v > 2)
                throw std::runtime_error("rpitx: sigma_delta_order must be 1 or 2");
            a.sigmaDeltaOrder = (int) v;
        } else if (key == "envelope_delay") {
            a.envelopeDelay = parseDouble(key, value);
        } else if (key == "envelope_floor") {
            a.envelopeFloor = parseDouble(key, value);
        } else if (key == "rfdac_lut") {
            a.rfdacLut = parseList(key, value, 1.0);
        } else if (key == "eer_lut") {
            a.eerLut = parseList(key, value, 1.0);
        } else if (key == "eer_ampm_lut") {
            a.eerAmpmLut = parseList(key, value, kDegree);
        } else if (key == "eer_gain") {
            a.eerGain = parseDouble(key, value);
        } else if (key == "eer_offset") {
            a.eerOffset = parseDouble(key, value);
        } else if (key == "eer_idle") {
            a.eerIdle = parseDouble(key, value);
        } else if (key == "eer_drive") {
            long v = parseInt(key, value);
            if (v < 0 || v > 7)
                throw std::runtime_error("rpitx: eer_drive out of range [0, 7]");
            a.eerDrive = (int) v;
        } else {
            throw std::runtime_error("rpitx: unknown setting '" + key + "'");
        }
        pending.ampTouched = true;
    }
}

void SoapyRPITX::ValidateSettings(const PendingSettings &pending) const {
    if (pending.harmonicTouched) {
        SoapySDR::Range r = PllRange();
        const double h = (double) pending.tune.harmonic;
        if (pending.tune.frequency < r.minimum() * h || pending.tune.frequency > r.maximum() * h)
            throw std::runtime_error("rpitx: the current frequency cannot be reached with harmonic " + std::to_string(pending.tune.harmonic));
    }
    if (pending.ampTouched) {
        std::string why = SoapyRPITX_CheckAmplitudeConfig(pending.amp);
        if (!why.empty())
            throw std::runtime_error("rpitx: " + why);
        if (tx_stream && !tx_stream->transmitter().AmplitudeConfigFits(pending.amp))
            throw std::runtime_error("rpitx: envelope_delay needs more look-ahead than the DMA ring of the open stream allows");
    }
}

void SoapyRPITX::ApplySettings(const PendingSettings &pending) {
    // The two steps that can be refused by the stream come first, the retune before the amplitude update.
    const SoapyRPITX_TuneConfig previousTune = tune;
    if (pending.tuneTouched)
        ApplyTune(pending.tune);
    if (pending.ampTouched) {
        try {
            ApplyAmplitude(pending.amp);
        } catch (...) {
            if (pending.tuneTouched) {
                try {
                    ApplyTune(previousTune);
                } catch (const std::exception &e) {
                    SoapySDR_logf(SOAPY_SDR_ERROR, "rpitx: could not restore the previous tuning: %s", e.what());
                }
            }
            throw;
        }
    }

    // Nothing below can fail.
    if (pending.modeTouched) {
        constantEnvelope = pending.constantEnvelope;
        if (tx_stream)
            tx_stream->transmitter().SetConstantEnvelope(constantEnvelope);
    }
    if (pending.scaleTouched) {
        iqScale = pending.iqScale;
        if (tx_stream)
            tx_stream->transmitter().SetIqScale(iqScale);
    }
    if (pending.debugTouched) {
        debugLevel = pending.debugLevel;
        dbg_setlevel(debugLevel);
    }
    if (pending.gainTouched) {
        driveLevel = pending.driveLevel;
        if (tx_stream)
            tx_stream->transmitter().SetDriveLevel(driveLevel);
    }
}

void SoapyRPITX::ApplyPttLocked(const SoapyRPITX_RelayConfig &relay, bool muteTx, bool dryRun) {
    std::string why = SoapyRPITX_CheckRelayConfig(relay);
    if (!why.empty())
        throw std::runtime_error("rpitx: " + why);
    const SoapyRPITX_RelayConfig current = ptt->GetConfig();
    if (tx_stream && (relay.gpio != current.gpio || relay.activeLow != current.activeLow))
        throw std::runtime_error("rpitx: relay_gpio and relay_active_low cannot change while a TX stream is open");
    if (dryRun)
        return;
    ptt->Configure(relay);
    rxMuteTx.store(muteTx);
}

void SoapyRPITX::writeSetting(const std::string &key, const std::string &value) {
    if (key == "profile") {
        LoadProfile(value, nullptr);
        return;
    }
    if (isRxKey(key)) {
        Rx().writeSetting(key.substr(std::string(RPITX_RX_PREFIX).size()), value);
        return;
    }

    std::lock_guard<std::mutex> lock(device_mutex);

    if (isPttKey(key)) {
        PttPending p;
        p.relay = ptt->GetConfig();
        p.muteTx = rxMuteTx.load();
        parsePttSetting(key, value, p);
        ApplyPttLocked(p.relay, p.muteTx, false);
        return;
    }

    PendingSettings pending;
    pending.tune = tune;
    pending.amp = ampCfg;
    pending.constantEnvelope = constantEnvelope;
    pending.iqScale = iqScale;
    pending.debugLevel = debugLevel;
    pending.driveLevel = driveLevel;
    ParseSetting(key, value, pending);
    ValidateSettings(pending);
    ApplySettings(pending);
}

void SoapyRPITX::ApplyAmplitude(const SoapyRPITX_AmplitudeConfig &a) {
    std::string why = SoapyRPITX_CheckAmplitudeConfig(a);
    if (!why.empty())
        throw std::runtime_error("rpitx: " + why);
    if (tx_stream && !tx_stream->transmitter().SetAmplitudeConfig(a))
        throw std::runtime_error("rpitx: envelope_delay needs more look-ahead than the DMA ring of the open stream allows");
    ampCfg = a;
}

std::string SoapyRPITX::readSetting(const std::string &key) const {
    if (isRxKey(key))
        return Rx().readSetting(key.substr(std::string(RPITX_RX_PREFIX).size()));

    std::lock_guard<std::mutex> lock(device_mutex);

    if (isPttKey(key)) {
        const SoapyRPITX_RelayConfig r = ptt->GetConfig();
        if (key == "relay_gpio")
            return std::to_string(r.gpio);
        if (key == "relay_active_low")
            return r.activeLow ? "true" : "false";
        if (key == "relay_tx_delay_ms")
            return std::to_string(r.txDelayMs);
        if (key == "relay_hang_ms")
            return std::to_string(r.hangMs);
        return rxMuteTx.load() ? "true" : "false";
    }

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
    if (key == "amplitude_lut")
        return formatList(ampCfg.amplitudeLut, 1.0);
    if (key == "ampm_lut")
        return formatList(ampCfg.ampmLut, kDegree);
    if (key == "amplitude_quantizer")
        return ampCfg.quantizer == SoapyRPITX_Quantizer::SigmaDelta ? "sigma_delta" : "nearest";
    if (key == "sigma_delta_order")
        return std::to_string(ampCfg.sigmaDeltaOrder);
    if (key == "envelope_delay")
        return formatDouble(ampCfg.envelopeDelay);
    if (key == "envelope_floor")
        return formatDouble(ampCfg.envelopeFloor);
    if (key == "rfdac_lut")
        return formatList(ampCfg.rfdacLut, 1.0);
    if (key == "eer_lut")
        return formatList(ampCfg.eerLut, 1.0);
    if (key == "eer_ampm_lut")
        return formatList(ampCfg.eerAmpmLut, kDegree);
    if (key == "eer_gain")
        return formatDouble(ampCfg.eerGain);
    if (key == "eer_offset")
        return formatDouble(ampCfg.eerOffset);
    if (key == "eer_idle")
        return formatDouble(ampCfg.eerIdle);
    if (key == "eer_drive")
        return std::to_string(ampCfg.eerDrive);
    if (key == "profile")
        return profilePath;

    throw std::runtime_error("rpitx: unknown setting '" + key + "'");
}

SoapySDR::ArgInfoList SoapyRPITX::getSettingInfo(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->getSettingInfo(SOAPY_SDR_RX, channel);
    return SoapySDR::ArgInfoList();
}

void SoapyRPITX::writeSetting(const int direction, const size_t channel, const std::string &key, const std::string &value) {
    if (!ForwardRx(direction))
        throw std::runtime_error("rpitx: channel settings exist only on the RX channel of a receiver");
    rxDev->writeSetting(SOAPY_SDR_RX, channel, key, value);
}

std::string SoapyRPITX::readSetting(const int direction, const size_t channel, const std::string &key) const {
    if (!ForwardRx(direction))
        throw std::runtime_error("rpitx: channel settings exist only on the RX channel of a receiver");
    return rxDev->readSetting(SOAPY_SDR_RX, channel, key);
}

void SoapyRPITX::SetPpmLocked(double ppm) {
    if (!(ppm >= -200 && ppm <= 200))
        throw std::runtime_error("rpitx: ppm out of range [-200, 200]");
    SoapyRPITX_TuneConfig t = tune;
    t.ppm = ppm;
    ApplyTune(t);
}

void SoapyRPITX::ApplyTune(const SoapyRPITX_TuneConfig &t) {
    if (tx_stream) {
        SoapyRPITX_RetuneResult r = tx_stream->transmitter().Retune(t);
        switch (r) {
            case SoapyRPITX_RetuneResult::Ok:
                break;
            case SoapyRPITX_RetuneResult::Refused:
                throw std::runtime_error("rpitx: the PLL cannot produce " + formatDouble(t.frequency) + " Hz with the requested harmonic and bandwidth");
            case SoapyRPITX_RetuneResult::PacingRefused:
                throw std::runtime_error("rpitx: the pacing clock cannot run at the slot rate for " + formatDouble(t.sampleRate) +
                                         " S/s, or the DMA ring is too short");
            case SoapyRPITX_RetuneResult::Stopped:
                throw std::runtime_error("rpitx: the transmitter thread had already stopped; the change was not applied");
            case SoapyRPITX_RetuneResult::Timeout:
                throw std::runtime_error("rpitx: the transmitter thread did not respond within 3 s; the change was not applied");
        }
    }
    tune = t;
}

// ******************** Antenna API ********************

std::vector<std::string> SoapyRPITX::listAntennas(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->listAntennas(SOAPY_SDR_RX, channel);

    std::vector<std::string> options;

    if (direction == SOAPY_SDR_TX)
        options.push_back(RPITX_ANTENNA_NAME);

    return options;
}

void SoapyRPITX::setAntenna(const int direction, const size_t channel, const std::string &name) {
    if (ForwardRx(direction)) {
        rxDev->setAntenna(SOAPY_SDR_RX, channel, name);
        return;
    }
    if (direction != SOAPY_SDR_TX || name != RPITX_ANTENNA_NAME)
        throw std::runtime_error("rpitx: the only antenna is TX '" RPITX_ANTENNA_NAME "'");
}

std::string SoapyRPITX::getAntenna(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->getAntenna(SOAPY_SDR_RX, channel);
    return direction == SOAPY_SDR_TX ? RPITX_ANTENNA_NAME : "";
}

// ******************** Frontend corrections API ********************

bool SoapyRPITX::hasDCOffsetMode(const int direction, const size_t channel) const {
    return ForwardRx(direction) && rxDev->hasDCOffsetMode(SOAPY_SDR_RX, channel);
}

void SoapyRPITX::setDCOffsetMode(const int direction, const size_t channel, const bool automatic) {
    if (ForwardRx(direction))
        rxDev->setDCOffsetMode(SOAPY_SDR_RX, channel, automatic);
}

bool SoapyRPITX::getDCOffsetMode(const int direction, const size_t channel) const {
    return ForwardRx(direction) && rxDev->getDCOffsetMode(SOAPY_SDR_RX, channel);
}

bool SoapyRPITX::hasDCOffset(const int direction, const size_t channel) const {
    return ForwardRx(direction) && rxDev->hasDCOffset(SOAPY_SDR_RX, channel);
}

void SoapyRPITX::setDCOffset(const int direction, const size_t channel, const std::complex<double> &offset) {
    if (ForwardRx(direction))
        rxDev->setDCOffset(SOAPY_SDR_RX, channel, offset);
}

std::complex<double> SoapyRPITX::getDCOffset(const int direction, const size_t channel) const {
    return ForwardRx(direction) ? rxDev->getDCOffset(SOAPY_SDR_RX, channel) : std::complex<double>(0.0, 0.0);
}

bool SoapyRPITX::hasIQBalance(const int direction, const size_t channel) const {
    return ForwardRx(direction) && rxDev->hasIQBalance(SOAPY_SDR_RX, channel);
}

void SoapyRPITX::setIQBalance(const int direction, const size_t channel, const std::complex<double> &balance) {
    if (ForwardRx(direction))
        rxDev->setIQBalance(SOAPY_SDR_RX, channel, balance);
}

std::complex<double> SoapyRPITX::getIQBalance(const int direction, const size_t channel) const {
    return ForwardRx(direction) ? rxDev->getIQBalance(SOAPY_SDR_RX, channel) : std::complex<double>(0.0, 0.0);
}

bool SoapyRPITX::hasFrequencyCorrection(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->hasFrequencyCorrection(SOAPY_SDR_RX, channel);
    return direction == SOAPY_SDR_TX;
}

void SoapyRPITX::setFrequencyCorrection(const int direction, const size_t channel, const double value) {
    if (ForwardRx(direction)) {
        rxDev->setFrequencyCorrection(SOAPY_SDR_RX, channel, value);
        return;
    }
    if (direction != SOAPY_SDR_TX)
        throw std::runtime_error("rpitx: no receiver attached: only the TX direction has a frequency correction");
    std::lock_guard<std::mutex> lock(device_mutex);
    SetPpmLocked(value);
}

double SoapyRPITX::getFrequencyCorrection(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->getFrequencyCorrection(SOAPY_SDR_RX, channel);
    std::lock_guard<std::mutex> lock(device_mutex);
    return direction == SOAPY_SDR_TX ? tune.ppm : 0.0;
}

// ******************** Gain API ********************

std::vector<std::string> SoapyRPITX::listGains(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->listGains(SOAPY_SDR_RX, channel);

    std::vector<std::string> options;
    if (direction == SOAPY_SDR_TX)
        options.push_back(RPITX_GAIN_NAME);
    return options;
}

bool SoapyRPITX::hasGainMode(const int direction, const size_t channel) const {
    return ForwardRx(direction) && rxDev->hasGainMode(SOAPY_SDR_RX, channel);
}

void SoapyRPITX::setGainMode(const int direction, const size_t channel, const bool automatic) {
    if (ForwardRx(direction)) {
        rxDev->setGainMode(SOAPY_SDR_RX, channel, automatic);
        return;
    }
    if (automatic)
        SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: automatic gain is not available, request ignored");
}

bool SoapyRPITX::getGainMode(const int direction, const size_t channel) const {
    return ForwardRx(direction) && rxDev->getGainMode(SOAPY_SDR_RX, channel);
}

void SoapyRPITX::setGain(const int direction, const size_t channel, const double value) {
    if (ForwardRx(direction)) {
        rxDev->setGain(SOAPY_SDR_RX, channel, value);
        return;
    }
    if (direction != SOAPY_SDR_TX)
        return;

    std::lock_guard<std::mutex> lock(device_mutex);
    // Clamped before rounding, so that any finite value maps to the nearest level of the range.
    double v = std::isfinite(value) ? std::min(std::max(value, 0.0), 7.0) : 0.0;
    driveLevel = (int) std::lround(v);
    if (tx_stream)
        tx_stream->transmitter().SetDriveLevel(driveLevel);
}

void SoapyRPITX::setGain(const int direction, const size_t channel, const std::string &name, const double value) {
    if (ForwardRx(direction)) {
        rxDev->setGain(SOAPY_SDR_RX, channel, name, value);
        return;
    }
    if (name != RPITX_GAIN_NAME)
        throw std::runtime_error("rpitx: unknown gain element '" + name + "'");
    setGain(direction, channel, value);
}

double SoapyRPITX::getGain(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->getGain(SOAPY_SDR_RX, channel);
    std::lock_guard<std::mutex> lock(device_mutex);
    return direction == SOAPY_SDR_TX ? (double) driveLevel : 0.0;
}

double SoapyRPITX::getGain(const int direction, const size_t channel, const std::string &name) const {
    if (ForwardRx(direction))
        return rxDev->getGain(SOAPY_SDR_RX, channel, name);
    return getGain(direction, channel);
}

SoapySDR::Range SoapyRPITX::getGainRange(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->getGainRange(SOAPY_SDR_RX, channel);
    return SoapySDR::Range(0, 7, 1);
}

SoapySDR::Range SoapyRPITX::getGainRange(const int direction, const size_t channel, const std::string &name) const {
    if (ForwardRx(direction))
        return rxDev->getGainRange(SOAPY_SDR_RX, channel, name);
    return SoapySDR::Range(0, 7, 1);
}

// ******************** Frequency API ********************

SoapySDR::Range SoapyRPITX::PllRange() const {
    return SoapySDR::Range(kMinPllFrequency, pi4 ? kMaxPllFrequencyPi4 : kMaxPllFrequency);
}

void SoapyRPITX::setFrequency(const int direction, const size_t channel, const double frequency, const SoapySDR::Kwargs &args) {
    if (ForwardRx(direction)) {
        rxDev->setFrequency(SOAPY_SDR_RX, channel, frequency, args);
        return;
    }
    setFrequency(direction, channel, "RF", frequency, args);
}

void SoapyRPITX::setFrequency(const int direction, const size_t channel, const std::string &name, const double frequency, const SoapySDR::Kwargs &args) {
    if (ForwardRx(direction)) {
        rxDev->setFrequency(SOAPY_SDR_RX, channel, name, frequency, args);
        return;
    }
    if (direction != SOAPY_SDR_TX)
        throw std::runtime_error("rpitx: no receiver attached: only the TX direction can be tuned");
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

double SoapyRPITX::getFrequency(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->getFrequency(SOAPY_SDR_RX, channel);
    return getFrequency(direction, channel, "RF");
}

double SoapyRPITX::getFrequency(const int direction, const size_t channel, const std::string &name) const {
    if (ForwardRx(direction))
        return rxDev->getFrequency(SOAPY_SDR_RX, channel, name);
    std::lock_guard<std::mutex> lock(device_mutex);
    return direction == SOAPY_SDR_TX ? tune.frequency : 0.0;
}

SoapySDR::ArgInfoList SoapyRPITX::getFrequencyArgsInfo(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->getFrequencyArgsInfo(SOAPY_SDR_RX, channel);
    return SoapySDR::ArgInfoList();
}

std::vector<std::string> SoapyRPITX::listFrequencies(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->listFrequencies(SOAPY_SDR_RX, channel);

    std::vector<std::string> names;
    if (direction == SOAPY_SDR_TX)
        names.push_back("RF");
    return names;
}

SoapySDR::RangeList SoapyRPITX::getFrequencyRange(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->getFrequencyRange(SOAPY_SDR_RX, channel);
    return getFrequencyRange(direction, channel, "RF");
}

SoapySDR::RangeList SoapyRPITX::getFrequencyRange(const int direction, const size_t channel, const std::string &name) const {
    if (ForwardRx(direction))
        return rxDev->getFrequencyRange(SOAPY_SDR_RX, channel, name);
    if (direction != SOAPY_SDR_TX)
        return SoapySDR::RangeList();

    std::lock_guard<std::mutex> lock(device_mutex);
    SoapySDR::Range r = PllRange();
    return SoapySDR::RangeList(1, SoapySDR::Range(r.minimum() * tune.harmonic, r.maximum() * tune.harmonic));
}

// ******************** Sample Rate API ********************

void SoapyRPITX::setSampleRate(const int direction, const size_t channel, const double rate) {
    if (ForwardRx(direction)) {
        rxDev->setSampleRate(SOAPY_SDR_RX, channel, rate);
        return;
    }
    if (direction != SOAPY_SDR_TX)
        throw std::runtime_error("rpitx: no receiver attached: only the TX direction has a sample rate");
    if (!(rate >= RPITX_MIN_SAMPLE_RATE && rate <= RPITX_MAX_SAMPLE_RATE))
        throw std::runtime_error("rpitx: sample rate " + formatDouble(rate) + " Hz is out of range");

    std::lock_guard<std::mutex> lock(device_mutex);
    SoapyRPITX_TuneConfig t = tune;
    t.sampleRate = (uint32_t) std::lround(rate);
    if (tx_stream) {
        const uint64_t slotRate = (uint64_t) t.sampleRate * tx_stream->transmitter().SlotsPerSample();
        if (slotRate > RPITX_MAX_SAMPLE_RATE)
            throw std::runtime_error("rpitx: sample rate " + formatDouble(rate) + " Hz times slots_per_sample of the open stream exceeds " +
                                     std::to_string(RPITX_MAX_SAMPLE_RATE) + " Hz");
        if (!SoapyRPITX_RingFitsRate(tx_stream->transmitter().RingSize(), (uint32_t) slotRate))
            throw std::runtime_error("rpitx: the DMA ring of the open stream (" + std::to_string(tx_stream->transmitter().RingSize()) +
                                     " slots) is shorter than " + std::to_string(RPITX_MIN_RING_MS) + " ms at " + std::to_string(slotRate) +
                                     " slots/s; close the stream or set ring_size");
    }
    ApplyTune(t);
    SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: sample rate %u Hz", t.sampleRate);
}

double SoapyRPITX::getSampleRate(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->getSampleRate(SOAPY_SDR_RX, channel);
    std::lock_guard<std::mutex> lock(device_mutex);
    return direction == SOAPY_SDR_TX ? (double) tune.sampleRate : 0.0;
}

std::vector<double> SoapyRPITX::listSampleRates(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->listSampleRates(SOAPY_SDR_RX, channel);

    std::vector<double> options;
    if (direction == SOAPY_SDR_TX)
        options = {24000, 48000, 96000, 192000};
    return options;
}

SoapySDR::RangeList SoapyRPITX::getSampleRateRange(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->getSampleRateRange(SOAPY_SDR_RX, channel);

    SoapySDR::RangeList results;
    if (direction == SOAPY_SDR_TX)
        results.push_back(SoapySDR::Range(RPITX_MIN_SAMPLE_RATE, RPITX_MAX_SAMPLE_RATE));
    return results;
}

void SoapyRPITX::setBandwidth(const int direction, const size_t channel, const double bw) {
    if (ForwardRx(direction)) {
        rxDev->setBandwidth(SOAPY_SDR_RX, channel, bw);
        return;
    }
    SoapySDR_logf(SOAPY_SDR_DEBUG, "rpitx: bandwidth %.0f Hz requested; the bandwidth follows the sample rate", bw);
}

double SoapyRPITX::getBandwidth(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->getBandwidth(SOAPY_SDR_RX, channel);
    return getSampleRate(direction, channel);
}

std::vector<double> SoapyRPITX::listBandwidths(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->listBandwidths(SOAPY_SDR_RX, channel);
    return std::vector<double>();
}

SoapySDR::RangeList SoapyRPITX::getBandwidthRange(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->getBandwidthRange(SOAPY_SDR_RX, channel);
    return SoapySDR::RangeList();
}
