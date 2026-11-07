/**
 * @file SoapyRPITX.hpp
 * @brief SoapySDR device driver for the librpitx GPIO transmitter (one TX channel), optionally combined with a
 *        SoapySDR receiver (RTL-SDR) as RX channel and an antenna relay GPIO into a half-duplex transceiver.
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

#include <array>
#include <atomic>
#include <complex>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Types.hpp>

#include "SoapyRPITX_IQ_dmaSync.hpp"
#include "SoapyRPITX_Profile.hpp"
#include "SoapyRPITX_Ptt.hpp"

/** @brief Name of the only antenna (the RF output pin). */
#define RPITX_ANTENNA_NAME "GPIO4"
/** @brief Name of the only gain element (pad drive strength). */
#define RPITX_GAIN_NAME "PAD"
/** @brief Device argument selecting the SoapySDR driver of the receiver ("rtlsdr"; "none" or empty: TX only). */
#define RPITX_RX_ARG "rx"
/** @brief Prefix of the device arguments and settings forwarded to the receiver (for example "rx.serial"). */
#define RPITX_RX_PREFIX "rx."

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
     * @brief Creates the stream, the transmitter hardware object and the keying supervisor thread.
     * @param format Host sample format.
     * @param cfg    Transmitter configuration.
     * @param ptt    Keyed state and antenna relay of the device, shared with the receive path.
     * @throws std::runtime_error when the hardware cannot be initialised.
     */
    tx_streamer(rpitxStreamFormat format, const SoapyRPITX_TxConfig &cfg, std::shared_ptr<SoapyRPITX_Ptt> ptt);

    /** @brief Stops transmission, unkeys and releases the hardware. */
    ~tx_streamer();

    tx_streamer(const tx_streamer &) = delete;
    tx_streamer &operator=(const tx_streamer &) = delete;

    /**
     * @brief Queues samples for transmission.
     *
     * Blocks while the FIFO is full, up to @p timeoutUs. Partial writes are reported through the return value.
     * The first write after an idle period keys the transmitter; with an antenna relay it first waits for the
     * relay delay (relay_tx_delay_ms), which is not counted in @p timeoutUs and ends at once when the stream is
     * deactivated or closed (the call then returns SOAPY_SDR_STREAM_ERROR).
     *
     * @param buffs     One buffer (channel 0) in the stream format.
     * @param numElems  Number of complex samples in the buffer.
     * @param flags     SOAPY_SDR_END_BURST marks the end of a burst; SOAPY_SDR_HAS_TIME is not supported.
     * @param timeNs    Ignored.
     * @param timeoutUs Maximum blocking time in microseconds.
     * @return Number of samples accepted, SOAPY_SDR_TIMEOUT when none could be queued in time,
     *         SOAPY_SDR_STREAM_ERROR when the stream is not active (or stopped during the relay delay), or
     *         SOAPY_SDR_NOT_SUPPORTED for timed writes.
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
     * @brief Stops the transmitter (samples still queued are discarded) and unkeys.
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
     * @brief Starts closing: stops the transmitter, unkeys and makes send() and readStatus() running in other
     *        threads return.
     *
     * Thread-safe; after this call readStatus() returns SOAPY_SDR_STREAM_ERROR.
     */
    void interrupt();

private:
    /**
     * @brief Supervisor thread: unkeys the transmitter once it has been drained for the relay hang time, and prints
     *        the messages queued by the feeder thread.
     */
    void Supervise();

    rpitxStreamFormat format;                        ///< Host sample format.
    std::unique_ptr<SoapyRPITX_IQ_dmaSync> iqsender; ///< Transmitter.
    std::vector<std::complex<float>> scratch;        ///< Conversion buffer.
    std::atomic<bool> endBurstPending{false};        ///< The last write carried SOAPY_SDR_END_BURST.
    bool timeWarningShown = false;                   ///< SOAPY_SDR_HAS_TIME warning already logged (producer thread only).
    std::atomic<uint64_t> reportedEvents{0};         ///< Underflow, overrun and DMA error events already reported.
    std::atomic<bool> closing{false};                ///< interrupt() was called: the stream is being closed.
    std::shared_ptr<SoapyRPITX_Ptt> ptt;             ///< Keyed state and antenna relay.
    std::atomic<bool> supervising{true};             ///< The supervisor thread keeps running.
    std::thread supervisor;                          ///< Keying supervisor thread.
};

