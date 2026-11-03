/**
 * @file SoapyRPITX_Session.hpp
 * @brief System-wide ownership of the transmitter hardware: lock file, crash record and recovery of the
 *        hardware left running by a terminated process, and checks for conflicting kernel users.
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

#include <string>
#include <utility>
#include <vector>

#include "gpio.hpp"

/**
 * @brief Directory of the lock file.
 *
 * A private directory on tmpfs, so that a record never survives a reboot (VideoCore handles do not either).
 * It is created with mode 0700 and must be owned by the effective user and not accessible to anybody else;
 * otherwise the session refuses to start. Can be overridden at build time (the host-side tests use their build
 * directory).
 */
#ifndef RPITX_LOCK_DIR
#define RPITX_LOCK_DIR "/run/soapy_rpitx"
#endif

/** @brief Name of the lock file inside @ref RPITX_LOCK_DIR. */
#define RPITX_LOCK_FILE_NAME "soapy_rpitx.lock"

/**
 * @brief Hardware state owned by a transmitter, written to the lock file while it exists.
 *
 * When a process dies without releasing the transmitter, the record lets the next process (or the
 * rpitx_reset tool) stop the DMA, turn the carrier off, restore the clocks and pads and release the
 * VideoCore memory.
 */
struct SoapyRPITX_SessionRecord {
    long pid = 0;                                            ///< Process that owned the transmitter.
    int dmaChannel = -1;                                     ///< DMA channel in use (-1 when none was allocated).
    bool dmaMaskKnown = true;                                ///< The kernel DMA channel mask was readable when @ref dmaChannel was allocated.
    unsigned memHandle = 0;                                  ///< VideoCore memory handle (0 when none was allocated).
    uint32_t pads = 0x1B;                                    ///< PADS_GPIO_0 value (drive, hysteresis, slew) to restore.
    uint32_t gpioFunction = 0;                               ///< Function-select code to restore on the RF output GPIO.
    bool pcmValid = false;                                   ///< The PCM block was used; @ref pcmCtl and @ref pcmDiv hold its original clock.
    uint32_t pcmCtl = 0;                                     ///< Original PCMCLK_CNTL (without password).
    uint32_t pcmDiv = 0;                                     ///< Original PCMCLK_DIV (without password).
    ClockTreeState clockTree;                                ///< Clock tree before librpitx modified it.
    std::vector<std::pair<unsigned, uint32_t>> pinFunctions; ///< Other GPIOs used by the transmitter and their original function.
    bool pwmValid = false;                                   ///< The PWM fields hold the original PWM state.
    uint32_t pwmClkCtl = 0;                                  ///< Original PWMCLK_CNTL (without password).
    uint32_t pwmClkDiv = 0;                                  ///< Original PWMCLK_DIV (without password).
    uint32_t pwmCtl = 0;                                     ///< Original PWM_CTL.
    uint32_t pwmRng = 0;                                     ///< Original PWM_RNG1.
    uint32_t pwmDmac = 0;                                    ///< Original PWM_DMAC.
    bool complete = true;                                    ///< The record text ended with its end marker (set by SoapyRPITX_ParseRecord()).
};

/**
 * @brief Returns the function to restore on a GPIO used by the transmitter.
 *
 * A clock output left on the RF pins (GPIO4 ALT0, GPIO20 ALT5) is replaced by input, so that a recorded
 * state taken while a carrier was on does not switch the carrier back on.
 *
 * @param pin      GPIO number.
 * @param function Recorded function-select code.
 * @return Function-select code to restore.
 */
uint32_t SoapyRPITX_RestoredFunction(unsigned pin, uint32_t function);

/**
 * @brief Checks that a record only describes hardware a transmitter may have owned.
 *
 * The DMA channel must be a legacy channel of the SoC not reserved for the kernel, the GPIOs must be in
 * 0-27, the pad value must only hold drive, hysteresis and slew, and the saved clock generators must use a
 * valid source. When the kernel DMA channel mask cannot be read, a channel is accepted only when the record
 * states that the mask could not be read when the channel was allocated either (a channel selected
 * explicitly by the application under the same condition).
 *
 * @param r   Record.
 * @param why Receives the reason when the record is refused.
 * @return True when the record can be applied by SoapyRPITX_RecoverHardware().
 */
bool SoapyRPITX_RecordPlausible(const SoapyRPITX_SessionRecord &r, std::string &why);

/**
 * @brief Formats a record as the text stored in the lock file.
 * @param r Record.
 * @return "key=value" lines, the last one being the end marker "end=1".
 */
std::string SoapyRPITX_FormatRecord(const SoapyRPITX_SessionRecord &r);

/**
 * @brief Parses the text stored in the lock file.
 *
 * Parsing stops at the end marker: text after it (left by an interrupted update of a longer record) is
 * ignored. A record without the end marker is returned with @ref SoapyRPITX_SessionRecord::complete false.
 *
 * @param text Lock-file contents.
 * @param r    Receives the record.
 * @return True when the text holds a record (at least the pid).
 */
