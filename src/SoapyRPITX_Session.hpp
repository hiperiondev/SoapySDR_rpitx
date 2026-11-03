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

#include "gpio.hpp"

/**
 * @brief Directory of the lock file.
 *
 * A tmpfs directory, so that a record never survives a reboot (VideoCore handles do not either). Can be
 * overridden at build time (the host-side tests use their build directory).
 */
#ifndef RPITX_LOCK_DIR
#define RPITX_LOCK_DIR "/run/lock"
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
    long pid = 0;              ///< Process that owned the transmitter.
    int dmaChannel = -1;       ///< DMA channel in use (-1 when none was allocated).
    unsigned memHandle = 0;    ///< VideoCore memory handle (0 when none was allocated).
    uint32_t pads = 0x1B;      ///< PADS_GPIO_0 value (drive, hysteresis, slew) to restore.
    uint32_t gpioFunction = 0; ///< Function-select code to restore on the RF output GPIO.
    bool pcmValid = false;     ///< @ref pcmCtl and @ref pcmDiv hold the original PCM clock.
    uint32_t pcmCtl = 0;       ///< Original PCMCLK_CNTL (without password).
    uint32_t pcmDiv = 0;       ///< Original PCMCLK_DIV (without password).
    ClockTreeState clockTree;  ///< Clock tree before librpitx modified it.
};

/**
 * @brief Formats a record as the text stored in the lock file.
 * @param r Record.
 * @return "key=value" lines.
 */
std::string SoapyRPITX_FormatRecord(const SoapyRPITX_SessionRecord &r);

/**
 * @brief Parses the text stored in the lock file.
 * @param text Lock-file contents.
 * @param r    Receives the record.
 * @return True when the text holds a record (at least the pid).
 */
bool SoapyRPITX_ParseRecord(const std::string &text, SoapyRPITX_SessionRecord &r);

/**
 * @brief Returns the hardware described by a record to its idle state.
 *
 * Resets the DMA channel, switches the RF output GPIO to its recorded function (input when it was the clock
 * output), restores the pads, stops GPCLK0, disables the PCM block and restores its clock, restores the
 * clock tree and releases the VideoCore memory. Every step is attempted even when an earlier one fails.
 *
 * @param r   Record left by the terminated process.
 * @param gpioLine RF output GPIO.
 * @return Text describing what was done (for logging).
 */
std::string SoapyRPITX_RecoverHardware(const SoapyRPITX_SessionRecord &r, unsigned gpioLine);

/**
 * @brief Tells whether a GPIO line of the SoC GPIO controller is claimed by a driver or a program.
 *
 * Uses the GPIO character device (/dev/gpiochipN whose label starts with "pinctrl-bcm2").
 *
 * @param line     GPIO number.
 * @param consumer Receives the consumer name (may be empty) when the line is claimed.
 * @return True when the line is claimed; false when it is free or cannot be queried.
 */
bool SoapyRPITX_GpioLineClaimed(unsigned line, std::string &consumer);

/**
 * @brief Tells whether the device tree enables the PCM/I2S controller for a sound card or another driver.
 * @return True when the i2s node status is "okay".
 */
bool SoapyRPITX_I2sEnabled();

/**
 * @brief Exclusive, system-wide claim of the transmitter hardware.
 *
 * Holds an flock() on @ref RPITX_LOCK_DIR/@ref RPITX_LOCK_FILE_NAME for its lifetime. The lock is released
 * by the kernel when the process dies, so finding a non-empty lock file after acquiring the lock means that
 * the previous owner terminated without cleaning up: its record is then recovered with
 * SoapyRPITX_RecoverHardware().
 */
class SoapyRPITX_Session {
public:
    /**
     * @brief Acquires the lock and recovers a stale record.
     * @param gpioLine RF output GPIO (used by the recovery).
     * @throws std::runtime_error when the lock file cannot be opened or another running process holds it.
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
     * @brief Returns the description of the recovery.
     * @return Text built by SoapyRPITX_RecoverHardware() (empty without recovery).
     */
    const std::string &RecoveryReport() const;

    /**
     * @brief Stores a record in the lock file (the pid is filled in).
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
    std::string report;     ///< Recovery description.
};