/**
 * @brief SoapySDR device for the Raspberry Pi GPIO transmitter, optionally a half-duplex transceiver.
 *
 * Device arguments (also accepted as settings, see getSettingInfo()): ppm, ppm_ntp, harmonic, amplitude_mode,
 * iq_scale, debug_level, the amplitude calibration keys (amplitude_lut, ampm_lut, amplitude_quantizer,
 * sigma_delta_order, envelope_delay, envelope_floor, rfdac_lut, eer_lut, eer_ampm_lut, eer_gain, eer_offset,
 * eer_idle, eer_drive) and the transmit/receive switching keys (relay_gpio, relay_active_low, relay_tx_delay_ms,
 * relay_hang_ms, rx_mute_tx, rx_mute_tail_ms). Other device arguments are reported with a warning, except the identification keys
 * produced by enumeration or by SoapySDR (driver, label, product, serial, manufacturer, tuner, type), keys
 * containing ':', and the receiver keys below.
 * Stream arguments: see getStreamArgsInfo().
 *
 * Receiver: the device argument "rx=<driver>" (for example "rx=rtlsdr") opens a SoapySDR receiver through its
 * installed module and exposes its channel 0 as RX channel 0 of this device. Device arguments "rx.<key>=<value>"
 * are passed to the receiver as "<key>=<value>" (for example "rx.serial=00000001"); its settings are available as
 * "rx.<key>". Every RX call (streams, frequency, gain, antennas, sample rate, bandwidth, corrections, sensors,
 * channel settings) is forwarded to it, and the device then reports one RX and one TX channel and full duplex
 * streaming (both streams may run at the same time). While the transmitter is keyed, and for rx_mute_tail_ms after
 * it was unkeyed, the receive samples are replaced by silence (rx_mute_tx). Enumeration lists the TX-only device first and, when the "rx" argument is not
 * given, one transceiver entry for every RTL-SDR found.
 *
 * Antenna relay: relay_gpio selects a GPIO that switches an external T/R relay: it is driven to the TX position
 * before the first sample of a transmission (which waits relay_tx_delay_ms), and back to the RX position once the
 * transmission has been played and relay_hang_ms has elapsed, when the stream is deactivated or closed, and when
 * the device is closed.
 */
class SoapyRPITX : public SoapySDR::Device {
public:
    /**
     * @brief Opens the device.
     *
     * The antenna relay, when configured, is driven to the RX position first. With the argument "profile=<file>"
     * the profile is loaded next (see LoadProfile()); setting keys given as device arguments are then applied with
     * writeSetting() and take precedence over the same keys of the profile. The receiver selected by "rx" is
     * opened last.
     *
     * @param args Device arguments; known setting keys are applied with writeSetting().
     * @throws std::runtime_error when the board is not supported, /dev/mem is not accessible, the profile cannot
     *         be read or holds an invalid value, a relay setting is invalid, or the receiver cannot be opened
     *         (driver not installed, "rpitx" itself, no RX channel, or refused by its module).
     */
    SoapyRPITX(const SoapySDR::Kwargs &args);

    /** @brief Stops and closes the streams, if any, closes the receiver, returns the relay to RX and releases the hardware. */
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
     * @return Driver version, board model, serial, origin, antenna relay GPIO; with a receiver also rx_driver,
     *         rx_hardware and its own hardware information with the "rx." prefix.
     */
    SoapySDR::Kwargs getHardwareInfo(void) const;

    /*******************************************************************
     * Channels API
     ******************************************************************/

    /**
     * @brief Returns the number of channels.
     * @param direction SOAPY_SDR_TX or SOAPY_SDR_RX.
     * @return 1 for TX; 1 for RX with a receiver, otherwise 0.
     */
    size_t getNumChannels(const int direction) const;

    /**
     * @brief Tells whether TX and RX streams can run at the same time.
     *
     * With a receiver both streams run concurrently; the antenna is shared through the relay, and the receive
     * samples are muted while the transmitter is keyed unless rx_mute_tx is false.
     *
     * @param direction Direction.
     * @param channel   Channel.
     * @return True with a receiver, false without one (TX only).
     */
    bool getFullDuplex(const int direction, const size_t channel) const;

    /*******************************************************************
     * Stream API
     ******************************************************************/

    /**
     * @brief Lists the supported host formats.
     * @param direction Direction.
     * @param channel   Channel.
     * @return CF32, CS16 and CS8 for TX; the formats of the receiver for RX (empty without one).
     */
    std::vector<std::string> getStreamFormats(const int direction, const size_t channel) const;

    /**
     * @brief Returns the native format.
     * @param direction Direction.
     * @param channel   Channel.
     * @param fullScale Receives 1.0 for TX, the full scale of the receiver for RX.
     * @return CF32 for TX, the native format of the receiver for RX.
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
     * @return Argument descriptions; for RX the stream arguments of the receiver (empty without one).
     */
    SoapySDR::ArgInfoList getStreamArgsInfo(const int direction, const size_t channel) const;

