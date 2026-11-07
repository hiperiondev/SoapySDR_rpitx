// Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
// Project Site: https://github.com/hiperiondev/SoapySDR_rpitx
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

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Registry.hpp>

#include "SoapyRPITX.hpp"

// The receiver is opened through the factory function registered by its module rather than with
// SoapySDR::Device::make(): this device is itself being created by the SoapySDR factory, and calling the module
// directly needs no factory lock and keeps the receiver out of the factory device table (this device owns it).
void SoapyRPITX::OpenReceiver(const std::string &driver, const SoapySDR::Kwargs &args) {
    if (driver == "rpitx")
        throw std::runtime_error("rpitx: rx=rpitx is not a receiver");

    const SoapySDR::MakeFunctions makers = SoapySDR::Registry::listMakeFunctions();
    auto maker = makers.find(driver);
    if (maker == makers.end())
        throw std::runtime_error("rpitx: receiver driver '" + driver +
                                 "' is not installed (no SoapySDR module registers it; for an RTL-SDR install "
                                 "soapysdr-module-rtlsdr)");

    SoapySDR::Kwargs rxArgs;
    const std::string prefix = RPITX_RX_PREFIX;
    for (const auto &kv : args)
        if (kv.first.compare(0, prefix.size(), prefix) == 0 && kv.first.size() > prefix.size())
            rxArgs[kv.first.substr(prefix.size())] = kv.second;
    rxArgs["driver"] = driver;

    SoapySDR::Device *dev = nullptr;
    try {
        dev = maker->second(rxArgs);
    } catch (const std::exception &e) {
        throw std::runtime_error("rpitx: cannot open the receiver '" + driver + "': " + e.what());
    }
    if (dev == nullptr)
        throw std::runtime_error("rpitx: cannot open the receiver '" + driver + "'");
    std::unique_ptr<SoapySDR::Device> owned(dev);
    if (owned->getNumChannels(SOAPY_SDR_RX) == 0)
        throw std::runtime_error("rpitx: the device opened with rx=" + driver + " has no RX channel");

    rxDev = std::move(owned);
    rxDriver = driver;
    SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: receiver %s (%s) attached as RX channel 0", rxDriver.c_str(), rxDev->getHardwareKey().c_str());
}

bool SoapyRPITX::ForwardRx(const int direction) const {
    return direction == SOAPY_SDR_RX && rxDev != nullptr;
}

SoapySDR::Device &SoapyRPITX::Rx() const {
    if (!rxDev)
        throw std::runtime_error("rpitx: no receiver attached (open the device with rx=rtlsdr)");
    return *rxDev;
}

bool SoapyRPITX::IsValidRxStreamHandle(SoapySDR::Stream *handle) const {
    return handle != nullptr && reinterpret_cast<uintptr_t>(handle) == rxHandle.load();
}

SoapySDR::Stream *SoapyRPITX::SetupRxStream(const std::string &format, const std::vector<size_t> &channels, const SoapySDR::Kwargs &args) {
    SoapySDR::Device &rx = Rx();
    if (channels.size() > 1 || (channels.size() == 1 && channels[0] != 0))
        throw std::runtime_error("rpitx: only RX channel 0 is available");

    std::lock_guard<std::mutex> lock(device_mutex);
    if (rxStream != nullptr)
        throw std::runtime_error("rpitx: an RX stream is already open");

    const size_t elemBytes = SoapySDR::formatToSize(format);
    if (elemBytes == 0 || format.empty() || format[0] != 'C')
        throw std::runtime_error("rpitx: invalid RX stream format '" + format + "'");

    SoapySDR::Stream *inner = rx.setupStream(SOAPY_SDR_RX, format, std::vector<size_t>{0}, args);
    if (inner == nullptr)
        throw std::runtime_error("rpitx: the receiver could not create the RX stream");

    rxStream = inner;
    // Silence of one complex element: zero for signed and float formats, mid-scale for unsigned ones (packed
    // CU4 and CU12 included). Written before the handle is published, read by MuteRx() after it.
    rxElemBytes = std::min<size_t>(elemBytes, rxSilence.size());
    rxSilence.fill(0);
    rxSilenceZero = true;
    if (format.size() > 1 && format[1] == 'U') {
        rxSilenceZero = false;
        if (format == SOAPY_SDR_CU4) {
            rxSilence[0] = 0x88;
        } else if (format == SOAPY_SDR_CU12) {
            // Two 12-bit values of 0x800 packed little-endian in 3 bytes.
            rxSilence[1] = 0x08;
            rxSilence[2] = 0x80;
        } else {
            // Byte-aligned components: little-endian mid-scale (only the top bit of each component set).
            const size_t component = rxElemBytes / 2;
            for (size_t c = 0; c < 2 && component > 0; c++)
                rxSilence[c * component + component - 1] = 0x80;
        }
    }
    if (++streamGeneration == 0)
        streamGeneration = 1;
    rxStreamPtr.store(inner);
    rxHandle.store(streamGeneration);
    SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: RX stream %s on %s", format.c_str(), rxDriver.c_str());
    return reinterpret_cast<SoapySDR::Stream *>(streamGeneration);
}

SoapySDR::Stream *SoapyRPITX::EnterRxCall(SoapySDR::Stream *handle) {
    rxCalls.fetch_add(1);
    const uintptr_t token = rxHandle.load();
    if (handle == nullptr || token == 0 || reinterpret_cast<uintptr_t>(handle) != token) {
        rxCalls.fetch_sub(1);
        return nullptr;
    }
    return rxStreamPtr.load();
}

void SoapyRPITX::LeaveRxCall() {
    rxCalls.fetch_sub(1);
}

void SoapyRPITX::ReleaseRxStreamLocked() {
    rxHandle.store(0);
    if (rxStream == nullptr)
        return;

    // The receiver returns from readStream() within the timeout of the call.
    const auto start = std::chrono::steady_clock::now();
    bool warned = false;
    while (rxCalls.load() != 0) {
        if (!warned && std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: waiting for readStream() / readStreamStatus() to return before closing the RX stream");
            warned = true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    try {
        rxDev->deactivateStream(rxStream);
    } catch (const std::exception &e) {
        SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: deactivating the RX stream: %s", e.what());
    }
    try {
        rxDev->closeStream(rxStream);
    } catch (const std::exception &e) {
        SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: closing the RX stream: %s", e.what());
    }
    rxStream = nullptr;
    rxStreamPtr.store(nullptr);
}

void SoapyRPITX::MuteRx(void *const *buffs, size_t numElems) const {
    if (!rxMuteTx.load() || numElems == 0 || !ptt->MutedNow(rxMuteTailMs.load()))
        return;

    uint8_t *p = static_cast<uint8_t *>(buffs[0]);
    if (rxSilenceZero) {
        memset(p, 0, numElems * rxElemBytes);
        return;
    }
    for (size_t i = 0; i < numElems; i++, p += rxElemBytes)
        memcpy(p, rxSilence.data(), rxElemBytes);
}
