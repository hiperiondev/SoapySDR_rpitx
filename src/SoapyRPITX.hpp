/**
 * @file SoapyRPITX.hpp
 * @brief SoapySDR device driver for the librpitx GPIO transmitter (TX only, one channel).
 *
 * @copyright Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
 *
 * Project Site: https://github.com/hiperiondev/SoapySDR_rpitx
 *
 * This is based on other projects:
 * - librpitx (https://github.com/F5OEO/librpitx), Copyright (C) 2018 Evariste COURJAUD F5OEO
 * - SoapyPlutoSDR (https://github.com/pothosware/SoapyPlutoSDR)
 *
 * please contact their authors for more information.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING. If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#pragma once

#include <atomic>
#include <complex>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Types.hpp>

#include "SoapyRPITX_IQ_dmaSync.hpp"
#include "SoapyRPITX_Profile.hpp"

/** @brief Name of the only antenna (the RF output pin). */
#define RPITX_ANTENNA_NAME "GPIO4"
/** @brief Name of the only gain element (pad drive strength). */
#define RPITX_GAIN_NAME "PAD"

/**
 * @brief Identification of the running board.
 */
struct SoapyRPITX_BoardInfo {
    std::string model;      ///< Device-tree model string (empty when unavailable).
    std::string serial;     ///< Device-tree serial number (empty when unavailable).
    bool supported = false; ///< True on a Raspberry Pi supported by librpitx (BCM2835/6/7, BCM2711).
    bool pi4 = false;       ///< True on BCM2711 (Pi 4, Pi 400, CM4).
    std::string reason;     ///< Why the board is not supported.
};

/**
 * @brief Identifies the running board from the device tree.
 *
 * Raspberry Pi 5 / 500 / CM5 (BCM2712 with RP1) are reported as unsupported: their GPIO, clock and DMA
 * blocks are not compatible with librpitx.
 *
 * @return Board information.
 */
SoapyRPITX_BoardInfo SoapyRPITX_DetectBoard();

/** @brief Memory device used to map the peripherals (overridable at build time for the host-side tests). */
#ifndef RPITX_DEV_MEM
#define RPITX_DEV_MEM "/dev/mem"
#endif

/** @brief VideoCore mailbox device (overridable at build time for the host-side tests). */
#ifndef RPITX_DEV_VCIO
#define RPITX_DEV_VCIO "/dev/vcio"
#endif

/**
 * @brief Tells whether the process can open /dev/mem and the VideoCore mailbox for reading and writing.
 *
 * The check uses the effective user and group IDs, so setuid programs are accepted.
 *
 * @return True when both @ref RPITX_DEV_MEM and @ref RPITX_DEV_VCIO are readable and writable.
 */
bool SoapyRPITX_HasHardwareAccess();

/** @brief Host-side sample formats accepted by writeStream(). */
typedef enum rpitxStreamFormat {
    RPITX_SDR_CF32, ///< Complex float32, full scale 1.0.
    RPITX_SDR_CS16, ///< Complex int16, full scale 32768.
    RPITX_SDR_CS8,  ///< Complex int8, full scale 128.
} rpitxStreamFormat_t;

/**
 * @brief Transmit stream: converts host samples and feeds the transmitter.
 */
class tx_streamer {
public:
    /**
     * @brief Creates the stream and the transmitter hardware object.
     * @param format Host sample format.
     * @param cfg    Transmitter configuration.
     * @throws std::runtime_error when the hardware cannot be initialised.
     */
    tx_streamer(rpitxStreamFormat format, const SoapyRPITX_TxConfig &cfg);

    /** @brief Stops transmission and releases the hardware. */
    ~tx_streamer();

    tx_streamer(const tx_streamer &) = delete;
    tx_streamer &operator=(const tx_streamer &) = delete;

    /**
     * @brief Queues samples for transmission.
     *
     * Blocks while the FIFO is full, up to @p timeoutUs. Partial writes are reported through the return value.
     *
     * @param buffs     One buffer (channel 0) in the stream format.
     * @param numElems  Number of complex samples in the buffer.
     * @param flags     SOAPY_SDR_END_BURST marks the end of a burst; SOAPY_SDR_HAS_TIME is not supported.
     * @param timeNs    Ignored.
     * @param timeoutUs Maximum blocking time in microseconds.
     * @return Number of samples accepted, SOAPY_SDR_TIMEOUT when none could be queued in time,
     *         SOAPY_SDR_STREAM_ERROR when the stream is not active, or SOAPY_SDR_NOT_SUPPORTED for timed writes.
     */
    int send(const void *const *buffs, size_t numElems, int &flags, long long timeNs, long timeoutUs);

