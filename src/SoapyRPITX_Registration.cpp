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
    results.push_back(devInfo);

    return results;
}

static SoapySDR::Device *make_RPITX(const SoapySDR::Kwargs &args) {
    return new SoapyRPITX(args);
}

static SoapySDR::Registry register_rpitx("rpitx", &find_RPITX, &make_RPITX, SOAPY_SDR_ABI_VERSION);
