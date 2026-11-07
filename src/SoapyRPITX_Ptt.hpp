/**
 * @file SoapyRPITX_Ptt.hpp
 * @brief Transmit/receive switching: keyed state of the transmitter and the optional antenna relay GPIO.
 *
 * @copyright Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
 *
 * Project Site: https://github.com/hiperiondev/SoapySDR_rpitx
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

#include <stdint.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

class generalgpio;

/** @brief Lowest GPIO accepted as antenna relay output (GPIO0/1 carry the HAT identification EEPROM). */
#define RPITX_RELAY_MIN_GPIO 2
/** @brief Highest GPIO accepted as antenna relay output (last pin of the 40-pin header). */
#define RPITX_RELAY_MAX_GPIO 27
/** @brief Longest accepted relay switching delay, in milliseconds. */
#define RPITX_RELAY_MAX_DELAY_MS 1000
/** @brief Longest accepted relay hang time, in milliseconds. */
#define RPITX_RELAY_MAX_HANG_MS 10000

/**
 * @brief Antenna relay (T/R switch) configuration.
 */
struct SoapyRPITX_RelayConfig {
    int gpio = -1;           ///< BCM GPIO driving the relay, -1 when no relay is connected.
    bool activeLow = false;  ///< True when the relay is in the TX position with the pin low (most relay modules).
    uint32_t txDelayMs = 20; ///< Time given to the relay to settle in the TX position before the first sample is queued.
    uint32_t hangMs = 100;   ///< Time the transmitter stays keyed after the last sample was played.
};

/**
 * @brief Checks a relay configuration.
 * @param cfg Configuration to check.
 * @return Empty string when valid, otherwise the reason it is refused.
 */
std::string SoapyRPITX_CheckRelayConfig(const SoapyRPITX_RelayConfig &cfg);

/**
 * @brief Keyed state of the transmitter and driver of the antenna relay GPIO.
 *
 * The transmitter is keyed by the first sample written after an idle period. When a relay is configured, its
 * GPIO is switched to the TX position first and the write is held back for @ref SoapyRPITX_RelayConfig::txDelayMs,
 * so no RF reaches the relay contacts while they move. The transmitter is unkeyed (and the relay returned to the RX
 * position) once every queued sample has been played and nothing was written for @ref SoapyRPITX_RelayConfig::hangMs,
 * or at once when the transmitter is stopped. The keyed state also tells the receive path when to mute.
 *
 * The relay GPIO is driven with the GPSET/GPCLR registers, which the transmitter DMA never writes. Its function is
 * set to output only by Configure(), which the device calls while no TX stream exists: the transmitter samples
 * the GPFSEL registers when it starts and rewrites them from DMA, so the pin function must not change while it
 * runs. Destroying the object leaves the pin driving the RX position.
 *
 * Thread model: every method is thread-safe. BeginWrite() / EndWrite() are called by the producer around each
 * write, Poll() by the supervisor thread of the TX stream, Release() after the transmitter was stopped.
 */
class SoapyRPITX_Ptt {
public:
    /** @brief Creates an unkeyed state without relay. */
    SoapyRPITX_Ptt();

    /** @brief Unkeys and leaves the relay GPIO, if any, driving the RX position. */
    ~SoapyRPITX_Ptt();

    SoapyRPITX_Ptt(const SoapyRPITX_Ptt &) = delete;
    SoapyRPITX_Ptt &operator=(const SoapyRPITX_Ptt &) = delete;

    /**
     * @brief Applies a relay configuration.
     *
     * A new pin is first driven to the RX position and then switched to output, so it never shows the TX
     * position; a pin that is no longer used is driven to the RX position and switched to input. The GPIO block
     * is mapped on first use. Must not be called while a TX stream exists when the pin or its polarity changes.
     *
     * @param cfg New configuration.
     * @throws std::runtime_error when the configuration is invalid or the GPIO block cannot be mapped (nothing
     *         is changed).
     */
    void Configure(const SoapyRPITX_RelayConfig &cfg);

    /**
     * @brief Returns the relay configuration in effect.
     * @return Configuration.
     */
    SoapyRPITX_RelayConfig GetConfig() const;

    /**
     * @brief Registers a write of samples; keys the transmitter when it is not keyed.
     *
     * Every call must be paired with EndWrite().
     *
     * @return Microseconds the caller must wait before queueing the samples: the relay delay when this call keyed
     *         the transmitter and a relay is configured, otherwise 0.
     */
    long BeginWrite();

    /** @brief Ends a write registered by BeginWrite(); the hang time restarts from now. */
    void EndWrite();

    /**
     * @brief Advances the unkeying timer (supervisor thread, about every millisecond).
     * @param drained True when every queued sample has been played or the transmitter is stopped.
     */
    void Poll(bool drained);

    /** @brief Unkeys at once (the transmitter has been stopped, so the carrier is already off). */
    void Release();

    /**
     * @brief Tells whether the transmitter is keyed (relay, if any, in the TX position).
     * @return True while keyed.
     */
    bool IsKeyed() const;

    /**
     * @brief Returns how many times the transmitter has been keyed.
     * @return Count since the object was created.
     */
    uint64_t KeyCount() const;

private:
    using clock = std::chrono::steady_clock;

    /**
     * @brief Drives the relay pin (mutex held, pin configured).
     * @param tx True for the TX position.
     */
    void DriveLocked(bool tx);

    /**
     * @brief Drives a pin to a level with GPSET/GPCLR.
     * @param pin   BCM GPIO number.
     * @param level Output level.
     */
    void WriteLevel(int pin, bool level);

    mutable std::mutex mutex;          ///< Protects every member below except the atomics.
    SoapyRPITX_RelayConfig cfg;        ///< Configuration in effect.
    std::unique_ptr<generalgpio> gpio; ///< GPIO block, mapped while a relay is configured.
    std::atomic<bool> keyed{false};    ///< Transmitter keyed.
    std::atomic<uint64_t> keyCount{0}; ///< Times the transmitter was keyed.
    int writers = 0;                   ///< Writes registered by BeginWrite() and not yet ended.
    clock::time_point lastBusy;        ///< Last time a write ended or the transmitter was not drained.
};
