/*
 * Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com))
 * * Project Site: https://github.com/hiperiondev/SoapySDR_rpitx *
 *
 * This is based on other projects:
 *    librpitx (https://github.com/F5OEO/librpitx)
 *        Copyright (C) 2018  Evariste COURJAUD F5OEO
 *    SoapyPlutoSDR (https://github.com/pothosware/SoapyPlutoSDR)
 *
 *    please contact their authors for more information.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include <stdint.h>

#include "dma.hpp"
#include "gpio.hpp"
#include "dsp.hpp"

class SoapyRPITX_IQ_dmaSync: public bufferdma, public clkgpio, public pwmgpio, public pcmgpio {
protected:
    uint64_t tunefreq;
    bool syncwithpwm;
    dsp mydsp;
    uint32_t Originfsel; //Save the original FSEL GPIO
    uint32_t SampleRate;

public:
    SoapyRPITX_IQ_dmaSync(uint64_t TuneFrequency, uint32_t SR, int Channel, uint32_t FifoSize);
    ~SoapyRPITX_IQ_dmaSync();
    void SetDmaAlgo();

    void SetIQSample(uint32_t Index, std::complex<float> sample, int Harmonic);
    int SetIQSamples(float *buff, size_t Size, int Harmonic, const long timeoutUs);
};
