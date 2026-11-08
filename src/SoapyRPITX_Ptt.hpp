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

/** @brief Lowest GPIO accepted as antenna relay or PTT output (GPIO0/1 carry the HAT identification EEPROM). */
#define RPITX_RELAY_MIN_GPIO 2
/** @brief Highest GPIO accepted as antenna relay or PTT output (last pin of the 40-pin header). */
#define RPITX_RELAY_MAX_GPIO 27
/** @brief Longest accepted relay switching delay, in milliseconds. */
#define RPITX_RELAY_MAX_DELAY_MS 1000
/** @brief Longest accepted relay hang time, in milliseconds. */
#define RPITX_RELAY_MAX_HANG_MS 10000
/** @brief Longest accepted PTT delay (PTT to first sample, or PTT release to relay release), in milliseconds. */
#define RPITX_PTT_MAX_DELAY_MS 1000
/** @brief Default time from the PTT line asserted to the first sample, in milliseconds. */
#define RPITX_PTT_DEFAULT_TX_DELAY_MS 10
/** @brief Default time from the PTT line released to the relay returning to the RX position, in milliseconds. */
#define RPITX_PTT_DEFAULT_RX_DELAY_MS 10
/** @brief Longest accepted receive mute tail after unkeying, in milliseconds. */
#define RPITX_RX_MUTE_MAX_TAIL_MS 10000
/** @brief Default receive mute tail after unkeying, in milliseconds. */
#define RPITX_RX_MUTE_DEFAULT_TAIL_MS 50

/**
 * @brief Transmit/receive switching configuration: antenna relay (T/R switch) and PTT line.
 *
 * Keying sequence: relay to TX, @ref txDelayMs, PTT line asserted, @ref pttTxDelayMs, first sample. Unkeying
 * sequence: last sample played, @ref hangMs, PTT line released, @ref pttRxDelayMs, relay to RX. A delay whose GPIO
 * is not configured is skipped: without relay the PTT line is asserted at once, without PTT line the first sample
 * follows @ref txDelayMs and the relay returns to RX when the PTT line would have been released.
 */
struct SoapyRPITX_RelayConfig {
    int gpio = -1;                                         ///< BCM GPIO driving the relay, -1 when no relay is connected.
    bool activeLow = false;                                ///< True when the relay is in the TX position with the pin low (most relay modules).
    uint32_t txDelayMs = 20;                               ///< Time given to the relay to settle in the TX position before the PTT line is
                                                           ///< asserted (before the first sample is queued when no PTT line is configured).
    uint32_t hangMs = 100;                                 ///< Time the transmitter stays keyed after the last sample was played.
    int pttGpio = -1;                                      ///< BCM GPIO driving the PTT line (amplifier, radio), -1 when no PTT line is connected.
    bool pttActiveLow = false;                             ///< True when the PTT line is asserted with the pin low.
    uint32_t pttTxDelayMs = RPITX_PTT_DEFAULT_TX_DELAY_MS; ///< Time from the PTT line asserted to the first queued sample.
    uint32_t pttRxDelayMs = RPITX_PTT_DEFAULT_RX_DELAY_MS; ///< Time from the PTT line released to the relay returning to the RX position.
};

/**
 * @brief Checks a transmit/receive switching configuration.
 * @param cfg Configuration to check.
 * @return Empty string when valid, otherwise the reason it is refused.
 */
std::string SoapyRPITX_CheckRelayConfig(const SoapyRPITX_RelayConfig &cfg);

/**
 * @brief Keyed state of the transmitter and sequencer of the antenna relay and PTT GPIOs.
 *
 * The transmitter is keyed by the first sample written after an idle period. The relay GPIO, when configured, is
 * switched to the TX position at once; the PTT line, when configured, is asserted @ref
 * SoapyRPITX_RelayConfig::txDelayMs later (at once without relay); the samples of the write are held back until @ref
 * SoapyRPITX_RelayConfig::pttTxDelayMs after the PTT line was asserted (until the relay delay has elapsed without PTT
 * line). No RF reaches the relay contacts while they move and the amplifier is keyed before it is driven.
 *
 * The transmitter is unkeyed once every queued sample has been played and nothing was written for @ref
 * SoapyRPITX_RelayConfig::hangMs, or when the transmitter is stopped: the PTT line is released first and the relay
 * returns to the RX position @ref SoapyRPITX_RelayConfig::pttRxDelayMs later (at once when no PTT line was asserted).
 * A write that begins while the relay is still waiting to return to RX asserts the PTT line again without moving
 * the relay. The keyed state, which lasts until the relay is back in the RX position, also tells the receive path
 * when to mute.
 *
 * Both GPIOs are driven with the GPSET/GPCLR registers, which the transmitter DMA never writes. Their function is set
 * to output only by Configure(), which the device calls while no TX stream exists: the transmitter samples the
 * GPFSEL registers when it starts and rewrites them from DMA, so the pin functions must not change while it runs.
 * Destroying the object leaves the PTT line released and the relay in the RX position.
 *
 * Unkeying needs a drain state that was observed after the last write began: every BeginWrite() advances a write
 * sequence number, the supervisor reads it with WriteSeq() before it evaluates the drain state, and Poll() keeps the
 * transmitter keyed when a write began in between. Samples queued by such a write are therefore never played with
 * the relay in the RX position or the PTT line released, whatever the hang time.
 *
 * Thread model: every method is thread-safe. BeginWrite() / ReadyInUs() / EndWrite() are called by the producer
 * around each write, Poll() by the supervisor thread of the TX stream, Release() after the transmitter was stopped.
 */
