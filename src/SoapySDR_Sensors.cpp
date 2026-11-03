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

#include <cstdio>
#include <stdexcept>

#include "SoapyRPITX.hpp"

// ******************** Sensor API ********************

std::vector<std::string> SoapyRPITX::listSensors(void) const {
    return {"pll_locked", "dma_running", "fifo_fill", "underflows", "late", "dma_errors", "sample_rate", "envelope_clip", "envelope_late"};
}

SoapySDR::ArgInfo SoapyRPITX::getSensorInfo(const std::string &key) const {
    SoapySDR::ArgInfo info;
    info.key = key;

    if (key == "pll_locked") {
        info.name = "PLLC locked";
        info.description = "PLLC (carrier PLL) lock status.";
        info.type = SoapySDR::ArgInfo::BOOL;
        info.value = "false";
    } else if (key == "dma_running") {
        info.name = "DMA running";
        info.description = "True while the DMA engine plays the sample ring.";
        info.type = SoapySDR::ArgInfo::BOOL;
        info.value = "false";
    } else if (key == "fifo_fill") {
        info.name = "FIFO fill";
        info.description = "Fill level of the software FIFO.";
        info.type = SoapySDR::ArgInfo::FLOAT;
        info.units = "%";
        info.value = "0";
    } else if (key == "underflows") {
        info.name = "Underflows";
        info.description = "Times the FIFO ran empty in the middle of a burst (the host did not supply samples in time).";
        info.type = SoapySDR::ArgInfo::INT;
        info.value = "0";
    } else if (key == "late") {
        info.name = "DMA overruns";
        info.description = "Times the DMA engine overtook the feeder thread (feeder stalled).";
        info.type = SoapySDR::ArgInfo::INT;
        info.value = "0";
    } else if (key == "dma_errors") {
        info.name = "DMA errors";
        info.description = "Times the DMA engine stopped by itself and was restarted.";
        info.type = SoapySDR::ArgInfo::INT;
        info.value = "0";
    } else if (key == "sample_rate") {
        info.name = "Actual sample rate";
        info.description = "IQ sample rate produced by the pacing clock (the nominal rate quantised by its divider).";
        info.type = SoapySDR::ArgInfo::FLOAT;
        info.units = "Hz";
        info.value = "0";
    } else if (key == "envelope_clip") {
        info.name = "Envelope clips";
        info.description = "Slots whose envelope (|IQ| x iq_scale, or the EER code) exceeded full scale and was limited.";
        info.type = SoapySDR::ArgInfo::INT;
        info.value = "0";
    } else if (key == "envelope_late") {
        info.name = "Late envelope words";
        info.description = "Envelope words that could not be written ahead of their sample because the DMA ring was nearly empty.";
        info.type = SoapySDR::ArgInfo::INT;
        info.value = "0";
    } else {
        throw std::runtime_error("rpitx: unknown sensor '" + key + "'");
    }

    return info;
}

std::string SoapyRPITX::readSensor(const std::string &key) const {
    std::lock_guard<std::mutex> lock(device_mutex);

    // Validates the key.
    SoapySDR::ArgInfo info = getSensorInfo(key);

    if (!tx_stream)
        return info.value;

    SoapyRPITX_IQ_dmaSync &tx = tx_stream->transmitter();

    if (key == "pll_locked")
        return tx.IsPllLocked() ? "true" : "false";

    SoapyRPITX_TxStats stats = tx.GetStats();
    if (key == "dma_running")
        return stats.dmaRunning ? "true" : "false";
    if (key == "fifo_fill") {
        char buf[32];
        double pct = stats.fifoCapacity ? 100.0 * (double) stats.fifoLevel / (double) stats.fifoCapacity : 0.0;
        std::snprintf(buf, sizeof(buf), "%.1f", pct);
        return buf;
    }
    if (key == "underflows")
        return std::to_string(stats.underflows);
    if (key == "dma_errors")
        return std::to_string(stats.dmaErrors);
    if (key == "envelope_clip")
        return std::to_string(stats.envelopeClips);
    if (key == "envelope_late")
        return std::to_string(stats.envelopeLate);
    if (key == "sample_rate") {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", tx.GetActualSampleRate());
        return buf;
    }
    return std::to_string(stats.late);
}
