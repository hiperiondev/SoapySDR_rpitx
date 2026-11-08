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

#include <algorithm>
#include <stdexcept>
#include <thread>

#include <SoapySDR/Logger.hpp>

#include "SoapyRPITX_Ptt.hpp"
#include "gpio.hpp"

namespace {

// Output set and clear registers of GPIO 0-31 (word index in the GPIO block).
constexpr unsigned kGpset0 = 0x1C / 4;
constexpr unsigned kGpclr0 = 0x28 / 4;

// The RF output (GPIO4) can never drive the relay nor the PTT line.
constexpr int kRfGpio = 4;

// Checks a switching output pin; empty string when valid.
std::string checkPin(const char *key, int pin) {
    if (pin != -1 && (pin < RPITX_RELAY_MIN_GPIO || pin > RPITX_RELAY_MAX_GPIO))
        return std::string(key) + " must be -1 (none) or a GPIO from " + std::to_string(RPITX_RELAY_MIN_GPIO) + " to " + std::to_string(RPITX_RELAY_MAX_GPIO);
    if (pin == kRfGpio)
        return std::string(key) + " cannot be GPIO4, the RF output";
    return std::string();
}

// Microseconds from now to t, rounded up; 0 when t has passed.
long usUntil(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point t) {
    if (t <= now)
        return 0;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t - now).count();
    return (long) ((ns + 999) / 1000);
}

} // namespace

std::string SoapyRPITX_CheckRelayConfig(const SoapyRPITX_RelayConfig &cfg) {
    std::string why = checkPin("relay_gpio", cfg.gpio);
    if (!why.empty())
        return why;
    why = checkPin("ptt_gpio", cfg.pttGpio);
    if (!why.empty())
        return why;
    if (cfg.gpio >= 0 && cfg.gpio == cfg.pttGpio)
        return "relay_gpio and ptt_gpio cannot be the same GPIO";
    if (cfg.txDelayMs > RPITX_RELAY_MAX_DELAY_MS)
        return "relay_tx_delay_ms out of range [0, " + std::to_string(RPITX_RELAY_MAX_DELAY_MS) + "]";
    if (cfg.hangMs > RPITX_RELAY_MAX_HANG_MS)
        return "relay_hang_ms out of range [0, " + std::to_string(RPITX_RELAY_MAX_HANG_MS) + "]";
    if (cfg.pttTxDelayMs > RPITX_PTT_MAX_DELAY_MS)
        return "ptt_tx_delay_ms out of range [0, " + std::to_string(RPITX_PTT_MAX_DELAY_MS) + "]";
    if (cfg.pttRxDelayMs > RPITX_PTT_MAX_DELAY_MS)
        return "ptt_rx_delay_ms out of range [0, " + std::to_string(RPITX_PTT_MAX_DELAY_MS) + "]";
    return std::string();
}

SoapyRPITX_Ptt::SoapyRPITX_Ptt() :
        unkeyedAtNs(INT64_MIN / 2),
        lastBusy(clock::now()) {
}

SoapyRPITX_Ptt::~SoapyRPITX_Ptt() {
    Release();
    std::lock_guard<std::mutex> lock(mutex);
    if (!gpio)
        return;
    if (cfg.pttGpio >= 0)
        DrivePttLocked(false);
    if (cfg.gpio >= 0)
        DriveRelayLocked(false);
}

void SoapyRPITX_Ptt::WriteLevel(int pin, bool level) {
    gpio->gpioreg[level ? kGpset0 : kGpclr0] = 1u << (unsigned) pin;
}

void SoapyRPITX_Ptt::DriveRelayLocked(bool tx) {
    // TX position: high, or low for an active-low relay.
    WriteLevel(cfg.gpio, tx != cfg.activeLow);
}

void SoapyRPITX_Ptt::DrivePttLocked(bool on) {
    // Asserted: high, or low for an active-low PTT line.
    WriteLevel(cfg.pttGpio, on != cfg.pttActiveLow);
    pttAsserted.store(on);
}