    /**
     * @brief Creates the TX stream, or the RX stream of the receiver.
     *
     * One stream per direction can be open; the RX stream is created on channel 0 of the receiver with the
     * format and arguments given here.
     *
     * @param direction SOAPY_SDR_TX, or SOAPY_SDR_RX with a receiver.
     * @param format    CF32, CS16 or CS8 for TX; a format of the receiver for RX.
     * @param channels  Empty or {0}.
     * @param args      Stream arguments (see getStreamArgsInfo()).
     * @return Stream handle.
     * @throws std::runtime_error for RX without a receiver, an unsupported format or channel, invalid arguments, a
     *         stream already open in that direction, a TX stream argument that needs the antenna relay GPIO, or a
     *         hardware or receiver initialisation failure.
     */
    SoapySDR::Stream *setupStream(const int direction, const std::string &format, const std::vector<size_t> &channels = std::vector<size_t>(),
                                  const SoapySDR::Kwargs &args = SoapySDR::Kwargs());

    /**
     * @brief Stops and destroys a stream.
     *
     * TX: the carrier is turned off immediately (queued samples are discarded), also while another thread waits in
     * deactivateStream() for the end of a burst, and the relay returns to RX. A writeStream() or readStreamStatus()
     * running in another thread is woken up and returns (SOAPY_SDR_STREAM_ERROR / partial count) before the stream
     * is destroyed; closeStream() waits for it.
     * RX: the receiver stream is deactivated and closed after the readStream() / readStreamStatus() calls in
     * flight have returned.
     *
     * @param stream Stream handle.
     */
    void closeStream(SoapySDR::Stream *stream);

    /**
     * @brief Returns the preferred number of samples per writeStream() / readStream() call.
     * @param stream Stream handle.
     * @return MTU in samples (the MTU of the receiver for the RX stream), 0 for an invalid handle.
     */
    size_t getStreamMTU(SoapySDR::Stream *stream) const;

    /**
     * @brief Starts transmission (activating an active TX stream has no effect), or starts the receiver stream.
     * @param stream   Stream handle.
     * @param flags    TX: SOAPY_SDR_HAS_TIME is not supported. RX: passed to the receiver.
     * @param timeNs   TX: ignored. RX: passed to the receiver.
     * @param numElems TX: ignored. RX: passed to the receiver.
     * @return 0, SOAPY_SDR_NOT_SUPPORTED for timed TX activation, SOAPY_SDR_STREAM_ERROR for an invalid handle,
     *         or the result of the receiver.
     */
    int activateStream(SoapySDR::Stream *stream, const int flags = 0, const long long timeNs = 0, const size_t numElems = 0);

    /**
     * @brief Stops transmission, turns the carrier off and returns the relay to RX, or stops the receiver stream.
     * @param stream Stream handle.
     * @param flags  TX: ignored. RX: passed to the receiver.
     * @param timeNs TX: ignored. RX: passed to the receiver.
     * @return 0, SOAPY_SDR_STREAM_ERROR for an invalid handle, or the result of the receiver.
     */
    int deactivateStream(SoapySDR::Stream *stream, const int flags = 0, const long long timeNs = 0);

    /**
     * @brief Reads samples from the receiver stream.
     *
     * Not serialised with the control API. While the transmitter is keyed and rx_mute_tx is true, and for
     * rx_mute_tail_ms after the unkeying, the samples returned are replaced by silence (zero, or mid-scale for
     * unsigned formats, packed CU4 and CU12 included); flags and time are kept. The receiver is called with
     * timeouts of at most 100 ms until @p timeoutUs has elapsed, so closeStream() never waits longer than one such
     * slice for a read in flight.
     *
     * @param stream    RX stream handle.
     * @param buffs     One buffer (channel 0) in the stream format.
     * @param numElems  Capacity of the buffer in samples.
     * @param flags     Receives the flags of the receiver.
     * @param timeNs    Receives the time of the receiver.
     * @param timeoutUs Maximum blocking time in microseconds.
     * @return Number of samples read or a SOAPY_SDR_* error code (SOAPY_SDR_STREAM_ERROR for a handle that is not
     *         the open RX stream).
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
     * @brief Reports underflow events of the TX stream (see tx_streamer::readStatus()), or the status of the
     *        receiver stream.
     * @param stream    Stream handle.
     * @param chanMask  Receives the channel mask.
     * @param flags     Receives the flags.
     * @param timeNs    Unchanged.
     * @param timeoutUs Maximum waiting time in microseconds.
     * @return TX: SOAPY_SDR_UNDERFLOW, SOAPY_SDR_TIMEOUT or SOAPY_SDR_STREAM_ERROR. RX: the result of the receiver,
     *         or SOAPY_SDR_STREAM_ERROR for an invalid handle.
     */
    int readStreamStatus(SoapySDR::Stream *stream, size_t &chanMask, int &flags, long long &timeNs, const long timeoutUs);

