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

#include <fcntl.h>
#include <unistd.h>

#include <fstream>
#include <iterator>
#include <string>

#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Registry.hpp>

#include "SoapyRPITX.hpp"
#include "rpi.hpp"

namespace {

// Device-tree strings are NUL terminated; anything after the first NUL is dropped.
std::string readDeviceTreeString(const char *path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::string();
    std::string value((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t nul = value.find('\0');
    if (nul != std::string::npos)
        value.resize(nul);
    while (!value.empty() && (value.back() == '\n' || value.back() == ' '))
        value.pop_back();
    return value;
}

bool startsWith(const std::string &s, const std::string &prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

// Receiver driver announced in the transceiver entries when the "rx" argument is not given.
constexpr const char *kDefaultReceiver = "rtlsdr";

// Enumerates the receivers of a driver through the find function of its module (not through the factory, which is
// enumerating this module); "rx.<key>" arguments are passed as "<key>".
std::vector<SoapySDR::Kwargs> findReceivers(const std::string &driver, const SoapySDR::Kwargs &args) {
    std::vector<SoapySDR::Kwargs> found;
    if (driver == "rpitx")
        return found;
    const SoapySDR::FindFunctions finders = SoapySDR::Registry::listFindFunctions();
    auto finder = finders.find(driver);
    if (finder == finders.end())
        return found;

    SoapySDR::Kwargs rxArgs;
    const std::string prefix = RPITX_RX_PREFIX;
    for (const auto &kv : args)
        if (startsWith(kv.first, prefix) && kv.first.size() > prefix.size())
            rxArgs[kv.first.substr(prefix.size())] = kv.second;
    rxArgs["driver"] = driver;
    try {
        found = finder->second(rxArgs);
    } catch (const std::exception &e) {
        SoapySDR_logf(SOAPY_SDR_DEBUG, "rpitx: enumerating %s receivers failed: %s", driver.c_str(), e.what());
        found.clear();
    }
    return found;
}

// Transceiver entry: the transmitter with one receiver, opened again with rx=<driver> and its serial.
SoapySDR::Kwargs transceiverEntry(const SoapySDR::Kwargs &tx, const std::string &driver, const SoapySDR::Kwargs &rx) {
    SoapySDR::Kwargs entry = tx;
    auto label = rx.find("label");
    entry["label"] = "RPITX transceiver :: " + tx.at("product") + " + " + (label != rx.end() ? label->second : driver);
    entry[RPITX_RX_ARG] = driver;
    auto rxSerial = rx.find("serial");
    if (rxSerial != rx.end())
        entry[std::string(RPITX_RX_PREFIX) + "serial"] = rxSerial->second;
    auto tuner = rx.find("tuner");
    entry["tuner"] = tuner != rx.end() ? tuner->second : driver;
    return entry;
}

} // namespace

SoapyRPITX_BoardInfo SoapyRPITX_DetectBoard() {
    SoapyRPITX_BoardInfo info;
    info.model = readDeviceTreeString(RPITX_DT_ROOT "/model");
    info.serial = readDeviceTreeString(RPITX_DT_ROOT "/serial-number");

    if (!startsWith(info.model, "Raspberry Pi")) {
        info.reason = info.model.empty() ? "not a Raspberry Pi (no device-tree model)" : "not a Raspberry Pi (" + info.model + ")";
        return info;
    }
    if (startsWith(info.model, "Raspberry Pi 5") || info.model.find("Compute Module 5") != std::string::npos) {
        info.reason = info.model + " (BCM2712/RP1) is not supported by librpitx";
        return info;
    }

    unsigned base = bcm_host_get_peripheral_address();
    if (base != 0x20000000 && base != 0x3F000000 && base != 0xFE000000) {
        info.reason = "unknown peripheral base address (BCM2711 high-peripheral mode, arm_peri_high=1, is not supported)";
        return info;
    }

    info.pi4 = (base == 0xFE000000);
    info.supported = true;
    return info;
}

bool SoapyRPITX_HasHardwareAccess() {
    // AT_EACCESS checks the effective IDs, which open() uses as well.
    bool mem = faccessat(AT_FDCWD, RPITX_DEV_MEM, R_OK | W_OK, AT_EACCESS) == 0;
    bool vcio = faccessat(AT_FDCWD, RPITX_DEV_VCIO, R_OK | W_OK, AT_EACCESS) == 0;
    return mem && vcio;
}

static std::vector<SoapySDR::Kwargs> find_RPITX(const SoapySDR::Kwargs &args) {
    std::vector<SoapySDR::Kwargs> results;

    SoapyRPITX_BoardInfo board = SoapyRPITX_DetectBoard();
    if (!board.supported)
        return results;

    auto serialArg = args.find("serial");
    if (serialArg != args.end() && serialArg->second != board.serial)
        return results;

    SoapySDR::Kwargs devInfo;
    devInfo["label"] = "RPITX GPIO transmitter :: " + board.model;
    devInfo["product"] = board.model;
    devInfo["serial"] = board.serial;
    devInfo["manufacturer"] = "LU3VEA";
    devInfo["tuner"] = "none";

    // Without "rx" the TX-only device comes first (the default of a bare driver=rpitx), followed by one
    // transceiver entry per RTL-SDR; with "rx" only the entries of that receiver are listed.
    auto rx = args.find(RPITX_RX_ARG);
    if (rx == args.end()) {
        results.push_back(devInfo);
        for (const SoapySDR::Kwargs &r : findReceivers(kDefaultReceiver, args))
            results.push_back(transceiverEntry(devInfo, kDefaultReceiver, r));
    } else if (rx->second.empty() || rx->second == "none") {
        results.push_back(devInfo);
    } else {
        std::vector<SoapySDR::Kwargs> found = findReceivers(rx->second, args);
        for (const SoapySDR::Kwargs &r : found)
            results.push_back(transceiverEntry(devInfo, rx->second, r));
        if (found.empty()) {
            // Opening reports why the receiver is not available.
            SoapySDR::Kwargs entry = transceiverEntry(devInfo, rx->second, SoapySDR::Kwargs());
            results.push_back(entry);
        }
    }

    return results;
}

static SoapySDR::Device *make_RPITX(const SoapySDR::Kwargs &args) {
    return new SoapyRPITX(args);
}

static SoapySDR::Registry register_rpitx("rpitx", &find_RPITX, &make_RPITX, SOAPY_SDR_ABI_VERSION);