    /**
     * @brief Reports asynchronous stream events.
     * @param chanMask  Receives the channel mask of the event (bit 0).
     * @param flags     Receives the event flags.
     * @param timeNs    Unchanged (no timestamps).
     * @param timeoutUs Maximum waiting time in microseconds.
     * @return SOAPY_SDR_UNDERFLOW when the FIFO ran empty during a burst, the DMA overran the feeder or the DMA
     *         engine had to be restarted since the last call, otherwise SOAPY_SDR_TIMEOUT.
     */
    int readStatus(size_t &chanMask, int &flags, long long &timeNs, long timeoutUs);

    /**
     * @brief Starts the transmitter; has no effect when it is already active.
     * @return 0.
     */
    int activate();

    /**
     * @brief Returns how long deactivation should wait for the last burst.
     * @return Duration of the FIFO at the sample rate plus two rings (queued slots and the slots replayed after a
     *         retune) at the slot rate, plus 200 ms, in microseconds, when the last write carried
     *         SOAPY_SDR_END_BURST; otherwise 0.
     */
    long drainTimeoutUs();

    /**
     * @brief Stops the transmitter (samples still queued are discarded).
     * @return 0.
     */
    int deactivate();

    /**
     * @brief Returns the preferred number of samples per writeStream() call.
     * @return MTU in samples.
     */
    size_t mtu() const;

    /**
     * @brief Returns the transmitter.
     * @return Transmitter object owned by the stream.
     */
    SoapyRPITX_IQ_dmaSync &transmitter();

    /**
     * @brief Starts closing: stops the transmitter and makes send() and readStatus() running in other threads return.
     *
     * Thread-safe; after this call readStatus() returns SOAPY_SDR_STREAM_ERROR.
     */
    void interrupt();

private:
    rpitxStreamFormat format;                        ///< Host sample format.
    std::unique_ptr<SoapyRPITX_IQ_dmaSync> iqsender; ///< Transmitter.
    std::vector<std::complex<float>> scratch;        ///< Conversion buffer.
    std::atomic<bool> endBurstPending{false};        ///< The last write carried SOAPY_SDR_END_BURST.
    bool timeWarningShown = false;                   ///< SOAPY_SDR_HAS_TIME warning already logged (producer thread only).
    std::atomic<uint64_t> reportedEvents{0};         ///< Underflow, overrun and DMA error events already reported.
    std::atomic<bool> closing{false};                ///< interrupt() was called: the stream is being closed.
};

/**
 * @brief SoapySDR device for the Raspberry Pi GPIO transmitter.
 *
 * Device arguments (also accepted as settings, see getSettingInfo()): ppm, ppm_ntp, harmonic, amplitude_mode,
 * iq_scale, debug_level, and the amplitude calibration keys (amplitude_lut, ampm_lut, amplitude_quantizer,
 * sigma_delta_order, envelope_delay, envelope_floor, rfdac_lut, eer_lut, eer_ampm_lut, eer_gain, eer_offset,
 * eer_idle, eer_drive). Other device arguments are reported with a warning, except the identification keys
 * produced by enumeration or by SoapySDR (driver, label, product, serial, manufacturer, tuner, type) and keys
 * containing ':'.
 * Stream arguments: see getStreamArgsInfo().
 */
class SoapyRPITX : public SoapySDR::Device {
public:
    /**
     * @brief Opens the device.
     *
     * With the argument "profile=<file>" the profile is loaded first (see LoadProfile()); setting keys given as
     * device arguments are then applied with writeSetting() and take precedence over the same keys of the profile.
     *
     * @param args Device arguments; known setting keys are applied with writeSetting().
     * @throws std::runtime_error when the board is not supported, /dev/mem is not accessible, or the profile
     *         cannot be read or holds an invalid value.
     */
    SoapyRPITX(const SoapySDR::Kwargs &args);