    /*******************************************************************
     * Sensor API
     ******************************************************************/

    /**
     * @brief Lists the global sensors: pll_locked, dma_running, fifo_fill, underflows, late, dma_errors,
     *        sample_rate, envelope_clip, envelope_late, ptt, ptt_count.
     * @return Sensor keys.
     */
    std::vector<std::string> listSensors(void) const;

    /**
     * @brief Describes a global sensor.
     * @param key Sensor key.
     * @return Sensor description.
     * @throws std::runtime_error for an unknown key.
     */
    SoapySDR::ArgInfo getSensorInfo(const std::string &key) const;

    /**
     * @brief Reads a global sensor.
     * @param key Sensor key.
     * @return Value as a string (the transmitter sensors read "false"/"0" while no TX stream exists; ptt and
     *         ptt_count are always available).
     * @throws std::runtime_error for an unknown key.
     */
    std::string readSensor(const std::string &key) const;

    /**
     * @brief Lists the channel sensors.
     * @param direction Direction.
     * @param channel   Channel.
     * @return The channel sensors of the receiver for RX; empty for TX.
     */
    std::vector<std::string> listSensors(const int direction, const size_t channel) const;

    /**
     * @brief Describes a channel sensor.
     * @param direction Must be SOAPY_SDR_RX with a receiver.
     * @param channel   Channel.
     * @param key       Sensor key.
     * @return Sensor description of the receiver.
     * @throws std::runtime_error for TX or without a receiver.
     */
    SoapySDR::ArgInfo getSensorInfo(const int direction, const size_t channel, const std::string &key) const;

    /**
     * @brief Reads a channel sensor.
     * @param direction Must be SOAPY_SDR_RX with a receiver.
     * @param channel   Channel.
     * @param key       Sensor key.
     * @return Value read by the receiver.
     * @throws std::runtime_error for TX or without a receiver.
     */
    std::string readSensor(const int direction, const size_t channel, const std::string &key) const;

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
     * - relay_gpio: GPIO of the antenna relay (-1: none); relay_active_low: TX position with the pin low;
     *   relay_tx_delay_ms: relay settling time before the first sample; relay_hang_ms: time the transmitter
     *   stays keyed after the last sample.
     * - rx_mute_tx: mute the receive samples while the transmitter is keyed; rx_mute_tail_ms: time they stay muted
     *   after the unkeying (receiver buffering latency).
     * - rx.<key>: the settings of the receiver, when one is attached.
     *
     * @return Setting descriptions.
     */
    SoapySDR::ArgInfoList getSettingInfo(void) const;

    /**
     * @brief Changes a setting.
     *
     * The key "profile" loads a profile file (see LoadProfile()): its settings and gain are applied at once, its
     * stream arguments become the defaults of the next setupStream(); an empty value forgets the stream defaults.
     * Keys starting with "rx." are written to the receiver without the prefix.
     *
     * @param key   Setting key.
     * @param value New value.
     * @throws std::runtime_error for an unknown key, an invalid value, a frequency that cannot be produced, a
     *         relay_gpio / relay_active_low change while a TX stream is open, a profile that cannot be read or
     *         holds an invalid value, or a receiver setting refused by the receiver.
     */
    void writeSetting(const std::string &key, const std::string &value);

    /**
     * @brief Reads a setting.
     * @param key Setting key ("rx.<key>" reads a setting of the receiver).
     * @return Value as a string.
     * @throws std::runtime_error for an unknown key.
     */
    std::string readSetting(const std::string &key) const;

    /**
     * @brief Describes the channel settings.
     * @param direction Direction.
     * @param channel   Channel.
     * @return The channel settings of the receiver for RX; empty for TX.
     */
    SoapySDR::ArgInfoList getSettingInfo(const int direction, const size_t channel) const;

    /**
     * @brief Changes a channel setting of the receiver.
     * @param direction Must be SOAPY_SDR_RX with a receiver.
     * @param channel   Channel.
     * @param key       Setting key.
     * @param value     New value.
     * @throws std::runtime_error for TX or without a receiver.
     */
    void writeSetting(const int direction, const size_t channel, const std::string &key, const std::string &value);

    /**
     * @brief Reads a channel setting of the receiver.
     * @param direction Must be SOAPY_SDR_RX with a receiver.
     * @param channel   Channel.
     * @param key       Setting key.
     * @return Value read by the receiver.
     * @throws std::runtime_error for TX or without a receiver.
     */
    std::string readSetting(const int direction, const size_t channel, const std::string &key) const;

