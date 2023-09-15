/*
 * Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com))
 * * Project Site: https://github.com/hiperiondev/SoapySDR_rpitx *
 *
 * This is based on other projects:
 *    librpitx (https://github.com/F5OEO/librpitx)
 *        Copyright (C) 2018  Evariste COURJAUD F5OEO
 *    SoapyPlutoSDR (https://github.com/pothosware/SoapyPlutoSDR)
 *
 *    please contact their authors for more information.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 */

#include <SoapySDR/Registry.hpp>
#include "SoapyRPITX.hpp"

static std::vector<SoapySDR::Kwargs> find_RPITX(const SoapySDR::Kwargs &args) {
    std::vector<SoapySDR::Kwargs> results;

    SoapySDR::Kwargs devInfo;
    devInfo["label"] = "rpitx";
    devInfo["product"] = "rpitx";
    devInfo["serial"] = "03071973";
    devInfo["manufacturer"] = "LU3VEA";
    devInfo["label"] = "RPITX I/Q transmitter";
    devInfo["tuner"] = "none";
    results.push_back(devInfo);

    return results;
}

static SoapySDR::Device* make_RPITX(const SoapySDR::Kwargs &args) {
    return new SoapyRPITX(args);
}

static SoapySDR::Registry register_rpitx("rpitx", &find_RPITX, &make_RPITX, SOAPY_SDR_ABI_VERSION);