    /** @brief Stops and closes the stream, if any, and releases the hardware. */
    ~SoapyRPITX();

    /*******************************************************************
     * Identification API
     ******************************************************************/

    /**
     * @brief Returns the driver key.
     * @return "rpitx".
     */
    std::string getDriverKey(void) const;

    /**
     * @brief Returns the hardware key.
     * @return Board model string.
     */
    std::string getHardwareKey(void) const;

    /**
     * @brief Returns hardware information.
     * @return Driver version, board model, serial and origin.
     */
    SoapySDR::Kwargs getHardwareInfo(void) const;

    /*******************************************************************
     * Channels API
     ******************************************************************/

    /**
     * @brief Returns the number of channels.
     * @param direction SOAPY_SDR_TX or SOAPY_SDR_RX.
     * @return 1 for TX, 0 for RX.
     */
    size_t getNumChannels(const int direction) const;

    /**
     * @brief Tells whether TX and RX can run at the same time.
     * @param direction Direction.
     * @param channel   Channel.
     * @return False (TX only).
     */
    bool getFullDuplex(const int direction, const size_t channel) const;

    /*******************************************************************
     * Stream API
     ******************************************************************/

    /**
     * @brief Lists the supported host formats.
     * @param direction Direction.
     * @param channel   Channel.
     * @return CF32, CS16 and CS8 for TX; empty for RX.
     */
    std::vector<std::string> getStreamFormats(const int direction, const size_t channel) const;

    /**
     * @brief Returns the native format.
     * @param direction Direction.
     * @param channel   Channel.
     * @param fullScale Receives 1.0.
     * @return CF32.
     */
    std::string getNativeStreamFormat(const int direction, const size_t channel, double &fullScale) const;

    /**
     * @brief Describes the stream arguments.
     *
     * - ring_size: DMA ring size in slots (default: 85 ms at the slot rate, sample rate x slots_per_sample,
     *   rounded up to a multiple of 512 and at least 4096; at least 10 ms of slots). The advertised default is
     *   the one of slots_per_sample = 1.
     * - fifo_ms: software FIFO capacity in milliseconds (follows sample-rate changes).
     * - preroll_ms: signal queued before the DMA starts, in milliseconds.
     * - dma_channel: DMA channel (-1 for the first free channel of the board).
     * - dma_force: reset an explicitly selected DMA channel even when it is active.
     * - rt_priority: SCHED_FIFO priority of the feeder thread (0 = normal scheduling).
     * - cpu: CPU the feeder thread is pinned to (-1 = no affinity).
     * - signal_cleanup: turn the carrier off when the process is killed by a signal with the default action.
     * - slots_per_sample: DMA slots per IQ sample (1-4); the slot rate (sample rate x slots) is at most 200 kHz.
     * - envelope_output: "none" (envelope on the pad drive), "pcm" (16-bit I2S DAC on GPIO18/19/21) or "pwm"
     *   (PWM duty on pwm_gpio) for Envelope Elimination and Restoration.
     * - pwm_gpio: PWM envelope pin, 18 or 12.
     * - pwm_bits: PWM envelope resolution, 6-12 bits.
     * - rf_dac: GPIO20 as a second clock output summed with GPIO4 by an external weighted network.
     *
     * @param direction Direction.
     * @param channel   Channel.
     * @return Argument descriptions (empty for RX).
     */
    SoapySDR::ArgInfoList getStreamArgsInfo(const int direction, const size_t channel) const;

    /**
     * @brief Creates the TX stream.
     * @param direction Must be SOAPY_SDR_TX.
     * @param format    CF32, CS16 or CS8.
     * @param channels  Empty or {0}.
     * @param args      Stream arguments (see getStreamArgsInfo()).
     * @return Stream handle.
     * @throws std::runtime_error for RX, an unsupported format or channel, invalid arguments, an already open
     *         stream, or a hardware initialisation failure.
     */
    SoapySDR::Stream *setupStream(const int direction, const std::string &format, const std::vector<size_t> &channels = std::vector<size_t>(),
                                  const SoapySDR::Kwargs &args = SoapySDR::Kwargs());