    /*******************************************************************
     * Antenna API
     ******************************************************************/

    /**
     * @brief Lists the antennas.
     * @param direction Direction.
     * @param channel   Channel.
     * @return {"GPIO4"} for TX; the antennas of the receiver for RX (empty without one).
     */
    std::vector<std::string> listAntennas(const int direction, const size_t channel) const;

    /**
     * @brief Selects the antenna.
     * @param direction Direction.
     * @param channel   Channel.
     * @param name      "GPIO4" for TX; an antenna of the receiver for RX.
     * @throws std::runtime_error for any other TX name, for RX without a receiver, or when the receiver refuses it.
     */
    void setAntenna(const int direction, const size_t channel, const std::string &name);

    /**
     * @brief Returns the antenna.
     * @param direction Direction.
     * @param channel   Channel.
     * @return "GPIO4" for TX; the antenna of the receiver for RX (empty without one).
     */
    std::string getAntenna(const int direction, const size_t channel) const;

    /*******************************************************************
     * Frontend corrections API
     ******************************************************************/

    /**
     * @brief Tells whether automatic DC offset correction is available.
     * @param direction Direction.
     * @param channel   Channel.
     * @return False for TX; the answer of the receiver for RX.
     */
    bool hasDCOffsetMode(const int direction, const size_t channel) const;

    /**
     * @brief Enables or disables automatic DC offset correction of the receiver.
     * @param direction Direction (TX: no effect).
     * @param channel   Channel.
     * @param automatic Requested mode.
     */
    void setDCOffsetMode(const int direction, const size_t channel, const bool automatic);

    /**
     * @brief Returns the automatic DC offset correction mode.
     * @param direction Direction.
     * @param channel   Channel.
     * @return False for TX; the mode of the receiver for RX.
     */
    bool getDCOffsetMode(const int direction, const size_t channel) const;

    /**
     * @brief Tells whether a manual DC offset correction is available.
     * @param direction Direction.
     * @param channel   Channel.
     * @return False for TX; the answer of the receiver for RX.
     */
    bool hasDCOffset(const int direction, const size_t channel) const;

    /**
     * @brief Sets the manual DC offset correction of the receiver.
     * @param direction Direction (TX: no effect).
     * @param channel   Channel.
     * @param offset    Correction.
     */
    void setDCOffset(const int direction, const size_t channel, const std::complex<double> &offset);

    /**
     * @brief Returns the manual DC offset correction.
     * @param direction Direction.
     * @param channel   Channel.
     * @return 0 for TX; the correction of the receiver for RX.
     */
    std::complex<double> getDCOffset(const int direction, const size_t channel) const;

    /**
     * @brief Tells whether an IQ balance correction is available.
     * @param direction Direction.
     * @param channel   Channel.
     * @return False for TX; the answer of the receiver for RX.
     */
    bool hasIQBalance(const int direction, const size_t channel) const;

    /**
     * @brief Sets the IQ balance correction of the receiver.
     * @param direction Direction (TX: no effect).
     * @param channel   Channel.
     * @param balance   Correction.
     */
    void setIQBalance(const int direction, const size_t channel, const std::complex<double> &balance);

    /**
     * @brief Returns the IQ balance correction.
     * @param direction Direction.
     * @param channel   Channel.
     * @return 0 for TX; the correction of the receiver for RX.
     */
    std::complex<double> getIQBalance(const int direction, const size_t channel) const;

    /**
     * @brief Tells whether a frequency correction is available.
     * @param direction Direction.
     * @param channel   Channel.
     * @return True for TX; the answer of the receiver for RX (false without one).
     */
    bool hasFrequencyCorrection(const int direction, const size_t channel) const;

    /**
     * @brief Sets the crystal correction (TX: same as the ppm setting; RX: correction of the receiver).
     * @param direction Direction.
     * @param channel   Channel.
     * @param value     Correction in ppm; for TX -200 to 200 (positive when the crystal runs slow, i.e. when the
     *                  board transmits below the requested frequency).
     * @throws std::runtime_error for RX without a receiver, a TX value out of range, or a stream that rejects it.
     */
    void setFrequencyCorrection(const int direction, const size_t channel, const double value);

    /**
     * @brief Returns the crystal correction.
     * @param direction Direction.
     * @param channel   Channel.
     * @return Correction in ppm (for RX the correction of the receiver, 0 without one).
     */
    double getFrequencyCorrection(const int direction, const size_t channel) const;

    /*******************************************************************
     * Gain API
     ******************************************************************/