bool SoapyRPITX_ParseRecord(const std::string &text, SoapyRPITX_SessionRecord &r);

/**
 * @brief Returns the hardware described by a record to its idle state.
 *
 * Resets the DMA channel, switches the RF output GPIO and the other recorded GPIOs to their recorded function
 * (input instead of a clock output, see SoapyRPITX_RestoredFunction()), restores the pads, stops GPCLK0,
 * disables the PCM block and restores its clock (when it was used), stops the PWM block and restores it (when it
 * was used), restores the clock tree and releases the VideoCore memory. The VideoCore memory is released only
 * for a complete record: freeing a handle that may belong to another VideoCore client is never attempted.
 * Every step is attempted even when an earlier one fails.
 *
 * @param r   Record left by the terminated process.
 * @param gpioLine RF output GPIO.
 * @return Text describing what was done (for logging).
 */
std::string SoapyRPITX_RecoverHardware(const SoapyRPITX_SessionRecord &r, unsigned gpioLine);

/**
 * @brief Tells whether a GPIO line of the SoC GPIO controller is claimed by a driver or a program.
 *
 * Uses the GPIO character device (/dev/gpiochipN whose label starts with "pinctrl-bcm2"), with the v2 line
 * information request and, when the kernel does not provide it, the v1 request.
 *
 * @param line     GPIO number.
 * @param consumer Receives the consumer name (may be empty) when the line is claimed.
 * @param checked  Receives true when the line state could be queried, false when no SoC GPIO controller was
 *                 found or both requests failed.
 * @return True when the line is claimed; false when it is free or cannot be queried.
 */
bool SoapyRPITX_GpioLineClaimed(unsigned line, std::string &consumer, bool &checked);

/**
 * @brief Tells whether the device tree enables the PCM/I2S controller for a sound card or another driver.
 * @return True when the i2s node exists and its status is "okay" or "ok", or it has no status property.
 */
bool SoapyRPITX_I2sEnabled();

/**
 * @brief Tells whether the device tree enables a user of the PWM block (PWM driver or analogue audio).
 * @return True when the pwm node or the audio node exists and has status "okay" or "ok", or no status
 *         property.
 */
bool SoapyRPITX_PwmEnabled();

/**
 * @brief Returns the text of an error number (thread safe).
 * @param err errno value.
 * @return Description of the error.
 */
std::string SoapyRPITX_ErrnoText(int err);

/**
 * @brief Exclusive, system-wide claim of the transmitter hardware.
 *
 * Holds an flock() on @ref RPITX_LOCK_DIR/@ref RPITX_LOCK_FILE_NAME for its lifetime. The lock is released
 * by the kernel when the process dies, so finding a non-empty lock file after acquiring the lock means that
 * the previous owner terminated without cleaning up: its record is then recovered with
 * SoapyRPITX_RecoverHardware(), or discarded when SoapyRPITX_RecordPlausible() refuses it.
 *
 * The lock directory must be a directory owned by the effective user with no access for group and others,
 * and the lock file a regular file owned by the effective user with a single link; symbolic links are not
 * followed. The lock is shared with child processes created by fork() without exec().
 */
class SoapyRPITX_Session {
public:
    /**
     * @brief Acquires the lock and recovers a stale record.
     *
     * When an exception leaves the constructor the lock file is closed, which releases the lock.
     *
     * @param gpioLine RF output GPIO (used by the recovery).
     * @throws std::runtime_error when the lock directory or file is not private to the effective user, the
     *         lock file cannot be opened, or another running process holds it.
     */
    explicit SoapyRPITX_Session(unsigned gpioLine);

    /** @brief Clears the record and releases the lock. */
    ~SoapyRPITX_Session();

    SoapyRPITX_Session(const SoapyRPITX_Session &) = delete;
    SoapyRPITX_Session &operator=(const SoapyRPITX_Session &) = delete;

    /**
     * @brief Tells whether a stale record was found and recovered.
     * @return True after a recovery.
     */
    bool Recovered() const;

    /**
     * @brief Tells whether a stale record was found and discarded as implausible.
     * @return True when a record was refused by SoapyRPITX_RecordPlausible().
     */
    bool Discarded() const;

    /**
     * @brief Returns the description of the recovery.
     * @return Text built by SoapyRPITX_RecoverHardware(), or the reason a record was discarded (empty
     *         otherwise).
     */
    const std::string &RecoveryReport() const;

    /**
     * @brief Stores a record in the lock file (the pid is filled in).
     *
     * The record is written over the previous one and the file is then cut to its length, so the file never
     * holds an empty record in between. Write errors are reported on stderr.
     *
     * @param r Record.
     */
    void Write(const SoapyRPITX_SessionRecord &r);

    /** @brief Empties the lock file. */
    void Clear();

    /**
     * @brief Returns the path of the lock file.
     * @return Absolute path.
     */
    static std::string LockPath();

private:
    int fd = -1;            ///< Lock file descriptor.
    bool recovered = false; ///< A stale record was recovered.
    bool discarded = false; ///< A stale record was discarded.
    std::string report;     ///< Recovery description.
};