    /**
     * @brief Stops and destroys a stream.
     *
     * The carrier is turned off immediately (queued samples are discarded), also while another thread waits in
     * deactivateStream() for the end of a burst. A writeStream() or readStreamStatus() running in another thread
     * is woken up and returns (SOAPY_SDR_STREAM_ERROR / partial count) before the stream is destroyed; closeStream()
     * waits for it.
     *
     * @param stream Stream handle.
     */
    void closeStream(SoapySDR::Stream *stream);

    /**
     * @brief Returns the preferred number of samples per writeStream() call.
     * @param stream Stream handle.
     * @return MTU in samples, 0 for an invalid handle.
     */
    size_t getStreamMTU(SoapySDR::Stream *stream) const;

    /**
     * @brief Starts transmission; activating an active stream has no effect.
     * @param stream   Stream handle.
     * @param flags    SOAPY_SDR_HAS_TIME is not supported.
     * @param timeNs   Ignored.
     * @param numElems Ignored.
     * @return 0, SOAPY_SDR_NOT_SUPPORTED for timed activation, or SOAPY_SDR_STREAM_ERROR for an invalid handle.
     */
    int activateStream(SoapySDR::Stream *stream, const int flags = 0, const long long timeNs = 0, const size_t numElems = 0);

    /**
     * @brief Stops transmission and turns the carrier off.
     * @param stream Stream handle.
     * @param flags  Ignored.
     * @param timeNs Ignored.
     * @return 0, or SOAPY_SDR_STREAM_ERROR for an invalid handle.
     */
    int deactivateStream(SoapySDR::Stream *stream, const int flags = 0, const long long timeNs = 0);

    /**
     * @brief Receiving is not supported.
     * @return SOAPY_SDR_NOT_SUPPORTED.
     */
    int readStream(SoapySDR::Stream *stream, void *const *buffs, const size_t numElems, int &flags, long long &timeNs, const long timeoutUs = 100000);

    /**
     * @brief Queues samples for transmission (see tx_streamer::send()).
     * @param stream    Stream handle.
     * @param buffs     One buffer.
     * @param numElems  Number of samples.
     * @param flags     Burst flags.
     * @param timeNs    Ignored.
     * @param timeoutUs Maximum blocking time in microseconds.
     * @return Number of samples accepted or a SOAPY_SDR_* error code.
     */
    int writeStream(SoapySDR::Stream *stream, const void *const *buffs, const size_t numElems, int &flags, const long long timeNs = 0,
                    const long timeoutUs = 100000);

    /**
     * @brief Reports underflow events (see tx_streamer::readStatus()).
     * @param stream    Stream handle.
     * @param chanMask  Receives the channel mask.
     * @param flags     Receives the flags.
     * @param timeNs    Unchanged.
     * @param timeoutUs Maximum waiting time in microseconds.
     * @return SOAPY_SDR_UNDERFLOW, SOAPY_SDR_TIMEOUT or SOAPY_SDR_STREAM_ERROR.
     */
    int readStreamStatus(SoapySDR::Stream *stream, size_t &chanMask, int &flags, long long &timeNs, const long timeoutUs);

    /*******************************************************************
     * Sensor API
     ******************************************************************/

    /**
     * @brief Lists the sensors: pll_locked, dma_running, fifo_fill, underflows, late, dma_errors, sample_rate,
     *        envelope_clip, envelope_late.
     * @return Sensor keys.
     */
    std::vector<std::string> listSensors(void) const;

    /**
     * @brief Describes a sensor.
     * @param key Sensor key.
     * @return Sensor description.
     * @throws std::runtime_error for an unknown key.
     */
    SoapySDR::ArgInfo getSensorInfo(const std::string &key) const;

    /**
     * @brief Reads a sensor.
     * @param key Sensor key.
     * @return Value as a string ("false"/"0" while no stream exists).
     * @throws std::runtime_error for an unknown key.
     */
    std::string readSensor(const std::string &key) const;

    /*******************************************************************
     * Settings API
     ******************************************************************/

