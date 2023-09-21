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

#include <stddef.h>
#include <complex>
#include <unistd.h>
#include <pthread.h>

#include "librpitx.hpp"
#include "SoapyRPITX.hpp"

std::vector<std::string> SoapyRPITX::getStreamFormats(const int direction, const size_t channel) const {
    std::vector<std::string> formats;

    formats.push_back(SOAPY_SDR_CS16);
    formats.push_back(SOAPY_SDR_CF32);

    return formats;
}

std::string SoapyRPITX::getNativeStreamFormat(const int direction, const size_t channel, double &fullScale) const {
    if (direction == SOAPY_SDR_TX) {
        fullScale = 32768;
    }

    return SOAPY_SDR_CF32;
}

SoapySDR::ArgInfoList SoapyRPITX::getStreamArgsInfo(const int direction, const size_t channel) const {
    SoapySDR::ArgInfoList streamArgs;

    SoapySDR::ArgInfo buffersArg;
    buffersArg.key = "buffers";
    buffersArg.value = std::to_string(4000);
    buffersArg.name = "Buffer Count";
    buffersArg.description = "Number of buffers per read.";
    buffersArg.units = "buffers";
    buffersArg.type = SoapySDR::ArgInfo::INT;
    streamArgs.push_back(buffersArg);

    return streamArgs;
}

bool SoapyRPITX::IsValidTxStreamHandle(SoapySDR::Stream *handle) const {
    if (handle == nullptr) {
        return false;
    }

    if (tx_stream) {
        if (reinterpret_cast<tx_streamer*>(handle) == tx_stream.get()) {
            return true;
        }
    }

    return false;
}

SoapySDR::Stream* SoapyRPITX::setupStream(const int direction, const std::string &format, const std::vector<size_t> &channels, const SoapySDR::Kwargs &args) {
    if (direction == SOAPY_SDR_TX) {
        rpitxStreamFormat streamFormat;
        if (format == SOAPY_SDR_CF32) {
            SoapySDR_log(SOAPY_SDR_INFO, "Using format CF32.");
            streamFormat = RPITX_SDR_CF32;
        } else {
            throw std::runtime_error("setupStream invalid format '" + format + "' -- Only CS16 and CF32 are supported by RPITX module.");
        }

        std::lock_guard<rpitx_spin_mutex> lock(tx_device_mutex);

        this->tx_stream = std::unique_ptr<tx_streamer>(new tx_streamer(streamFormat, args));

        return reinterpret_cast<SoapySDR::Stream*>(this->tx_stream.get());
    }

    return nullptr;
}

void SoapyRPITX::closeStream(SoapySDR::Stream *handle) {
    //scope lock :
    {
        std::lock_guard<rpitx_spin_mutex> lock(tx_device_mutex);

        if (IsValidTxStreamHandle(handle)) {
            this->tx_stream.reset();
        }
    }
}

size_t SoapyRPITX::getStreamMTU(SoapySDR::Stream *handle) const {
    if (IsValidTxStreamHandle(handle)) {
        return libRPITX_fifoSize;
    }

    return 0;
}

int SoapyRPITX::activateStream(SoapySDR::Stream *handle, const int flags, const long long timeNs, const size_t numElems) {
    // TODO:
    if (flags & ~SOAPY_SDR_END_BURST)
        return SOAPY_SDR_NOT_SUPPORTED;

    SoapySDR_logf(SOAPY_SDR_INFO, "Start TX");

    return 0;
}

int SoapyRPITX::deactivateStream(SoapySDR::Stream *handle, const int flags, const long long timeNs) {
    SoapySDR_logf(SOAPY_SDR_INFO, "Stop TX");

    return 0;
}

int SoapyRPITX::readStream(SoapySDR::Stream *handle, void *const*buffs, const size_t numElems, int &flags, long long &timeNs, const long timeoutUs) {
    return SOAPY_SDR_NOT_SUPPORTED;
}

int SoapyRPITX::writeStream(SoapySDR::Stream *handle, const void *const*buffs, const size_t numElems, int &flags, const long long timeNs,
        const long timeoutUs) {
    std::lock_guard<rpitx_spin_mutex> lock(tx_device_mutex);

    if (IsValidTxStreamHandle(handle)) {
        return this->tx_stream->send(buffs, numElems, flags, timeNs, timeoutUs);;
    } else {
        return SOAPY_SDR_NOT_SUPPORTED;
    }
}

int SoapyRPITX::readStreamStatus(SoapySDR::Stream *stream, size_t &chanMask, int &flags, long long &timeNs, const long timeoutUs) {
    return SOAPY_SDR_NOT_SUPPORTED;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
tx_streamer::tx_streamer(const rpitxStreamFormat _format, const SoapySDR::Kwargs &args) {
    iqsender = new iqdmasync(libRPITX_Frequency, 48000, 14, libRPITX_fifoSize);
    iqsender->Setppm(ppmpll);
}

tx_streamer::~tx_streamer() {
    delete (iqsender);
}

int tx_streamer::send(const void *const*buffs, const size_t numElems, int &flags, const long long timeNs, const long timeoutUs) {
    int elemToSend = (numElems >= (libRPITX_fifoSize * 2) * 3 / 4) ? ((libRPITX_fifoSize * 2) * 3 / 4) : numElems;

    switch (format) {
        case RPITX_SDR_CF32:
            iqsender->SetIQSamples2((float*) buffs[0], elemToSend, Harmonic, timeoutUs);
            break;

        default:
            SoapySDR_logf(SOAPY_SDR_ERROR, "Stream format not allowed");
            throw std::runtime_error("Stream format not allowed");
    }

    return numElems;
}

int tx_streamer::flush() {
    return 0;
}