    /**
     * @brief Lists the gain elements.
     * @param direction Direction.
     * @param channel   Channel.
     * @return {"PAD"} for TX (pad drive strength 0-7, 2 mA to 16 mA); the gain elements of the receiver for RX.
     */
    std::vector<std::string> listGains(const int direction, const size_t channel) const;

    /**
     * @brief Tells whether automatic gain is available.
     * @param direction Direction.
     * @param channel   Channel.
     * @return False for TX; the answer of the receiver for RX.
     */
    bool hasGainMode(const int direction, const size_t channel) const;

    /**
     * @brief Sets the automatic gain mode; for TX a request to enable it is ignored with a warning.
     * @param direction Direction.
     * @param channel   Channel.
     * @param automatic Requested mode.
     */
    void setGainMode(const int direction, const size_t channel, const bool automatic);

    /**
     * @brief Returns the automatic gain state.
     * @param direction Direction.
     * @param channel   Channel.
     * @return False for TX; the mode of the receiver for RX.
     */
    bool getGainMode(const int direction, const size_t channel) const;

    /**
     * @brief Sets the overall gain (TX: pad drive level 0-7, rounded and clamped; RX: overall gain of the receiver).
     * @param direction Direction.
     * @param channel   Channel.
     * @param value     Gain.
     */
    void setGain(const int direction, const size_t channel, const double value);

    /**
     * @brief Sets the gain of an element.
     * @param direction Direction.
     * @param channel   Channel.
     * @param name      "PAD" for TX; an element of the receiver for RX.
     * @param value     Gain.
     * @throws std::runtime_error for an unknown TX element.
     */
    void setGain(const int direction, const size_t channel, const std::string &name, const double value);

    /**
     * @brief Returns the overall gain.
     * @param direction Direction.
     * @param channel   Channel.
     * @return Pad drive level for TX; the overall gain of the receiver for RX (0 without one).
     */
    double getGain(const int direction, const size_t channel) const;

    /**
     * @brief Returns the gain of an element.
     * @param direction Direction.
     * @param channel   Channel.
     * @param name      Element name.
     * @return Pad drive level for TX; the element gain of the receiver for RX (0 without one).
     */
    double getGain(const int direction, const size_t channel, const std::string &name) const;

    /**
     * @brief Returns the overall gain range.
     * @param direction Direction.
     * @param channel   Channel.
     * @return 0 to 7, step 1, for TX; the overall range of the receiver for RX.
     */
    SoapySDR::Range getGainRange(const int direction, const size_t channel) const;

    /**
     * @brief Returns the gain range of an element.
     * @param direction Direction.
     * @param channel   Channel.
     * @param name      Element name.
     * @return 0 to 7, step 1, for TX; the element range of the receiver for RX.
     */
    SoapySDR::Range getGainRange(const int direction, const size_t channel, const std::string &name) const;

    /*******************************************************************
     * Frequency API
     ******************************************************************/

    /**
     * @brief Tunes the channel as a whole.
     *
     * TX: sets the "RF" element (see the element overload). RX: forwarded to the receiver, which distributes the
     * frequency over its own elements.
     *
     * @param direction Direction.
     * @param channel   Channel.
     * @param frequency Frequency in Hz.
     * @param args      Tuning arguments (RX only).
     * @throws std::runtime_error when the frequency is out of range or cannot be produced, or for RX without a
     *         receiver.
     */
    void setFrequency(const int direction, const size_t channel, const double frequency, const SoapySDR::Kwargs &args = SoapySDR::Kwargs());

    /**
     * @brief Sets the frequency of an element.
     *
     * TX: applied immediately to an existing stream (the DMA restarts from the queued samples). Without a stream
     * the PLL configuration is checked at once for the current sample rate and harmonic.
     *
     * @param direction Direction.
     * @param channel   Channel.
     * @param name      "RF" for TX; an element of the receiver for RX.
     * @param frequency Frequency in Hz.
     * @param args      TX: ignored. RX: passed to the receiver.
     * @throws std::runtime_error when the frequency is out of range or cannot be produced, or for RX without a
     *         receiver.
     */
    void setFrequency(const int direction, const size_t channel, const std::string &name, const double frequency,
                      const SoapySDR::Kwargs &args = SoapySDR::Kwargs());

    /**
     * @brief Returns the frequency of the channel as a whole.
     * @param direction Direction.
     * @param channel   Channel.
     * @return Frequency in Hz (for RX the frequency of the receiver, 0 without one).
     */
    double getFrequency(const int direction, const size_t channel) const;

    /**
     * @brief Returns the frequency of an element.
     * @param direction Direction.
     * @param channel   Channel.
     * @param name      Element name.
     * @return Frequency in Hz (for RX the element frequency of the receiver, 0 without one).
     */
    double getFrequency(const int direction, const size_t channel, const std::string &name) const;

