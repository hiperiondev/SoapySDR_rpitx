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
#include <climits>
#include <complex>
#include <initializer_list>
#include <stdexcept>
#include <thread>

#include "SoapyRPITX.hpp"

namespace {

// Size of the conversion buffer, in samples.
constexpr size_t kScratchSize = 4096;
// Longest uninterrupted sleep of the T/R sequence wait.
constexpr auto kRelaySlice = std::chrono::milliseconds(2);
// Longest blocking time of one call to the receiver; longer timeouts are split into slices of this length.
constexpr long kRxSliceUs = 100000;

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
        v = std::stol(it->second, &used, 10);
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

std::string streamStringArg(const SoapySDR::Kwargs &args, const char *key, const char *def, std::initializer_list<const char *> options) {
    auto it = args.find(key);
    if (it == args.end())
        return def;
    for (const char *o : options)
        if (it->second == o)
            return it->second;
    std::string list;
    for (const char *o : options)
        list += std::string(list.empty() ? "" : ", ") + o;
    throw std::runtime_error(std::string("rpitx: invalid value '") + it->second + "' for stream argument " + key + " (" + list + ")");
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

std::vector<std::string> SoapyRPITX::getStreamFormats(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->getStreamFormats(SOAPY_SDR_RX, channel);

    std::vector<std::string> formats;

    if (direction == SOAPY_SDR_TX) {
        formats.push_back(SOAPY_SDR_CF32);
        formats.push_back(SOAPY_SDR_CS16);
        formats.push_back(SOAPY_SDR_CS8);
    }

    return formats;
}

std::string SoapyRPITX::getNativeStreamFormat(const int direction, const size_t channel, double &fullScale) const {
    if (ForwardRx(direction))
        return rxDev->getNativeStreamFormat(SOAPY_SDR_RX, channel, fullScale);

    fullScale = 1.0;
    return SOAPY_SDR_CF32;
}

SoapySDR::ArgInfoList SoapyRPITX::getStreamArgsInfo(const int direction, const size_t channel) const {
    if (ForwardRx(direction))
        return rxDev->getStreamArgsInfo(SOAPY_SDR_RX, channel);

    SoapySDR::ArgInfoList streamArgs;
    if (direction != SOAPY_SDR_TX)
        return streamArgs;

    uint32_t rate;
    {
        std::lock_guard<std::mutex> lock(device_mutex);
        rate = tune.sampleRate;
    }

    streamArgs.push_back(intArg("ring_size", SoapyRPITX_DefaultRingSize(rate), "DMA ring size",
                                "Number of slots in the circular DMA buffer (default: 85 ms at the slot rate = sample rate x slots_per_sample, rounded up to "
                                "512 slots, at least 4096; the value shown is for slots_per_sample=1; at least 10 ms).",
                                "slots", RPITX_MIN_RING_SIZE, RPITX_MAX_RING_SIZE));
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
    streamArgs.push_back(intArg("slots_per_sample", 1, "Slots per sample",
                                "DMA slots per IQ sample; the IQ signal is interpolated and the amplitude quantised at the slot rate (at most 200 kHz).", "", 1,
                                RPITX_MAX_SLOTS_PER_SAMPLE));
    SoapySDR::ArgInfo env;
    env.key = "envelope_output";
    env.value = "none";
    env.name = "Envelope output";
    env.description = "none: envelope on the pad drive of GPIO4; pcm: 16-bit I2S DAC on GPIO18 (BCLK), GPIO19 (LRCLK), GPIO21 (DOUT); "
                      "pwm: PWM duty on pwm_gpio. With pcm or pwm GPIO4 carries the constant-envelope phase signal (EER).";
    env.type = SoapySDR::ArgInfo::STRING;
    env.options = {"none", "pcm", "pwm"};
    env.optionNames = {"None", "PCM / I2S DAC", "PWM"};
    streamArgs.push_back(env);
    SoapySDR::ArgInfo pin = intArg("pwm_gpio", 18, "PWM envelope pin", "GPIO of the PWM envelope output: 18 (ALT5) or 12 (ALT0).", "", 12, 18);
    pin.options = {"12", "18"};
    pin.optionNames = {"GPIO12", "GPIO18"};
    streamArgs.push_back(pin);
    streamArgs.push_back(intArg("pwm_bits", 8, "PWM envelope resolution", "Bits of the PWM envelope (period of 2^bits PWM clocks).", "bits", 6, 12));
    streamArgs.push_back(boolArg("rf_dac", "RF-DAC", "Use GPIO20 as a second clock output, summed with GPIO4 by an external weighted network."));

    return streamArgs;
}

bool SoapyRPITX::IsValidTxStreamHandle(SoapySDR::Stream *handle) const {
    // The handle is an opaque generation token, never dereferenced: a stale handle of a closed stream cannot
    // match a later stream, even if the new tx_streamer is allocated at the same address.
    return handle != nullptr && reinterpret_cast<uintptr_t>(handle) == streamHandle.load();
}

void SoapyRPITX::ParseStreamArgs(const SoapySDR::Kwargs &sargs, SoapyRPITX_TxConfig &cfg, std::string &envOut) const {
    cfg.slotsPerSample = (uint32_t) streamArg(sargs, "slots_per_sample", 1, 1, RPITX_MAX_SLOTS_PER_SAMPLE);
    const uint64_t slotRate = (uint64_t) cfg.tune.sampleRate * cfg.slotsPerSample;
    cfg.ringSize = (uint32_t) streamArg(sargs, "ring_size", SoapyRPITX_DefaultRingSize((uint32_t) std::min<uint64_t>(slotRate, UINT32_MAX)),
                                        RPITX_MIN_RING_SIZE, RPITX_MAX_RING_SIZE);
    envOut = streamStringArg(sargs, "envelope_output", "none", {"none", "pcm", "pwm"});
    cfg.envelopeOutput = envOut == "pcm" ? SoapyRPITX_EnvelopeOutput::Pcm : envOut == "pwm" ? SoapyRPITX_EnvelopeOutput::Pwm : SoapyRPITX_EnvelopeOutput::None;
    cfg.pwmGpio = (int) streamArg(sargs, "pwm_gpio", 18, 12, 18);
    if (cfg.pwmGpio != 12 && cfg.pwmGpio != 18)
        throw std::runtime_error("rpitx: stream argument pwm_gpio must be 12 or 18");
    cfg.pwmBits = (int) streamArg(sargs, "pwm_bits", 8, 6, 12);
    cfg.rfDac = streamBoolArg(sargs, "rf_dac", false);
    cfg.fifoMs = (uint32_t) streamArg(sargs, "fifo_ms", kDefaultFifoMs, 20, 5000);
    cfg.prerollMs = (uint32_t) streamArg(sargs, "preroll_ms", kDefaultPrerollMs, 0, 5000);
    cfg.dmaChannel = (int) streamArg(sargs, "dma_channel", kDefaultDmaChannel, -1, 14);
    cfg.dmaForce = streamBoolArg(sargs, "dma_force", false);
    cfg.signalCleanup = streamBoolArg(sargs, "signal_cleanup", false);
    cfg.rtPriority = (int) streamArg(sargs, "rt_priority", kDefaultRtPriority, 0, 99);
    cfg.cpu = (int) streamArg(sargs, "cpu", kDefaultCpu, -1, 63);
}

void SoapyRPITX::CheckRelayPin(const SoapyRPITX_TxConfig &cfg) const {
    const SoapyRPITX_RelayConfig r = ptt->GetConfig();
    const struct {
        int pin;
        const char *key;
        const char *what;
    } outputs[] = {{r.gpio, "relay_gpio", "antenna relay"}, {r.pttGpio, "ptt_gpio", "PTT line"}};
    for (const auto &o : outputs) {
        if (o.pin < 0)
            continue;
        bool used = false;
        if (cfg.rfDac && o.pin == 20)
            used = true;
        if (cfg.envelopeOutput == SoapyRPITX_EnvelopeOutput::Pcm && (o.pin == 18 || o.pin == 19 || o.pin == 21))
            used = true;
        if (cfg.envelopeOutput == SoapyRPITX_EnvelopeOutput::Pwm && o.pin == cfg.pwmGpio)
            used = true;
        if (used)
            throw std::runtime_error("rpitx: GPIO" + std::to_string(o.pin) + " is the " + o.what + " (" + o.key + ") and cannot be used by the TX stream");
    }
}

SoapySDR::Stream *SoapyRPITX::setupStream(const int direction, const std::string &format, const std::vector<size_t> &channels, const SoapySDR::Kwargs &args) {
    if (direction == SOAPY_SDR_RX) {
        if (!rxDev)
            throw std::runtime_error("rpitx: no receiver attached; open the device with rx=rtlsdr for an RX stream");
        return SetupRxStream(format, channels, args);
    }
    if (direction != SOAPY_SDR_TX)
        throw std::runtime_error("rpitx: invalid stream direction");

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

    // Default stream arguments of the loaded profile; the arguments of this call take precedence.
    SoapySDR::Kwargs merged = profileStream;
    for (const auto &kv : args)
        merged[kv.first] = kv.second;
    const SoapySDR::Kwargs &sargs = merged;

    SoapyRPITX_TxConfig cfg;
    cfg.tune = tune;
    std::string envOut;
    ParseStreamArgs(sargs, cfg, envOut);
    CheckRelayPin(cfg);
    const uint64_t slotRate = (uint64_t) tune.sampleRate * cfg.slotsPerSample;
    if (slotRate > RPITX_MAX_SAMPLE_RATE)
        throw std::runtime_error("rpitx: sample rate x slots_per_sample (" + std::to_string(slotRate) + " Hz) exceeds " +
                                 std::to_string(RPITX_MAX_SAMPLE_RATE) + " Hz");
    if (!SoapyRPITX_RingFitsRate(cfg.ringSize, (uint32_t) slotRate))
        throw std::runtime_error("rpitx: ring_size " + std::to_string(cfg.ringSize) + " is shorter than " + std::to_string(RPITX_MIN_RING_MS) + " ms at " +
                                 std::to_string(slotRate) + " slots/s");
    cfg.amplitude = ampCfg;
    cfg.driveLevel = driveLevel;
    cfg.iqScale = iqScale;
    cfg.constantEnvelope = constantEnvelope;

    SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: TX stream %s, ring %u slots, %u slot(s) per sample, FIFO %u ms, pre-roll %u ms, envelope output %s%s", format.c_str(),
                  cfg.ringSize, cfg.slotsPerSample, cfg.fifoMs, cfg.prerollMs, envOut.c_str(), cfg.rfDac ? ", RF-DAC" : "");

    tx_stream = std::make_shared<tx_streamer>(streamFormat, cfg, ptt);

    // Every stream gets a new token (0 is reserved for "no stream"; the counter is only changed under device_mutex).
    if (++streamGeneration == 0)
        streamGeneration = 1;
    streamPtr.store(tx_stream.get());
    streamHandle.store(streamGeneration);
    return reinterpret_cast<SoapySDR::Stream *>(streamGeneration);
}

tx_streamer *SoapyRPITX::EnterStreamCall(SoapySDR::Stream *handle) {
    // Register first, validate afterwards (both sequentially consistent): closeStream() clears the handle and
    // then waits for the counter, so either this call sees the cleared handle, or closeStream() sees this call.
    streamCalls.fetch_add(1);
    const uintptr_t token = streamHandle.load();
    if (handle == nullptr || token == 0 || reinterpret_cast<uintptr_t>(handle) != token) {
        streamCalls.fetch_sub(1);
        return nullptr;
    }
    // The token matched while this call is registered: closeStream() cannot finish (nor a new stream be set
    // up) before LeaveStreamCall(), so streamPtr is the stream this token was issued for.
    return streamPtr.load();
}

void SoapyRPITX::LeaveStreamCall() {
    streamCalls.fetch_sub(1);
}

void SoapyRPITX::ReleaseTxStreamLocked() {
    // From here no new data-path call can obtain the stream.
    streamHandle.store(0);
    if (!tx_stream)
        return;

    // Stopping also ends a drain in progress in deactivateStream(), which then releases its reference, and wakes
    // a writer blocked on a full FIFO and a readStreamStatus() poll.
    tx_stream->interrupt();

    // The data-path calls do not take device_mutex and return promptly now; destroying the stream (its
    // condition variable included) before they are gone would be undefined behaviour.
    const auto start = std::chrono::steady_clock::now();
    bool warned = false;
    while (streamCalls.load() != 0) {
        if (!warned && std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            SoapySDR_logf(SOAPY_SDR_WARNING, "rpitx: waiting for writeStream() / readStreamStatus() to return before closing the stream");
            warned = true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    tx_stream.reset();
    streamPtr.store(nullptr);
}

void SoapyRPITX::closeStream(SoapySDR::Stream *handle) {
    std::lock_guard<std::mutex> lock(device_mutex);

    if (IsValidTxStreamHandle(handle))
        ReleaseTxStreamLocked();
    else if (IsValidRxStreamHandle(handle))
        ReleaseRxStreamLocked();
}

size_t SoapyRPITX::getStreamMTU(SoapySDR::Stream *handle) const {
    std::lock_guard<std::mutex> lock(device_mutex);

    if (IsValidTxStreamHandle(handle))
        return tx_stream->mtu();
    if (IsValidRxStreamHandle(handle))
        return rxDev->getStreamMTU(rxStream);

    return 0;
}

int SoapyRPITX::activateStream(SoapySDR::Stream *handle, const int flags, const long long timeNs, const size_t numElems) {
    std::lock_guard<std::mutex> lock(device_mutex);

    if (IsValidRxStreamHandle(handle)) {
        SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: start RX");
        return rxDev->activateStream(rxStream, flags, timeNs, numElems);
    }
    if (!IsValidTxStreamHandle(handle))
        return SOAPY_SDR_STREAM_ERROR;
    if (flags & SOAPY_SDR_HAS_TIME)
        return SOAPY_SDR_NOT_SUPPORTED;

    SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: start TX");
    return tx_stream->activate();
}

int SoapyRPITX::deactivateStream(SoapySDR::Stream *handle, const int flags, const long long timeNs) {
    std::shared_ptr<tx_streamer> stream;
    long drainUs = 0;
    {
        std::lock_guard<std::mutex> lock(device_mutex);
        if (IsValidRxStreamHandle(handle)) {
            SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: stop RX");
            return rxDev->deactivateStream(rxStream, flags, timeNs);
        }
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

int SoapyRPITX::readStream(SoapySDR::Stream *handle, void *const *buffs, const size_t numElems, int &flags, long long &timeNs, const long timeoutUs) {
    // Not serialised with device_mutex, like writeStream(); closeStream() waits for the registered call.
    SoapySDR::Stream *inner = EnterRxCall(handle);
    if (inner == nullptr)
        return SOAPY_SDR_STREAM_ERROR;

    // The receiver is called with timeouts of at most kRxSliceUs, so that closeStream() never waits longer than one
    // slice for a read in flight; between slices the call ends when the stream was closed.
    const auto deadline = SoapyRPITX_DeadlineAfter(timeoutUs);
    int ret;
    try {
        for (;;) {
            long slice = 0;
            if (timeoutUs > 0) {
                const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now()).count();
                slice = (long) std::max<long long>(0, std::min<long long>(left, kRxSliceUs));
            }
            int sliceFlags = flags;
            ret = rxDev->readStream(inner, buffs, numElems, sliceFlags, timeNs, slice);
            flags = sliceFlags;
            if (ret != SOAPY_SDR_TIMEOUT || slice == 0 || std::chrono::steady_clock::now() >= deadline)
                break;
            if (!IsValidRxStreamHandle(handle)) {
                ret = SOAPY_SDR_STREAM_ERROR;
                break;
            }
        }
    } catch (...) {
        LeaveRxCall();
        throw;
    }
    if (ret > 0)
        MuteRx(buffs, (size_t) ret);
    LeaveRxCall();
    return ret;
}

int SoapyRPITX::writeStream(SoapySDR::Stream *handle, const void *const *buffs, const size_t numElems, int &flags, const long long timeNs,
                            const long timeoutUs) {
    // Not serialised with device_mutex: blocking here must not stall the control API. The call is registered, so
    // closeStream() / the destructor wait for it instead of destroying the stream under it.
    StreamCall call(*this, handle);
    if (call.get() == nullptr)
        return SOAPY_SDR_STREAM_ERROR;

    return call.get()->send(buffs, numElems, flags, timeNs, timeoutUs);
}

int SoapyRPITX::readStreamStatus(SoapySDR::Stream *handle, size_t &chanMask, int &flags, long long &timeNs, const long timeoutUs) {
    SoapySDR::Stream *inner = EnterRxCall(handle);
    if (inner != nullptr) {
        // Sliced like readStream(), so that closeStream() does not wait for a long status poll.
        const auto deadline = SoapyRPITX_DeadlineAfter(timeoutUs);
        int ret;
        try {
            for (;;) {
                long slice = 0;
                if (timeoutUs > 0) {
                    const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now()).count();
                    slice = (long) std::max<long long>(0, std::min<long long>(left, kRxSliceUs));
                }
                ret = rxDev->readStreamStatus(inner, chanMask, flags, timeNs, slice);
                if (ret != SOAPY_SDR_TIMEOUT || slice == 0 || std::chrono::steady_clock::now() >= deadline)
                    break;
                if (!IsValidRxStreamHandle(handle)) {
                    ret = SOAPY_SDR_STREAM_ERROR;
                    break;
                }
            }
        } catch (...) {
            LeaveRxCall();
            throw;
        }
        LeaveRxCall();
        return ret;
    }

    StreamCall call(*this, handle);
    if (call.get() == nullptr)
        return SOAPY_SDR_STREAM_ERROR;

    return call.get()->readStatus(chanMask, flags, timeNs, timeoutUs);
}

// ************************************** TX STREAMER **************************************

tx_streamer::tx_streamer(rpitxStreamFormat _format, const SoapyRPITX_TxConfig &cfg, std::shared_ptr<SoapyRPITX_Ptt> _ptt) :
        format(_format),
        iqsender(new SoapyRPITX_IQ_dmaSync(cfg)),
        scratch(kScratchSize),
        ptt(std::move(_ptt)) {
    supervisor = std::thread(&tx_streamer::Supervise, this);
}

tx_streamer::~tx_streamer() {
    iqsender->Stop();
    ptt->Release();
    supervising.store(false);
    if (supervisor.joinable())
        supervisor.join();
}

void tx_streamer::Supervise() {
    // A stopped transmitter counts as drained: its carrier is off. The write sequence is read before the drain
    // state, so a write that begins in between keeps the transmitter keyed.
    while (supervising.load()) {
        const uint64_t seq = ptt->WriteSeq();
        const bool drained = !iqsender->IsActive() || iqsender->IsDrained();
        ptt->Poll(seq, drained);
        iqsender->FlushLog();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

SoapyRPITX_IQ_dmaSync &tx_streamer::transmitter() {
    return *iqsender;
}

void tx_streamer::interrupt() {
    closing.store(true);
    iqsender->Stop();
    ptt->Release();
}

size_t tx_streamer::mtu() const {
    return std::min<size_t>(kScratchSize, std::max<size_t>(iqsender->FifoCapacity() / 4, 256));
}

int tx_streamer::activate() {
    // Activating an active stream changes nothing (the event counters keep running).
    if (iqsender->IsActive())
        return 0;
    endBurstPending.store(false);
    reportedEvents.store(0);
    iqsender->Start();
    return 0;
}

long tx_streamer::drainTimeoutUs() {
    if (!iqsender->IsActive() || !endBurstPending.load())
        return 0;
    // Let the last burst reach the antenna: the FIFO (IQ samples), the ring and, after a retune, one more ring
    // of replayed slots (slots run at the slot rate), plus a margin.
    SoapyRPITX_TuneConfig t = iqsender->GetTune();
    const double sampleRate = (double) t.sampleRate;
    const double slotRate = sampleRate * (double) iqsender->SlotsPerSample();
    double seconds = (double) iqsender->FifoCapacity() / sampleRate + 2.0 * (double) iqsender->RingSize() / slotRate + 0.2;
    return (long) (seconds * 1e6);
}

int tx_streamer::deactivate() {
    iqsender->Stop();
    ptt->Release();
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

    // Keying: the first write of a transmission waits for the T/R sequence (relay settled in the TX position, PTT
    // line asserted and its delay elapsed) before any of its samples can reach the DMA.
    long waitUs = ptt->BeginWrite();
    struct WriteScope {
        SoapyRPITX_Ptt &p;
        ~WriteScope() {
            p.EndWrite();
        }
    } scope{*ptt};
    // The sequence advances in slices, so that closing or stopping the stream ends the wait at once.
    while (waitUs > 0) {
        if (closing.load() || !iqsender->IsActive())
            return SOAPY_SDR_STREAM_ERROR;
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(std::chrono::microseconds(waitUs), kRelaySlice));
        waitUs = ptt->ReadyInUs();
    }
    if (closing.load() || !iqsender->IsActive())
        return SOAPY_SDR_STREAM_ERROR;

    const auto deadline = SoapyRPITX_DeadlineAfter(timeoutUs);
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
            remainingUs = left > 0 ? (long) std::min<long long>((long long) left, (long long) LONG_MAX) : 0;
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
    const auto deadline = SoapyRPITX_DeadlineAfter(timeoutUs);

    for (;;) {
        if (closing.load())
            return SOAPY_SDR_STREAM_ERROR;
        SoapyRPITX_TxStats stats = iqsender->GetStats();
        uint64_t events = stats.underflows + stats.late + stats.dmaErrors;
        // Each new event is reported to one caller only: the counter of reported events is advanced atomically.
        uint64_t reported = reportedEvents.load();
        bool report = false;
        for (;;) {
            const uint64_t base = events < reported ? 0 : reported;
            if (events <= base)
                break;
            if (reportedEvents.compare_exchange_weak(reported, events)) {
                report = true;
                break;
            }
        }
        if (report) {
            chanMask = 1;
            flags = 0;
            return SOAPY_SDR_UNDERFLOW;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return SOAPY_SDR_TIMEOUT;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