    /**
     * @brief Describes the settings.
     *
     * - ppm: manual crystal correction in ppm (positive when the crystal runs slow).
     * - ppm_ntp: add the kernel NTP frequency estimate to the correction.
     * - harmonic: transmit on this harmonic of the PLL output.
     * - amplitude_mode: "envelope" (follow |IQ|) or "constant" (carrier at full drive).
     * - iq_scale: gain applied to |IQ| before amplitude quantisation.
     * - debug_level: librpitx diagnostic level (0-2).
     * - amplitude_lut, ampm_lut: measured amplitude and phase (degrees) of the 8 pad drive levels.
     * - amplitude_quantizer: "nearest" or "sigma_delta"; sigma_delta_order: 1 or 2.
     * - envelope_delay: envelope delay relative to the phase path, in samples; envelope_floor: carrier-off level.
     * - rfdac_lut: amplitude with GPIO4 only, GPIO20 only and both (rf_dac stream argument).
     * - eer_lut, eer_ampm_lut, eer_gain, eer_offset, eer_idle, eer_drive: envelope DAC calibration (envelope_output).
     *
     * @return Setting descriptions.
     */
    SoapySDR::ArgInfoList getSettingInfo(void) const;

    /**
     * @brief Changes a setting.
     *
     * The key "profile" loads a profile file (see LoadProfile()): its settings and gain are applied at once, its
     * stream arguments become the defaults of the next setupStream(); an empty value forgets the stream defaults.
     *
     * @param key   Setting key.
     * @param value New value.
     * @throws std::runtime_error for an unknown key, an invalid value, a frequency that cannot be produced, or a
     *         profile that cannot be read or holds an invalid value.
     */
    void writeSetting(const std::string &key, const std::string &value);

    /**
     * @brief Reads a setting.
     * @param key Setting key.
     * @return Value as a string.
     * @throws std::runtime_error for an unknown key.
     */
    std::string readSetting(const std::string &key) const;

    /*******************************************************************
     * Antenna API
     ******************************************************************/

    /**
     * @brief Lists the antennas.
     * @param direction Direction.
     * @param channel   Channel.
     * @return {"GPIO4"} for TX, empty for RX.
     */
    std::vector<std::string> listAntennas(const int direction, const size_t channel) const;

    /**
     * @brief Selects the antenna.
     * @param direction Direction.
     * @param channel   Channel.
     * @param name      Must be "GPIO4".
     * @throws std::runtime_error for any other name or for RX.
     */
    void setAntenna(const int direction, const size_t channel, const std::string &name);

    /**
     * @brief Returns the antenna.
     * @param direction Direction.
     * @param channel   Channel.
     * @return "GPIO4" for TX, empty for RX.
     */
    std::string getAntenna(const int direction, const size_t channel) const;

    /*******************************************************************
     * Frontend corrections API
     ******************************************************************/

    /**
     * @brief Tells whether automatic DC offset correction is available.
     * @param direction Direction.
     * @param channel   Channel.
     * @return False.
     */
    bool hasDCOffsetMode(const int direction, const size_t channel) const;

    /**
     * @brief Tells whether a frequency correction is available.
     * @param direction Direction.
     * @param channel   Channel.
     * @return True for TX.
     */
    bool hasFrequencyCorrection(const int direction, const size_t channel) const;

    /**
     * @brief Sets the crystal correction (same as the ppm setting).
     * @param direction Must be SOAPY_SDR_TX.
     * @param channel   Channel.
     * @param value     Correction in ppm, -200 to 200 (positive when the crystal runs slow, i.e. when the board
     *                  transmits below the requested frequency).
     * @throws std::runtime_error for RX, a value out of range, or a stream that rejects it.
     */
    void setFrequencyCorrection(const int direction, const size_t channel, const double value);

    /**
     * @brief Returns the crystal correction.
     * @param direction Direction.
     * @param channel   Channel.
     * @return Correction in ppm (0 for RX).
     */
    double getFrequencyCorrection(const int direction, const size_t channel) const;

    /*******************************************************************
     * Gain API
     ******************************************************************/

    /**
     * @brief Lists the gain elements.
     * @param direction Direction.
     * @param channel   Channel.
     * @return {"PAD"} for TX (pad drive strength 0-7, 2 mA to 16 mA).
     */
    std::vector<std::string> listGains(const int direction, const size_t channel) const;

    /**
     * @brief Tells whether automatic gain is available.
     * @param direction Direction.
     * @param channel   Channel.
     * @return False.
     */
    bool hasGainMode(const int direction, const size_t channel) const;