    /**
     * @brief Describes the tuning arguments.
     * @param direction Direction.
     * @param channel   Channel.
     * @return Empty list for TX; the tuning arguments of the receiver for RX.
     */
    SoapySDR::ArgInfoList getFrequencyArgsInfo(const int direction, const size_t channel) const;

    /**
     * @brief Lists the tunable elements.
     * @param direction Direction.
     * @param channel   Channel.
     * @return {"RF"} for TX; the elements of the receiver for RX.
     */
    std::vector<std::string> listFrequencies(const int direction, const size_t channel) const;

    /**
     * @brief Returns the tuning range of the channel as a whole.
     * @param direction Direction.
     * @param channel   Channel.
     * @return TX: range of the board (multiplied by the harmonic setting). RX: range of the receiver.
     */
    SoapySDR::RangeList getFrequencyRange(const int direction, const size_t channel) const;

    /**
     * @brief Returns the tuning range of an element.
     * @param direction Direction.
     * @param channel   Channel.
     * @param name      Element name.
     * @return TX: range of the board (multiplied by the harmonic setting). RX: element range of the receiver.
     */
    SoapySDR::RangeList getFrequencyRange(const int direction, const size_t channel, const std::string &name) const;

    /*******************************************************************
     * Sample Rate API
     ******************************************************************/

    /**
     * @brief Sets the IQ sample rate.
     * @param direction Direction.
     * @param channel   Channel.
     * @param rate      Rate in Hz, within getSampleRateRange().
     * @throws std::runtime_error for an unsupported TX rate, a TX rate at which the PLL cannot produce the current
     *         frequency, a TX rate for which the slot rate of the open stream
     *         exceeds 200 kHz or its DMA ring is shorter than 10 ms, for RX without a receiver, or when the
     *         receiver refuses the rate.
     */
    void setSampleRate(const int direction, const size_t channel, const double rate);

    /**
     * @brief Returns the IQ sample rate.
     * @param direction Direction.
     * @param channel   Channel.
     * @return Nominal rate in Hz (for RX the rate of the receiver, 0 without one); the sample_rate sensor reports
     *         the TX rate produced by the pacing clock.
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
     * @return Ranges in Hz.
     */
    SoapySDR::RangeList getSampleRateRange(const int direction, const size_t channel) const;

    /**
     * @brief Sets the analog bandwidth; for TX the bandwidth always equals the sample rate, so this has no effect.
     * @param direction Direction.
     * @param channel   Channel.
     * @param bw        Requested bandwidth in Hz.
     */
    void setBandwidth(const int direction, const size_t channel, const double bw);

    /**
     * @brief Returns the bandwidth.
     * @param direction Direction.
     * @param channel   Channel.
     * @return TX: sample rate in Hz. RX: bandwidth of the receiver.
     */
    double getBandwidth(const int direction, const size_t channel) const;

    /**
     * @brief Lists bandwidths.
     * @param direction Direction.
     * @param channel   Channel.
     * @return Empty list for TX (not adjustable); the bandwidths of the receiver for RX.
     */
    std::vector<double> listBandwidths(const int direction, const size_t channel) const;

    /**
     * @brief Returns the bandwidth ranges.
     * @param direction Direction.
     * @param channel   Channel.
     * @return Empty list for TX; the bandwidth ranges of the receiver for RX.
     */
    SoapySDR::RangeList getBandwidthRange(const int direction, const size_t channel) const;

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

    /**
     * @brief Checks and applies the transmit/receive switching settings (device_mutex held).
     * @param relay  Antenna relay configuration.
     * @param muteTx     Mute the receive samples while the transmitter is keyed.
     * @param muteTailMs Time the receive samples stay muted after the unkeying, in milliseconds.
     * @param dryRun     Only check, change nothing.
     * @throws std::runtime_error when the relay configuration is invalid, its pin or polarity changes while a TX
     *         stream is open, or the GPIO block cannot be mapped.
     */
    void ApplyPttLocked(const SoapyRPITX_RelayConfig &relay, bool muteTx, uint32_t muteTailMs, bool dryRun);

    /**
     * @brief Checks that the TX stream configuration leaves the antenna relay GPIO free.
     * @param cfg Transmitter configuration of the stream being created.
     * @throws std::runtime_error when the stream would use the relay GPIO.
     */
    void CheckRelayPin(const SoapyRPITX_TxConfig &cfg) const;

    /*******************************************************************
     * Receiver (SoapyRPITX_Receiver.cpp)
     ******************************************************************/