void SoapyRPITX_Ptt::SwitchPinLocked(int oldPin, bool oldActiveLow, int newPin, bool newActiveLow, bool active, const char *what) {
    const bool pinChanged = newPin != oldPin;
    if (oldPin >= 0 && pinChanged) {
        // The old pin returns to its inactive level and is released.
        WriteLevel(oldPin, oldActiveLow);
        gpio->setmode((uint32_t) oldPin, fsel_input);
        SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: %s released from GPIO%d", what, oldPin);
    }
    if (newPin >= 0 && (pinChanged || newActiveLow != oldActiveLow)) {
        // Level first, function afterwards: the pin never shows the active level while it becomes an output.
        WriteLevel(newPin, active != newActiveLow);
        gpio->setmode((uint32_t) newPin, fsel_output);
        SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: %s on GPIO%d (active %s)", what, newPin, newActiveLow ? "low" : "high");
    }
}

void SoapyRPITX_Ptt::Configure(const SoapyRPITX_RelayConfig &next) {
    std::string why = SoapyRPITX_CheckRelayConfig(next);
    if (!why.empty())
        throw std::runtime_error("rpitx: " + why);

    std::lock_guard<std::mutex> lock(mutex);

    if ((next.gpio >= 0 || next.pttGpio >= 0) && !gpio)
        gpio.reset(new generalgpio()); // throws without /dev/mem access: nothing changed yet

    const SoapyRPITX_RelayConfig prev = cfg;
    cfg = next;
    SwitchPinLocked(prev.gpio, prev.activeLow, cfg.gpio, cfg.activeLow, keyed.load(), "antenna relay");
    SwitchPinLocked(prev.pttGpio, prev.pttActiveLow, cfg.pttGpio, cfg.pttActiveLow, pttAsserted.load(), "PTT line");
    if (cfg.pttGpio < 0) {
        pttAsserted.store(false);
        pttPending = false;
    }

    if (prev.gpio != cfg.gpio || prev.pttGpio != cfg.pttGpio || prev.txDelayMs != cfg.txDelayMs || prev.hangMs != cfg.hangMs ||
        prev.pttTxDelayMs != cfg.pttTxDelayMs || prev.pttRxDelayMs != cfg.pttRxDelayMs) {
        if (cfg.gpio >= 0 || cfg.pttGpio >= 0)
            SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: T/R sequence: relay %s, %u ms, PTT %s, %u ms, RF; unkey after %u ms hang, PTT off, %u ms, relay RX",
                          cfg.gpio >= 0 ? ("GPIO" + std::to_string(cfg.gpio)).c_str() : "none", cfg.txDelayMs,
                          cfg.pttGpio >= 0 ? ("GPIO" + std::to_string(cfg.pttGpio)).c_str() : "none", cfg.pttTxDelayMs, cfg.hangMs, cfg.pttRxDelayMs);
    }

    // A relay waiting for the PTT release delay is returned at once when there is no longer a relay or PTT line.
    if (releasing && (cfg.gpio < 0 || cfg.pttGpio < 0))
        FinishUnkeyLocked();

    if (cfg.gpio < 0 && cfg.pttGpio < 0)
        gpio.reset();
}

SoapyRPITX_RelayConfig SoapyRPITX_Ptt::GetConfig() const {
    std::lock_guard<std::mutex> lock(mutex);
    return cfg;
}

void SoapyRPITX_Ptt::SchedulePttLocked(clock::time_point at) {
    if (cfg.pttGpio >= 0) {
        pttPending = true;
        pttAt = at;
        readyAt = at + std::chrono::milliseconds(cfg.pttTxDelayMs);
    } else {
        pttPending = false;
        readyAt = at;
    }
}

void SoapyRPITX_Ptt::AdvanceLocked(clock::time_point now) {
    if (pttPending && now >= pttAt) {
        pttPending = false;
        DrivePttLocked(true);
        // The PTT delay runs from the moment the line was actually asserted.
        const clock::time_point due = now + std::chrono::milliseconds(cfg.pttTxDelayMs);
        if (due > readyAt)
            readyAt = due;
    }
    if (releasing && now >= relayRxAt)
        FinishUnkeyLocked();
}

