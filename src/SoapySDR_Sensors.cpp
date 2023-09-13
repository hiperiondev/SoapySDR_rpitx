/*
 * Copyright 2023 Emiliano Gonzalez (egonzalez . hiperion @ gmail . com))
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

#include "SoapyRPITX.hpp"
#include "libRPITX.hpp"

/*******************************************************************
 * Sensor API
 ******************************************************************/

bool SoapyRPITX::is_sensor_channel(int channel) const {
    // TODO:
    return false;
}

double SoapyRPITX::double_from_buf(const char *buf) const {
    std::istringstream val_as_string(buf);
    val_as_string.imbue(std::locale::classic()); // ignore global C++ locale

    double val = 0.0;
    val_as_string >> val;

    return val;
}

double SoapyRPITX::get_sensor_value(int channel) const {
    // TODO:

    return 0;
}

std::string SoapyRPITX::id_to_unit(const std::string &id) const {
    // TODO:
    return std::string();
}

std::vector<std::string> SoapyRPITX::listSensors(void) const {
    // TODO:
    std::vector<std::string> sensors;
    // TODO:
    sensors.push_back("rpitx");

    return sensors;
}

SoapySDR::ArgInfo SoapyRPITX::getSensorInfo(const std::string &key) const {
    SoapySDR::ArgInfo info;

    // TODO:

    return info;
}

std::string SoapyRPITX::readSensor(const std::string &key) const {
    std::string sensorValue;
    // TODO:

    return sensorValue;
}