    /**
     * @brief Opens the receiver through the factory of its installed SoapySDR module (constructor only).
     *
     * The receiver gets the arguments "driver=<driver>" and every "rx.<key>=<value>" of @p args as "<key>=<value>".
     *
     * @param driver SoapySDR driver key of the receiver.
     * @param args   Device arguments of this device.
     * @throws std::runtime_error when the driver is "rpitx", not installed, has no RX channel or refuses to open.
     */
    void OpenReceiver(const std::string &driver, const SoapySDR::Kwargs &args);

    /**
     * @brief Tells whether a call for @p direction goes to the receiver.
     * @param direction Direction.
     * @return True for SOAPY_SDR_RX with a receiver.
     */
    bool ForwardRx(const int direction) const;

    /**
     * @brief Returns the receiver.
     * @return Receiver device.
     * @throws std::runtime_error when no receiver is attached.
     */
    SoapySDR::Device &Rx() const;

    /**
     * @brief Creates the RX stream on channel 0 of the receiver.
     * @param format   Stream format.
     * @param channels Empty or {0}.
     * @param args     Stream arguments of the receiver.
     * @return Stream handle (a generation token shared with the TX handles).
     * @throws std::runtime_error without a receiver, for another channel, an RX stream already open, or a
     *         refusal of the receiver.
     */
    SoapySDR::Stream *SetupRxStream(const std::string &format, const std::vector<size_t> &channels, const SoapySDR::Kwargs &args);

    /**
     * @brief Tells whether a handle is the open RX stream.
     * @param handle Stream handle.
     * @return True when valid.
     */
    bool IsValidRxStreamHandle(SoapySDR::Stream *handle) const;

    /**
     * @brief Registers an RX data-path call (readStream() / readStreamStatus()) and validates the handle.
     *
     * Same protocol as EnterStreamCall(); every successful call must be paired with LeaveRxCall().
     *
     * @param handle Stream handle.
     * @return The receiver stream, or nullptr (nothing registered) when the handle is not the open RX stream.
     */
    SoapySDR::Stream *EnterRxCall(SoapySDR::Stream *handle);

    /** @brief Unregisters a call registered by EnterRxCall(). */
    void LeaveRxCall();

    /**
     * @brief Deactivates and closes the open RX stream, if any, after the data-path calls in flight have returned.
     *
     * The caller holds @ref device_mutex.
     */
    void ReleaseRxStreamLocked();

    /**
     * @brief Replaces received samples with silence while the transmitter is keyed (and for rx_mute_tail_ms after the
     *        unkeying) and rx_mute_tx is set.
     * @param buffs    Buffers of the call.
     * @param numElems Number of samples read into each buffer.
     */
    void MuteRx(void *const *buffs, size_t numElems) const;

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

    std::shared_ptr<tx_streamer> tx_stream;        ///< Open TX stream, if any (kept alive by a draining deactivateStream()).
    std::atomic<uintptr_t> streamHandle{0};        ///< Opaque generation token of the open stream (0: none), read by writeStream() without the mutex.
    std::atomic<tx_streamer *> streamPtr{nullptr}; ///< Open stream the token was issued for; valid while a registered call holds the token.
    uintptr_t streamGeneration = 0;                ///< Last token issued; never reused, so a stale handle cannot match a new stream (device_mutex).
    std::atomic<int> streamCalls{0};               ///< writeStream() / readStreamStatus() calls in flight; closing the stream waits for zero.

    std::shared_ptr<SoapyRPITX_Ptt> ptt;                               ///< Keyed state and antenna relay, shared with the TX stream.
    std::atomic<bool> rxMuteTx{true};                                  ///< Mute the receive samples while the transmitter is keyed.
    std::atomic<uint32_t> rxMuteTailMs{RPITX_RX_MUTE_DEFAULT_TAIL_MS}; ///< Time the receive samples stay muted after the unkeying, in milliseconds.

    std::unique_ptr<SoapySDR::Device> rxDev;              ///< Receiver, or nullptr; set by the constructor only.
    std::string rxDriver;                                 ///< SoapySDR driver key of the receiver.
    SoapySDR::Stream *rxStream = nullptr;                 ///< Open stream of the receiver (device_mutex).
    std::atomic<uintptr_t> rxHandle{0};                   ///< Generation token of the open RX stream (0: none).
    std::atomic<SoapySDR::Stream *> rxStreamPtr{nullptr}; ///< Receiver stream the RX token was issued for.
    std::atomic<int> rxCalls{0};                          ///< readStream() / readStreamStatus() calls in flight on the RX stream.
    size_t rxElemBytes = 0;                               ///< Bytes of one complex element of the RX stream format (set before the RX handle is published).
    std::array<uint8_t, 16> rxSilence{};                  ///< Silence of one complex element in the RX stream format.
    bool rxSilenceZero = true;                            ///< The silence of the RX stream format is all zero bytes.
};
