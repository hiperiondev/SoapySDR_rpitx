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

#include <stdexcept>

#include <SoapySDR/Logger.hpp>

#include "SoapyRPITX_Ptt.hpp"
#include "gpio.hpp"

namespace {

// Output set and clear registers of GPIO 0-31 (word index in the GPIO block).
constexpr unsigned kGpset0 = 0x1C / 4;
constexpr unsigned kGpclr0 = 0x28 / 4;

// The RF output (GPIO4) can never drive the relay.
constexpr int kRfGpio = 4;

} // namespace

std::string SoapyRPITX_CheckRelayConfig(const SoapyRPITX_RelayConfig &cfg) {
    if (cfg.gpio != -1 && (cfg.gpio < RPITX_RELAY_MIN_GPIO || cfg.gpio > RPITX_RELAY_MAX_GPIO))
        return "relay_gpio must be -1 (no relay) or a GPIO from " + std::to_string(RPITX_RELAY_MIN_GPIO) + " to " + std::to_string(RPITX_RELAY_MAX_GPIO);
    if (cfg.gpio == kRfGpio)
        return "relay_gpio cannot be GPIO4, the RF output";
    if (cfg.txDelayMs > RPITX_RELAY_MAX_DELAY_MS)
        return "relay_tx_delay_ms out of range [0, " + std::to_string(RPITX_RELAY_MAX_DELAY_MS) + "]";
    if (cfg.hangMs > RPITX_RELAY_MAX_HANG_MS)
        return "relay_hang_ms out of range [0, " + std::to_string(RPITX_RELAY_MAX_HANG_MS) + "]";
    return std::string();
}

SoapyRPITX_Ptt::SoapyRPITX_Ptt() :
        lastBusy(clock::now()) {
}

SoapyRPITX_Ptt::~SoapyRPITX_Ptt() {
    std::lock_guard<std::mutex> lock(mutex);
    keyed.store(false);
    if (cfg.gpio >= 0 && gpio)
        DriveLocked(false);
}

void SoapyRPITX_Ptt::WriteLevel(int pin, bool level) {
    gpio->gpioreg[level ? kGpset0 : kGpclr0] = 1u << (unsigned) pin;
}

void SoapyRPITX_Ptt::DriveLocked(bool tx) {
    // TX position: high, or low for an active-low relay.
    WriteLevel(cfg.gpio, tx != cfg.activeLow);
}

void SoapyRPITX_Ptt::Configure(const SoapyRPITX_RelayConfig &next) {
    std::string why = SoapyRPITX_CheckRelayConfig(next);
    if (!why.empty())
        throw std::runtime_error("rpitx: " + why);

    std::lock_guard<std::mutex> lock(mutex);

    if (next.gpio >= 0 && !gpio)
        gpio.reset(new generalgpio()); // throws without /dev/mem access: nothing changed yet

    const bool pinChanged = next.gpio != cfg.gpio;
    const bool polarityChanged = next.activeLow != cfg.activeLow;

    if (cfg.gpio >= 0 && pinChanged) {
        // The old pin returns to the RX position and is released.
        DriveLocked(false);
        gpio->setmode((uint32_t) cfg.gpio, fsel_input);
        SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: antenna relay released from GPIO%d", cfg.gpio);
    }

    const bool wasKeyed = keyed.load();
    cfg = next;

    if (cfg.gpio >= 0 && (pinChanged || polarityChanged)) {
        // Level first, function afterwards: the pin never shows the TX position while it becomes an output.
        DriveLocked(wasKeyed);
        gpio->setmode((uint32_t) cfg.gpio, fsel_output);
        SoapySDR_logf(SOAPY_SDR_INFO, "rpitx: antenna relay on GPIO%d (TX = %s), %u ms TX delay, %u ms hang", cfg.gpio, cfg.activeLow ? "low" : "high",
                      cfg.txDelayMs, cfg.hangMs);
    }

    if (cfg.gpio < 0)
        gpio.reset();
}

SoapyRPITX_RelayConfig SoapyRPITX_Ptt::GetConfig() const {
    std::lock_guard<std::mutex> lock(mutex);
    return cfg;
}

long SoapyRPITX_Ptt::BeginWrite() {
    std::lock_guard<std::mutex> lock(mutex);
    writers++;
    lastBusy = clock::now();
    if (keyed.load())
        return 0;

    keyed.store(true);
    keyCount.fetch_add(1);
    if (cfg.gpio < 0)
        return 0;
    DriveLocked(true);
    return (long) cfg.txDelayMs * 1000L;
}

void SoapyRPITX_Ptt::EndWrite() {
    std::lock_guard<std::mutex> lock(mutex);
    if (writers > 0)
        writers--;
    lastBusy = clock::now();
}

void SoapyRPITX_Ptt::Poll(bool drained) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!keyed.load())
        return;

    const clock::time_point now = clock::now();
    if (writers > 0 || !drained) {
        lastBusy = now;
        return;
    }
    if (now - lastBusy < std::chrono::milliseconds(cfg.hangMs))
        return;

    keyed.store(false);
    if (cfg.gpio >= 0)
        DriveLocked(false);
}

void SoapyRPITX_Ptt::Release() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!keyed.exchange(false))
        return;
    if (cfg.gpio >= 0)
        DriveLocked(false);
}

bool SoapyRPITX_Ptt::IsKeyed() const {
    return keyed.load();
}

uint64_t SoapyRPITX_Ptt::KeyCount() const {
    return keyCount.load();
}
