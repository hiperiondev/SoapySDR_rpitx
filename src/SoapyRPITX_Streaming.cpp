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

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <complex>
#include <stdexcept>
#include <thread>

#include "SoapyRPITX.hpp"

namespace {

// Size of the conversion buffer, in samples.
constexpr size_t kScratchSize = 4096;

// Stream argument defaults (see getStreamArgsInfo()).
constexpr long kDefaultFifoMs = 250;
constexpr long kDefaultPrerollMs = 100;
constexpr long kDefaultDmaChannel = -1;
constexpr long kDefaultRtPriority = 50;
constexpr long kDefaultCpu = -1;

long streamArg(const SoapySDR::Kwargs &args, const char *key, long def, long lo, long hi) {
    auto it = args.find(key);
    if (it == args.end())
        return def;

    long v;
    try {
        size_t used = 0;
        v = std::stol(it->second, &used, 0);
        if (used != it->second.size())
            throw std::invalid_argument(it->second);
    } catch (const std::exception &) {
        throw std::runtime_error(std::string("rpitx: invalid integer '") + it->second + "' for stream argument " + key);
    }
    if (v < lo || v > hi)
        throw std::runtime_error(std::string("rpitx: stream argument ") + key + " out of range [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
    return v;
}

bool streamBoolArg(const SoapySDR::Kwargs &args, const char *key, bool def) {
    auto it = args.find(key);
    if (it == args.end())
        return def;
    std::string v = it->second;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    if (v == "true" || v == "1" || v == "yes" || v == "on")
        return true;
    if (v == "false" || v == "0" || v == "no" || v == "off")
        return false;
    throw std::runtime_error(std::string("rpitx: invalid boolean '") + it->second + "' for stream argument " + key);
}

SoapySDR::ArgInfo boolArg(const char *key, const char *name, const char *description) {
    SoapySDR::ArgInfo arg;
    arg.key = key;
    arg.value = "false";
    arg.name = name;
    arg.description = description;
    arg.type = SoapySDR::ArgInfo::BOOL;
    return arg;
}

SoapySDR::ArgInfo intArg(const char *key, long def, const char *name, const char *description, const char *units, long lo, long hi) {
    SoapySDR::ArgInfo arg;
    arg.key = key;
    arg.value = std::to_string(def);
    arg.name = name;
    arg.description = description;
    arg.units = units;
    arg.type = SoapySDR::ArgInfo::INT;
    arg.range = SoapySDR::Range((double) lo, (double) hi, 1);
    return arg;
}

} // namespace

std::vector<std::string> SoapyRPITX::getStreamFormats(const int direction, const size_t) const {
    std::vector<std::string> formats;

    if (direction == SOAPY_SDR_TX) {
        formats.push_back(SOAPY_SDR_CF32);
        formats.push_back(SOAPY_SDR_CS16);
        formats.push_back(SOAPY_SDR_CS8);
    }

    return formats;
}

std::string SoapyRPITX::getNativeStreamFormat(const int, const size_t, double &fullScale) const {
    fullScale = 1.0;
    return SOAPY_SDR_CF32;
}

SoapySDR::ArgInfoList SoapyRPITX::getStreamArgsInfo(const int direction, const size_t) const {
    SoapySDR::ArgInfoList streamArgs;
    if (direction != SOAPY_SDR_TX)
        return streamArgs;

    uint32_t rate;
    {
        std::lock_guard<std::mutex> lock(device_mutex);
        rate = tune.sampleRate;
    }

    streamArgs.push_back(intArg("ring_size", SoapyRPITX_DefaultRingSize(rate), "DMA ring size",
                                "Number of samples in the circular DMA buffer (default: 85 ms at the sample rate, at least 4096; at least 10 ms).", "samples",
                                RPITX_MIN_RING_SIZE, RPITX_MAX_RING_SIZE));
    streamArgs.push_back(intArg("fifo_ms", kDefaultFifoMs, "FIFO length",
                                "Capacity of the software FIFO between writeStream() and the DMA ring (follows the sample rate).", "ms", 20, 5000));
    streamArgs.push_back(
            intArg("preroll_ms", kDefaultPrerollMs, "Pre-roll", "Signal queued before the DMA starts (absorbs host scheduling jitter).", "ms", 0, 5000));
    streamArgs.push_back(intArg(
            "dma_channel", kDefaultDmaChannel, "DMA channel",
            "DMA channel; -1 selects the first free channel (7-10, 6, 5 on Pi 4; 14 down to 5 on the other models) not reserved for the kernel.", "", -1, 14));
    streamArgs.push_back(boolArg("dma_force", "Force DMA channel", "Reset an explicitly selected DMA channel even when it is active."));
    streamArgs.push_back(
            intArg("rt_priority", kDefaultRtPriority, "Feeder priority", "SCHED_FIFO priority of the feeder thread; 0 keeps normal scheduling.", "", 0, 99));
    streamArgs.push_back(intArg("cpu", kDefaultCpu, "Feeder CPU", "CPU the feeder thread is pinned to; -1 for no affinity.", "", -1, 63));
    streamArgs.push_back(
            boolArg("signal_cleanup", "Signal cleanup", "Turn the carrier off when the process is killed by a signal whose action is the default one."));

    return streamArgs;
}

bool SoapyRPITX::IsValidTxStreamHandle(SoapySDR::Stream *handle) const {
    return handle != nullptr && reinterpret_cast<tx_streamer *>(handle) == streamHandle.load();
}

SoapySDR::Stream *SoapyRPITX::setupStream(const int direction, const std::string &format, const std::vector<size_t> &channels, const SoapySDR::Kwargs &args) {
    if (direction != SOAPY_SDR_TX)
        throw std::runtime_error("rpitx: receive streams are not supported (TX only)");

    rpitxStreamFormat streamFormat;
    if (format == SOAPY_SDR_CF32) {
        streamFormat = RPITX_SDR_CF32;
    } else if (format == SOAPY_SDR_CS16) {
        streamFormat = RPITX_SDR_CS16;
    } else if (format == SOAPY_SDR_CS8) {
        streamFormat = RPITX_SDR_CS8;
    } else {
        throw std::runtime_error("rpitx: invalid stream format '" + format + "'; supported formats are CF32, CS16 and CS8");
    }

    if (channels.size() > 1 || (channels.size() == 1 && channels[0] != 0))
        throw std::runtime_error("rpitx: only channel 0 is available");

    std::lock_guard<std::mutex> lock(device_mutex);

    if (tx_stream)
        throw std::runtime_error("rpitx: a TX stream is already open");

    SoapyRPITX_TxConfig cfg;
    cfg.tune = tune;
    cfg.ringSize = (uint32_t) streamArg(args, "ring_size", SoapyRPITX_DefaultRingSize(tune.sampleRate), RPITX_MIN_RING_SIZE, RPITX_MAX_RING_SIZE);
    if (!SoapyRPITX_RingFitsRate(cfg.ringSize, tune.sampleRate))
        throw std::runtime_error("rpitx: ring_size " + std::to_string(cfg.ringSize) + " is shorter than " + std::to_string(RPITX_MIN_RING_MS) + " ms at " +
                                 std::to_string(tune.sampleRate) + " Hz");
    cfg.fifoMs = (uint32_t) streamArg(args, "fifo_ms", kDefaultFifoMs, 20, 5000);
    cfg.prerollMs = (uint32_t) streamArg(args, "preroll_ms", kDefaultPrerollMs, 0, 5000);
    cfg.dmaChannel = (int) streamArg(args, "dma_channel", kDefaultDmaChannel, -1, 14);
    cfg.dmaForce = streamBoolArg(args, "dma_force", false);
    cfg.signalCleanup = streamBoolArg(args, "signal_cleanup", false);
    cfg.rtPriority = (int) streamArg(args, "rt_priority", kDefaultRtPriority, 0, 99);
    cfg.cpu = (int) streamArg(args, "cpu", kDefaultCpu, -1, 63);
    cfg.driveLevel = driveLevel;
    cfg.iqScale = iqScale;
    cfg.constantEnvelope = constantEnvelope;

    SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: TX stream %s, ring %u samples, FIFO %u ms, pre-roll %u ms", format.c_str(), cfg.ringSize, cfg.fifoMs, cfg.prerollMs);

    tx_stream = std::make_shared<tx_streamer>(streamFormat, cfg);
    streamHandle.store(tx_stream.get());
    return reinterpret_cast<SoapySDR::Stream *>(tx_stream.get());
}

void SoapyRPITX::closeStream(SoapySDR::Stream *handle) {
    std::lock_guard<std::mutex> lock(device_mutex);

    if (IsValidTxStreamHandle(handle)) {
        streamHandle.store(nullptr);
        tx_stream.reset();
    }
}

size_t SoapyRPITX::getStreamMTU(SoapySDR::Stream *handle) const {
    std::lock_guard<std::mutex> lock(device_mutex);

    if (IsValidTxStreamHandle(handle))
        return tx_stream->mtu();

    return 0;
}

int SoapyRPITX::activateStream(SoapySDR::Stream *handle, const int flags, const long long, const size_t) {
    std::lock_guard<std::mutex> lock(device_mutex);

    if (!IsValidTxStreamHandle(handle))
        return SOAPY_SDR_STREAM_ERROR;
    if (flags & SOAPY_SDR_HAS_TIME)
        return SOAPY_SDR_NOT_SUPPORTED;

    SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: start TX");
    return tx_stream->activate();
}

int SoapyRPITX::deactivateStream(SoapySDR::Stream *handle, const int, const long long) {
    std::shared_ptr<tx_streamer> stream;
    long drainUs = 0;
    {
        std::lock_guard<std::mutex> lock(device_mutex);
        if (!IsValidTxStreamHandle(handle))
            return SOAPY_SDR_STREAM_ERROR;
        stream = tx_stream;
        drainUs = stream->drainTimeoutUs();
    }

    // The drain wait does not hold the device mutex, so the control API stays available meanwhile.
    if (drainUs > 0 && !stream->transmitter().WaitDrained(drainUs))
        SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: TX stopped before the last burst was fully sent");

    std::lock_guard<std::mutex> lock(device_mutex);
    if (tx_stream != stream)
        return 0; // closed meanwhile: closing stopped the transmitter
    SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: stop TX");
    return stream->deactivate();
}

int SoapyRPITX::readStream(SoapySDR::Stream *, void *const *, const size_t, int &, long long &, const long) {
    return SOAPY_SDR_NOT_SUPPORTED;
}

int SoapyRPITX::writeStream(SoapySDR::Stream *handle, const void *const *buffs, const size_t numElems, int &flags, const long long timeNs,
                            const long timeoutUs) {
    // Not serialised with device_mutex: blocking here must not stall the control API. The Soapy contract
    // forbids closing a stream while another thread writes to it.
    if (!IsValidTxStreamHandle(handle))
        return SOAPY_SDR_STREAM_ERROR;

    return reinterpret_cast<tx_streamer *>(handle)->send(buffs, numElems, flags, timeNs, timeoutUs);
}

int SoapyRPITX::readStreamStatus(SoapySDR::Stream *handle, size_t &chanMask, int &flags, long long &timeNs, const long timeoutUs) {
    if (!IsValidTxStreamHandle(handle))
        return SOAPY_SDR_STREAM_ERROR;

    return reinterpret_cast<tx_streamer *>(handle)->readStatus(chanMask, flags, timeNs, timeoutUs);
}

// ************************************** TX STREAMER **************************************

tx_streamer::tx_streamer(rpitxStreamFormat _format, const SoapyRPITX_TxConfig &cfg) :
        format(_format),
        iqsender(new SoapyRPITX_IQ_dmaSync(cfg)),
        scratch(kScratchSize) {
}

tx_streamer::~tx_streamer() {
    iqsender->Stop();
}

SoapyRPITX_IQ_dmaSync &tx_streamer::transmitter() {
    return *iqsender;
}

size_t tx_streamer::mtu() const {
    return std::min<size_t>(kScratchSize, std::max<size_t>(iqsender->FifoCapacity() / 4, 256));
}

int tx_streamer::activate() {
    endBurstPending.store(false);
    reportedEvents.store(0);
    iqsender->Start();
    return 0;
}

long tx_streamer::drainTimeoutUs() {
    if (!iqsender->IsActive() || !endBurstPending.load())
        return 0;
    // Let the last burst reach the antenna: FIFO + ring duration plus a margin.
    SoapyRPITX_TuneConfig t = iqsender->GetTune();
    double seconds = (double) (iqsender->FifoCapacity() + iqsender->RingSize()) / (double) t.sampleRate + 0.2;
    return (long) (seconds * 1e6);
}

int tx_streamer::deactivate() {
    iqsender->Stop();
    endBurstPending.store(false);
    return 0;
}

int tx_streamer::send(const void *const *buffs, size_t numElems, int &flags, long long, long timeoutUs) {
    if (!iqsender->IsActive())
        return SOAPY_SDR_STREAM_ERROR;

    if (flags & SOAPY_SDR_HAS_TIME) {
        if (!timeWarningShown) {
            SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: timed transmission is not supported");
            timeWarningShown = true;
        }
        return SOAPY_SDR_NOT_SUPPORTED;
    }

    const bool endOfBurst = (flags & SOAPY_SDR_END_BURST) != 0;
    flags = 0;

    if (numElems == 0) {
        if (endOfBurst) {
            iqsender->EndBurst();
            endBurstPending.store(true);
        }
        return 0;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeoutUs > 0 ? timeoutUs : 0);
    size_t done = 0;

    // The return value is an int: larger requests are accepted partially.
    numElems = std::min<size_t>(numElems, (size_t) INT32_MAX);
    while (done < numElems) {
        size_t n = std::min(numElems - done, scratch.size());

        switch (format) {
            case RPITX_SDR_CF32: {
                const std::complex<float> *in = reinterpret_cast<const std::complex<float> *>(buffs[0]) + done;
                std::copy(in, in + n, scratch.begin());
                break;
            }
            case RPITX_SDR_CS16: {
                const int16_t *in = reinterpret_cast<const int16_t *>(buffs[0]) + 2 * done;
                for (size_t i = 0; i < n; i++)
                    scratch[i] = std::complex<float>(in[2 * i] / 32768.0f, in[2 * i + 1] / 32768.0f);
                break;
            }
            case RPITX_SDR_CS8: {
                const int8_t *in = reinterpret_cast<const int8_t *>(buffs[0]) + 2 * done;
                for (size_t i = 0; i < n; i++)
                    scratch[i] = std::complex<float>(in[2 * i] / 128.0f, in[2 * i + 1] / 128.0f);
                break;
            }
        }

        long remainingUs = 0;
        if (timeoutUs > 0) {
            auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now()).count();
            remainingUs = left > 0 ? (long) left : 0;
        }

        size_t w = iqsender->Write(scratch.data(), n, remainingUs);
        done += w;
        if (w < n)
            break;
    }

    if (done == 0)
        return SOAPY_SDR_TIMEOUT;

    endBurstPending.store(false);
    if (endOfBurst && done == numElems) {
        iqsender->EndBurst();
        endBurstPending.store(true);
        flags = SOAPY_SDR_END_BURST;
    }

    return (int) std::min<size_t>(done, (size_t) INT32_MAX);
}

int tx_streamer::readStatus(size_t &chanMask, int &flags, long long &, long timeoutUs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeoutUs > 0 ? timeoutUs : 0);

    for (;;) {
        SoapyRPITX_TxStats stats = iqsender->GetStats();
        uint64_t events = stats.underflows + stats.late + stats.dmaErrors;
        uint64_t reported = reportedEvents.load();
        if (events < reported)
            reported = 0;
        if (events > reported) {
            reportedEvents.store(events);
            chanMask = 1;
            flags = 0;
            return SOAPY_SDR_UNDERFLOW;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return SOAPY_SDR_TIMEOUT;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