    /**
     * @brief Automatic gain is not available; a request to enable it is ignored with a warning.
     * @param direction Direction.
     * @param channel   Channel.
     * @param automatic Requested mode.
     */
    void setGainMode(const int direction, const size_t channel, const bool automatic);

    /**
     * @brief Returns the automatic gain state.
     * @param direction Direction.
     * @param channel   Channel.
     * @return False.
     */
    bool getGainMode(const int direction, const size_t channel) const;

    /**
     * @brief Sets the overall gain (pad drive level 0-7, rounded and clamped).
     * @param direction Direction.
     * @param channel   Channel.
     * @param value     Gain.
     */
    void setGain(const int direction, const size_t channel, const double value);

    /**
     * @brief Sets the gain of an element.
     * @param direction Direction.
     * @param channel   Channel.
     * @param name      Must be "PAD".
     * @param value     Gain.
     * @throws std::runtime_error for an unknown element.
     */
    void setGain(const int direction, const size_t channel, const std::string &name, const double value);

    /**
     * @brief Returns the overall gain.
     * @param direction Direction.
     * @param channel   Channel.
     * @return Pad drive level.
     */
    double getGain(const int direction, const size_t channel) const;

    /**
     * @brief Returns the gain of an element.
     * @param direction Direction.
     * @param channel   Channel.
     * @param name      Element name.
     * @return Pad drive level.
     */
    double getGain(const int direction, const size_t channel, const std::string &name) const;

    /**
     * @brief Returns the overall gain range.
     * @param direction Direction.
     * @param channel   Channel.
     * @return 0 to 7, step 1.
     */
    SoapySDR::Range getGainRange(const int direction, const size_t channel) const;

    /**
     * @brief Returns the gain range of an element.
     * @param direction Direction.
     * @param channel   Channel.
     * @param name      Element name.
     * @return 0 to 7, step 1.
     */
    SoapySDR::Range getGainRange(const int direction, const size_t channel, const std::string &name) const;

    /*******************************************************************
     * Frequency API
     ******************************************************************/

    /**
     * @brief Sets the output frequency.
     *
     * Applied immediately to an existing stream (the DMA restarts from the queued samples).
     *
     * @param direction Must be SOAPY_SDR_TX.
     * @param channel   Channel.
     * @param name      "RF".
     * @param frequency Frequency in Hz.
     * @param args      Ignored.
     * @throws std::runtime_error when the frequency is out of range or cannot be produced.
     */
    void setFrequency(const int direction, const size_t channel, const std::string &name, const double frequency,
                      const SoapySDR::Kwargs &args = SoapySDR::Kwargs());

    /**
     * @brief Returns the output frequency.
     * @param direction Direction.
     * @param channel   Channel.
     * @param name      Element name.
     * @return Frequency in Hz (0 for RX).
     */
    double getFrequency(const int direction, const size_t channel, const std::string &name) const;

    /**
     * @brief Describes the tuning arguments.
     * @param direction Direction.
     * @param channel   Channel.
     * @return Empty list.
     */
    SoapySDR::ArgInfoList getFrequencyArgsInfo(const int direction, const size_t channel) const;

    /**
     * @brief Lists the tunable elements.
     * @param direction Direction.
     * @param channel   Channel.
     * @return {"RF"} for TX.
     */
    std::vector<std::string> listFrequencies(const int direction, const size_t channel) const;

    /**
     * @brief Returns the tuning range.
     * @param direction Direction.
     * @param channel   Channel.
     * @param name      Element name.
     * @return Range of the board (multiplied by the harmonic setting).
     */
    SoapySDR::RangeList getFrequencyRange(const int direction, const size_t channel, const std::string &name) const;

    /*******************************************************************
     * Sample Rate API
     ******************************************************************/

    /**
     * @brief Sets the IQ sample rate.
     * @param direction Must be SOAPY_SDR_TX.
     * @param channel   Channel.
     * @param rate      Rate in Hz, within getSampleRateRange().
     * @throws std::runtime_error for an unsupported rate, or a rate for which the slot rate of the open stream
     *         exceeds 200 kHz or its DMA ring is shorter than 10 ms.
     */
    void setSampleRate(const int direction, const size_t channel, const double rate);

    /**
     * @brief Returns the IQ sample rate.
     * @param direction Direction.
     * @param channel   Channel.
     * @return Nominal rate in Hz (0 for RX); the sample_rate sensor reports the rate produced by the pacing clock.
     */
    double getSampleRate(const int direction, const size_t channel) const;

