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

#include "SoapyRPITX.hpp"

float libRPITX_TX_frequency = 144e6;
float libRPITX_PPM = 0;
int libRPITX_fifoSize = 1000;
double libRPITX_Gain = 0;
bool libRPITX_GainMode = false;

SoapyRPITX::SoapyRPITX(const SoapySDR::Kwargs &args) {
    SoapySDR_logf(SOAPY_SDR_INFO, "Opening %s...", args.at("label").c_str());
}

SoapyRPITX::~SoapyRPITX(void) {
    // TODO:
}

/*******************************************************************
 * Identification API
 ******************************************************************/
std::string SoapyRPITX::getDriverKey(void) const {
    return "rpitx";
}

std::string SoapyRPITX::getHardwareKey(void) const {
    return "rpitx";
}

SoapySDR::Kwargs SoapyRPITX::getHardwareInfo(void) const {
    SoapySDR::Kwargs info;

    info["library_version"] = "1.0.0";
    info["backend_version"] = "1.0.0";
    info["manufacturer"] = "LU3VEA";
    info["serial"] = "03071973";
    info["label"] = "RPITX I/Q transmitter";
    info["origin"] = "https://github.com/hiperiondev/SoapySDR_rpitx";

    return info;
}

/*******************************************************************
 * Channels API
 ******************************************************************/
size_t SoapyRPITX::getNumChannels(const int dir) const {
    return (1);
}

bool SoapyRPITX::getFullDuplex(const int direction, const size_t channel) const {
    return (false);
}

/*******************************************************************
 * Settings API
 ******************************************************************/
SoapySDR::ArgInfoList SoapyRPITX::getSettingInfo(void) const {
    SoapySDR::ArgInfoList setArgs;

    return setArgs;
}

void SoapyRPITX::writeSetting(const std::string &key, const std::string &value) {
    // TODO:
}

std::string SoapyRPITX::readSetting(const std::string &key) const {
    std::string info;
    // TODO:
    return info;
}

/*******************************************************************
 * Antenna API
 ******************************************************************/
std::vector<std::string> SoapyRPITX::listAntennas(const int direction, const size_t channel) const {
    std::vector<std::string> options;

    if (direction == SOAPY_SDR_TX) {
        options.push_back("Antenna 1");
        options.push_back("Antenna 2");
    }

    return (options);
}

void SoapyRPITX::setAntenna(const int direction, const size_t channel, const std::string &name) {
    if (direction == SOAPY_SDR_TX) {
        // TODO:
    }
}

std::string SoapyRPITX::getAntenna(const int direction, const size_t channel) const {
    std::string options;
    // TODO:

    if (direction == SOAPY_SDR_TX) {
        options = "TX antenna";
    }

    return options;
}

/*******************************************************************
 * Frontend corrections API
 ******************************************************************/
bool SoapyRPITX::hasDCOffsetMode(const int direction, const size_t channel) const {
    return (false);
}

/*******************************************************************
 * Gain API
 ******************************************************************/
std::vector<std::string> SoapyRPITX::listGains(const int direction, const size_t channel) const {
    std::vector<std::string> options;
    options.push_back("rpitx_gain_0");
    return (options);
}

bool SoapyRPITX::hasGainMode(const int direction, const size_t channel) const {
    if (direction == SOAPY_SDR_TX)
        return false;
    return false;
}

void SoapyRPITX::setGainMode(const int direction, const size_t channel, const bool automatic) {
    if (direction == SOAPY_SDR_TX) {
        libRPITX_GainMode = automatic;
    }
}

bool SoapyRPITX::getGainMode(const int direction, const size_t channel) const {
    return libRPITX_GainMode;
}

void SoapyRPITX::setGain(const int direction, const size_t channel, const double value) {
    if (direction == SOAPY_SDR_TX) {
        libRPITX_Gain = value;
    }
}

void SoapyRPITX::setGain(const int direction, const size_t channel, const std::string &name, const double value) {
    this->setGain(direction, channel, value);
}

double SoapyRPITX::getGain(const int direction, const size_t channel, const std::string &name) const {
    if (direction == SOAPY_SDR_TX) {
        return libRPITX_Gain;
    }
    return 0;
}

SoapySDR::Range SoapyRPITX::getGainRange(const int direction, const size_t channel, const std::string &name) const {
    if (direction == SOAPY_SDR_TX)
        return (SoapySDR::Range(0, 0));

    return (SoapySDR::Range(0, 0));
}

/*******************************************************************
 * Frequency API
 ******************************************************************/
void SoapyRPITX::setFrequency(const int direction, const size_t channel, const std::string &name, const double frequency, const SoapySDR::Kwargs &args) {
    SoapySDR_logf(SOAPY_SDR_NOTICE, "FREQUENCY: %f", frequency);
    if (direction == SOAPY_SDR_TX)
        libRPITX_TX_frequency = frequency;
}

double SoapyRPITX::getFrequency(const int direction, const size_t channel, const std::string &name) const {
    if (direction == SOAPY_SDR_TX)
        return libRPITX_TX_frequency;

    return 0;
}

SoapySDR::ArgInfoList SoapyRPITX::getFrequencyArgsInfo(const int direction, const size_t channel) const {
    // TODO:
    SoapySDR::ArgInfoList freqArgs;

    return freqArgs;
}

std::vector<std::string> SoapyRPITX::listFrequencies(const int direction, const size_t channel) const {
    // TODO:
    std::vector<std::string> names;
    names.push_back("RF");
    return (names);
}

SoapySDR::RangeList SoapyRPITX::getFrequencyRange(const int direction, const size_t channel, const std::string &name) const {
    return (SoapySDR::RangeList(1, SoapySDR::Range(1, 600000000ull)));
}

/*******************************************************************
 * Sample Rate API
 ******************************************************************/
void SoapyRPITX::setSampleRate(const int direction, const size_t channel, const double rate) {
    SoapySDR_logf(SOAPY_SDR_NOTICE, "SAMPLE RATE: %f", rate);
    if (direction == SOAPY_SDR_TX) {

    }
}

double SoapyRPITX::getSampleRate(const int direction, const size_t channel) const {
    if (direction == SOAPY_SDR_TX) {
        return 48000;
    }

    return 0;
}

std::vector<double> SoapyRPITX::listSampleRates(const int direction, const size_t channel) const {
    std::vector<double> options;

    options.push_back(48000);

    return (options);
}

SoapySDR::RangeList SoapyRPITX::getSampleRateRange(const int direction, const size_t channel) const {
    SoapySDR::RangeList results;

    results.push_back(SoapySDR::Range(48000, 48000));

    return results;
}

void SoapyRPITX::setBandwidth(const int direction, const size_t channel, const double bw) {
    if (direction == SOAPY_SDR_TX) {
        // TODO:
    }
}

double SoapyRPITX::getBandwidth(const int direction, const size_t channel) const {
    if (direction == SOAPY_SDR_TX) {
        // TODO:
        return 0;
    }

    return 0;
}

std::vector<double> SoapyRPITX::listBandwidths(const int direction, const size_t channel) const {
    std::vector<double> options;
    // TODO:
    //options.push_back(0.2e6);

    return (options);
}