class SoapyRPITX_Ptt {
public:
    /** @brief Creates an unkeyed state without relay nor PTT line. */
    SoapyRPITX_Ptt();

    /** @brief Unkeys with the unkeying sequence and leaves the PTT line released and the relay in the RX position. */
    ~SoapyRPITX_Ptt();

    SoapyRPITX_Ptt(const SoapyRPITX_Ptt &) = delete;
    SoapyRPITX_Ptt &operator=(const SoapyRPITX_Ptt &) = delete;

    /**
     * @brief Applies a transmit/receive switching configuration.
     *
     * A new pin is first driven to its inactive level (relay in RX, PTT released) and then switched to output, so it
     * never shows the active level; a pin that is no longer used is driven to its inactive level and switched to
     * input. The GPIO block is mapped on first use. Must not be called while a TX stream exists when a pin or its
     * polarity changes.
     *
     * @param cfg New configuration.
     * @throws std::runtime_error when the configuration is invalid or the GPIO block cannot be mapped (nothing
     *         is changed).
     */
    void Configure(const SoapyRPITX_RelayConfig &cfg);

    /**
     * @brief Returns the configuration in effect.
     * @return Configuration.
     */
    SoapyRPITX_RelayConfig GetConfig() const;

    /**
     * @brief Registers a write of samples; starts the keying sequence when the transmitter is not keyed.
     *
     * Every call must be paired with EndWrite(). While the returned time is not zero the caller sleeps (at most that
     * long) and asks again with ReadyInUs().
     *
     * @return Microseconds until the next step of the keying sequence, 0 when the samples can be queued now.
     */
    long BeginWrite();

    /**
     * @brief Advances the keying sequence and tells whether the samples of a write can be queued.
     *
     * Asserts the PTT line when its time has come.
     *
     * @return Microseconds until the next step of the keying sequence (PTT line or first sample), 0 when the samples
     *         can be queued now or the transmitter is not keyed.
     */
    long ReadyInUs();

    /** @brief Ends a write registered by BeginWrite(); the hang time restarts from now. */
    void EndWrite();

    /**
     * @brief Returns the write sequence number.
     *
     * The number advances on every BeginWrite(). The supervisor reads it before it evaluates the drain state passed
     * to Poll().
     *
     * @return Number of writes registered so far.
     */
    uint64_t WriteSeq() const;

    /**
     * @brief Advances the keying and unkeying sequences (supervisor thread, about every millisecond).
     *
     * Asserts the PTT line when its time has come and returns the relay to RX when the PTT release delay has
     * elapsed. The unkeying sequence starts only when no write began since @p writeSeq was read, no write is in
     * progress, the transmitter is drained and @ref SoapyRPITX_RelayConfig::hangMs has elapsed since the last activity.
     *
     * @param writeSeq Value of WriteSeq() read before @p drained was evaluated.
     * @param drained  True when every queued sample has been played or the transmitter is stopped.
     */
    void Poll(uint64_t writeSeq, bool drained);

    /**
     * @brief Unkeys now (the transmitter has been stopped, so the carrier is already off).
     *
     * Releases the PTT line and, when it was asserted, blocks for what is left of @ref
     * SoapyRPITX_RelayConfig::pttRxDelayMs before the relay returns to the RX position.
     */
    void Release();

    /**
     * @brief Tells whether the transmitter is keyed (relay, if any, not yet back in the RX position).
     * @return True from the start of the keying sequence to the end of the unkeying sequence.
     */
    bool IsKeyed() const;