    /**
     * @brief Lists common sample rates.
     * @param direction Direction.
     * @param channel   Channel.
     * @return Rates in Hz.
     */
    std::vector<double> listSampleRates(const int direction, const size_t channel) const;

    /**
     * @brief Returns the supported sample-rate range.
     * @param direction Direction.
     * @param channel   Channel.
     * @return Continuous range in Hz.
     */
    SoapySDR::RangeList getSampleRateRange(const int direction, const size_t channel) const;

    /**
     * @brief Sets the analog bandwidth; the bandwidth always equals the sample rate, so this has no effect.
     * @param direction Direction.
     * @param channel   Channel.
     * @param bw        Requested bandwidth in Hz.
     */
    void setBandwidth(const int direction, const size_t channel, const double bw);

    /**
     * @brief Returns the bandwidth.
     * @param direction Direction.
     * @param channel   Channel.
     * @return Sample rate in Hz.
     */
    double getBandwidth(const int direction, const size_t channel) const;

    /**
     * @brief Lists bandwidths.
     * @param direction Direction.
     * @param channel   Channel.
     * @return Empty list (not adjustable).
     */
    std::vector<double> listBandwidths(const int direction, const size_t channel) const;

private:
    /**
     * @brief Tells whether a handle is the open TX stream.
     *
     * Handles are generation tokens, not addresses, so a handle kept after closeStream() never matches a stream
     * opened later (no ABA on a reused allocation).
     *
     * @param handle Stream handle.
     * @return True when valid.
     */
    bool IsValidTxStreamHandle(SoapySDR::Stream *handle) const;

    /**
     * @brief Registers a data-path call (writeStream() / readStreamStatus()) and validates the handle.
     *
     * The call counter is incremented before the handle is checked, so closeStream() (which clears the handle
     * first and then waits for the counter to reach zero) can never free the stream under a registered call.
     * Every successful call must be paired with LeaveStreamCall().
     *
     * @param handle Stream handle.
     * @return The stream, or nullptr (nothing registered) when the handle is not the open TX stream.
     */
    tx_streamer *EnterStreamCall(SoapySDR::Stream *handle);

    /** @brief Unregisters a call registered by EnterStreamCall(). */
    void LeaveStreamCall();

    /**
     * @brief Stops and destroys the open TX stream, if any, after the data-path calls in flight have returned.
     *
     * The caller holds @ref device_mutex.
     */
    void ReleaseTxStreamLocked();

    /** @brief Scope guard of a data-path call. */
    class StreamCall {
    public:
        StreamCall(SoapyRPITX &dev, SoapySDR::Stream *handle) :
                dev_(dev),
                stream_(dev.EnterStreamCall(handle)) {
        }
        ~StreamCall() {
            if (stream_ != nullptr)
                dev_.LeaveStreamCall();
        }
        StreamCall(const StreamCall &) = delete;
        StreamCall &operator=(const StreamCall &) = delete;

        /** @brief The registered stream, nullptr when the handle was not valid. */
        tx_streamer *get() const {
            return stream_;
        }

    private:
        SoapyRPITX &dev_;
        tx_streamer *stream_;
    };

    /**
     * @brief Applies the tune parameters to the open stream, if any.
     * @param tune New parameters.
     * @throws std::runtime_error when the stream rejects them.
     */
    void ApplyTune(const SoapyRPITX_TuneConfig &tune);

    /**
     * @brief Returns the board tuning range of the PLL output (before the harmonic factor).
     * @return Range in Hz.
     */
    SoapySDR::Range PllRange() const;

    /**
     * @brief Validates and applies amplitude parameters to the device and the open stream (device_mutex held).
     * @param amplitude New parameters.
     * @throws std::runtime_error when invalid or refused by the stream.
     */
    void ApplyAmplitude(const SoapyRPITX_AmplitudeConfig &amplitude);

    /**
     * @brief Validates and applies a manual crystal correction (device_mutex held).
     * @param ppm Correction in ppm.
     * @throws std::runtime_error when out of range or rejected by the stream.
     */
    void SetPpmLocked(double ppm);