long SoapyRPITX_Ptt::WaitUsLocked(clock::time_point now) const {
    if (!keyed.load())
        return 0;
    if (pttPending)
        return std::max<long>(usUntil(now, pttAt), 1);
    return usUntil(now, readyAt);
}

void SoapyRPITX_Ptt::BeginUnkeyLocked(clock::time_point now) {
    pttPending = false;
    if (cfg.pttGpio >= 0 && pttAsserted.load()) {
        DrivePttLocked(false);
        if (cfg.gpio >= 0 && cfg.pttRxDelayMs > 0) {
            releasing = true;
            relayRxAt = now + std::chrono::milliseconds(cfg.pttRxDelayMs);
            return;
        }
    }
    FinishUnkeyLocked();
}

void SoapyRPITX_Ptt::FinishUnkeyLocked() {
    releasing = false;
    pttPending = false;
    if (cfg.gpio >= 0)
        DriveRelayLocked(false);
    keyed.store(false);
    unkeyedAtNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count());
}

uint64_t SoapyRPITX_Ptt::WriteSeq() const {
    return writeSeq.load();
}

long SoapyRPITX_Ptt::BeginWrite() {
    std::lock_guard<std::mutex> lock(mutex);
    writers++;
    writeSeq.fetch_add(1);
    const clock::time_point now = clock::now();
    lastBusy = now;

    if (!keyed.load()) {
        keyed.store(true);
        keyCount.fetch_add(1);
        if (cfg.gpio >= 0) {
            DriveRelayLocked(true);
            SchedulePttLocked(now + std::chrono::milliseconds(cfg.txDelayMs));
        } else {
            SchedulePttLocked(now);
        }
    } else if (releasing) {
        // The relay is still in the TX position: only the PTT line is keyed again.
        releasing = false;
        SchedulePttLocked(now);
    }

    AdvanceLocked(now);
    return WaitUsLocked(now);
}

long SoapyRPITX_Ptt::ReadyInUs() {
    std::lock_guard<std::mutex> lock(mutex);
    const clock::time_point now = clock::now();
    AdvanceLocked(now);
    return WaitUsLocked(now);
}

void SoapyRPITX_Ptt::EndWrite() {
    std::lock_guard<std::mutex> lock(mutex);
    if (writers > 0)
        writers--;
    lastBusy = clock::now();
}

void SoapyRPITX_Ptt::Poll(uint64_t seq, bool drained) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!keyed.load())
        return;

    const clock::time_point now = clock::now();
    AdvanceLocked(now);
    if (!keyed.load() || releasing)
        return;

    // A write that began after the drain state was evaluated may have queued samples that state does not cover.
    if (writers > 0 || !drained || writeSeq.load() != seq) {
        lastBusy = now;
        return;
    }
    if (now - lastBusy < std::chrono::milliseconds(cfg.hangMs))
        return;

    BeginUnkeyLocked(now);
}

void SoapyRPITX_Ptt::Release() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!keyed.load())
        return;
    if (!releasing)
        BeginUnkeyLocked(clock::now());
    if (releasing) {
        // The amplifier gets the PTT release delay before the relay contacts move.
        std::this_thread::sleep_until(relayRxAt);
        FinishUnkeyLocked();
    }
}

bool SoapyRPITX_Ptt::MutedNow(uint32_t tailMs) const {
    if (keyed.load())
        return true;
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
    return now - unkeyedAtNs.load() < (int64_t) tailMs * 1000000;
}

bool SoapyRPITX_Ptt::IsKeyed() const {
    return keyed.load();
}

bool SoapyRPITX_Ptt::IsPttAsserted() const {
    return pttAsserted.load();
}

uint64_t SoapyRPITX_Ptt::KeyCount() const {
    return keyCount.load();
}