    /**
     * @brief Tells whether the PTT line is asserted.
     * @return True while the PTT GPIO drives its active level; always false without PTT line.
     */
    bool IsPttAsserted() const;

    /**
     * @brief Tells whether received samples must be muted now.
     *
     * Receivers deliver samples some time after they were captured; samples captured at the end of a transmission
     * can still be read after the unkeying. The receive path stays muted for @p tailMs after the transmitter was
     * unkeyed to cover that latency.
     *
     * @param tailMs Time after the unkeying during which samples are still muted, in milliseconds.
     * @return True while keyed and during the @p tailMs that follow the last unkeying.
     */
    bool MutedNow(uint32_t tailMs) const;

    /**
     * @brief Returns how many times the transmitter has been keyed.
     * @return Count since the object was created.
     */
    uint64_t KeyCount() const;

private:
    using clock = std::chrono::steady_clock;

    /**
     * @brief Drives a pin to a level with GPSET/GPCLR.
     * @param pin   BCM GPIO number.
     * @param level Output level.
     */
    void WriteLevel(int pin, bool level);

    /**
     * @brief Drives the relay pin (mutex held, relay configured).
     * @param tx True for the TX position.
     */
    void DriveRelayLocked(bool tx);

    /**
     * @brief Drives the PTT pin (mutex held, PTT line configured).
     * @param on True to assert the PTT line.
     */
    void DrivePttLocked(bool on);

    /**
     * @brief Moves a switching output from one pin or polarity to another (mutex held).
     *
     * The old pin, when it changes, is driven to its inactive level and switched to input; the new pin is driven to
     * @p active and then switched to output.
     *
     * @param oldPin       Pin in use, -1 for none.
     * @param oldActiveLow Polarity of the pin in use.
     * @param newPin       Pin to use, -1 for none.
     * @param newActiveLow Polarity of the pin to use.
     * @param active       True to drive the new pin to its active level.
     * @param what         Name of the output for the log.
     */
    void SwitchPinLocked(int oldPin, bool oldActiveLow, int newPin, bool newActiveLow, bool active, const char *what);

    /**
     * @brief Schedules the PTT line and the first sample from a given time (mutex held, keyed).
     * @param pttAt Time at which the PTT line is asserted.
     */
    void SchedulePttLocked(clock::time_point pttAt);

    /**
     * @brief Performs the steps of the keying and unkeying sequences whose time has come (mutex held).
     * @param now Current time.
     */
    void AdvanceLocked(clock::time_point now);

    /**
     * @brief Microseconds until the next step of the keying sequence (mutex held).
     * @param now Current time.
     * @return 0 when the samples can be queued now or the transmitter is not keyed.
     */
    long WaitUsLocked(clock::time_point now) const;

    /**
     * @brief Starts the unkeying sequence (mutex held, keyed): releases the PTT line and schedules the relay.
     * @param now Current time.
     */
    void BeginUnkeyLocked(clock::time_point now);

    /** @brief Ends the unkeying sequence (mutex held): relay to RX, unkeyed, unkeying time recorded. */
    void FinishUnkeyLocked();

    mutable std::mutex mutex;             ///< Protects every member below except the atomics.
    SoapyRPITX_RelayConfig cfg;           ///< Configuration in effect.
    std::unique_ptr<generalgpio> gpio;    ///< GPIO block, mapped while a relay or a PTT line is configured.
    std::atomic<bool> keyed{false};       ///< Transmitter keyed (keying sequence started, relay not yet back in RX).
    std::atomic<bool> pttAsserted{false}; ///< PTT line asserted.
    std::atomic<uint64_t> keyCount{0};    ///< Times the transmitter was keyed.
    std::atomic<uint64_t> writeSeq{0};    ///< Writes registered by BeginWrite() so far.
    std::atomic<int64_t> unkeyedAtNs;     ///< Steady-clock time of the last unkeying in nanoseconds (far past when never keyed).
    int writers = 0;                      ///< Writes registered by BeginWrite() and not yet ended.
    clock::time_point lastBusy;           ///< Last time a write ended or the transmitter was not drained.
    bool pttPending = false;              ///< The PTT line is scheduled and not yet asserted.
    clock::time_point pttAt;              ///< Time at which the scheduled PTT line is asserted.
    clock::time_point readyAt;            ///< Time from which the samples of a write can be queued.
    bool releasing = false;               ///< The PTT line was released and the relay waits to return to RX.
    clock::time_point relayRxAt;          ///< Time at which the relay returns to RX while releasing.
};