    /** @brief Settings parsed but not yet applied (defined in SoapyRPITX_Settings.cpp). */
    struct PendingSettings;

    /**
     * @brief Parses one setting into @p pending without touching the hardware or the device state (device_mutex held).
     * @param key     Setting key.
     * @param value   Value.
     * @param pending Receives the parsed value.
     * @throws std::runtime_error for an unknown key or an invalid value.
     */
    void ParseSetting(const std::string &key, const std::string &value, PendingSettings &pending) const;

    /**
     * @brief Checks the combination of parsed settings against the board and the open stream (device_mutex held).
     * @param pending Parsed settings.
     * @throws std::runtime_error when the combination cannot be applied.
     */
    void ValidateSettings(const PendingSettings &pending) const;

    /**
     * @brief Applies validated settings: at most one retune and one amplitude update (device_mutex held).
     *
     * When the retune is refused or times out nothing is changed. If the amplitude update fails after the retune,
     * the previous tune is restored.
     *
     * @param pending Parsed and validated settings.
     * @throws std::runtime_error when the stream refuses the change.
     */
    void ApplySettings(const PendingSettings &pending);

    /**
     * @brief Parses the stream arguments into @p cfg without creating anything (dry run of setupStream()).
     *
     * Fills the fields controlled by stream arguments; @p cfg.tune must hold the tune the stream will use
     * (its sample rate gives the default ring size). Checks that depend on the sample rate of a later
     * setupStream() call are left to it.
     *
     * @param sargs  Stream arguments (profile defaults already merged).
     * @param cfg    Receives the values.
     * @param envOut Receives the envelope_output name.
     * @throws std::runtime_error for an invalid value.
     */
    void ParseStreamArgs(const SoapySDR::Kwargs &sargs, SoapyRPITX_TxConfig &cfg, std::string &envOut) const;

    /**
     * @brief Loads a profile file (device_mutex not held).
     *
     * The load is transactional. Every setting, the gain and the stream arguments of the file (except the keys
     * present in @p explicitArgs) are parsed and validated without touching the hardware; then the result is
     * applied once under the device mutex (one retune, one amplitude update, gain, profile name and stream
     * defaults). The "stream." lines replace the default stream arguments used by setupStream() for the keys the
     * application does not pass. Unknown setting and stream argument keys are reported with a warning and
     * ignored. When anything is refused, nothing is changed and an exception is thrown. An empty @p path forgets
     * the profile and its stream defaults.
     *
     * @param path         Profile file name.
     * @param explicitArgs Device arguments that take precedence over the profile, or nullptr.
     * @throws std::runtime_error when the file cannot be read, a line is malformed or a value is refused.
     */
    void LoadProfile(const std::string &path, const SoapySDR::Kwargs *explicitArgs);

    mutable std::mutex device_mutex;   ///< Serialises the control API.
    SoapyRPITX_TuneConfig tune;        ///< Frequency-related parameters.
    int driveLevel = 7;                ///< Pad drive level 0-7.
    float iqScale = 1.0f;              ///< IQ magnitude gain.
    bool constantEnvelope = false;     ///< Constant-envelope amplitude mode.
    int debugLevel = 0;                ///< librpitx diagnostic level.
    SoapyRPITX_AmplitudeConfig ampCfg; ///< Amplitude calibration and processing.
    std::string model;                 ///< Board model string.
    std::string serial;                ///< Board serial number.
    bool pi4 = false;                  ///< BCM2711 board.
    std::string profilePath;           ///< Profile file loaded last (empty: none).
    SoapySDR::Kwargs profileStream;    ///< Default stream arguments of the loaded profile.

    std::shared_ptr<tx_streamer> tx_stream;           ///< Open TX stream, if any (kept alive by a draining deactivateStream()).
    std::atomic<uintptr_t> streamHandle{0};           ///< Opaque generation token of the open stream (0: none), read by writeStream() without the mutex.
    std::atomic<tx_streamer *> streamPtr{nullptr};    ///< Open stream the token was issued for; valid while a registered call holds the token.
    uintptr_t streamGeneration = 0;                   ///< Last token issued; never reused, so a stale handle cannot match a new stream (device_mutex).
    std::atomic<int> streamCalls{0};                  ///< writeStream() / readStreamStatus() calls in flight; closing the stream waits for zero.
};
