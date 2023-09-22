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

#include <unistd.h>
#include <sched.h>
#include <stdio.h>

#include "util.hpp"
#include "SoapyRPITX_IQ_dmaSync.hpp"

SoapyRPITX_IQ_dmaSync::SoapyRPITX_IQ_dmaSync(uint64_t TuneFrequency, uint32_t SR, int Channel, uint32_t FifoSize) :
        bufferdma(Channel, FifoSize, 4, 3) {
    SampleRate = SR;
    tunefreq = TuneFrequency;
    clkgpio::SetAdvancedPllMode(true);
    clkgpio::SetCenterFrequency(TuneFrequency, SampleRate);
    clkgpio::SetFrequency(0);
    clkgpio::enableclk(4);
    syncwithpwm = false;

    if (syncwithpwm) {
        pwmgpio::SetPllNumber(clk_plld, 1);
        pwmgpio::SetFrequency(SampleRate);
    } else {
        pcmgpio::SetPllNumber(clk_plld, 1);
        pcmgpio::SetFrequency(SampleRate);
    }

    mydsp.samplerate = SampleRate;

    Originfsel = clkgpio::gengpio.gpioreg[GPFSEL0];

    SetDmaAlgo();
}

SoapyRPITX_IQ_dmaSync::~SoapyRPITX_IQ_dmaSync() {
    clkgpio::gengpio.gpioreg[GPFSEL0] = Originfsel;
    clkgpio::disableclk(4);
}

void SoapyRPITX_IQ_dmaSync::SetDmaAlgo() {
    dma_cb_t *cbp = cbarray;
    for (uint32_t samplecnt = 0; samplecnt < buffersize; samplecnt++) {

        SetEasyCB(cbp, samplecnt * registerbysample + 1, dma_pad, 1);
        cbp++;

        SetEasyCB(cbp, samplecnt * registerbysample, dma_pllc_frac, 1);
        cbp++;

        SetEasyCB(cbp, samplecnt * registerbysample + 2, dma_fsel, 1);
        cbp++;

        SetEasyCB(cbp, samplecnt * registerbysample, syncwithpwm ? dma_pwm : dma_pcm, 1);
        cbp++;
    }

    cbp--;
    cbp->next = mem_virt_to_phys(cbarray);
}

void SoapyRPITX_IQ_dmaSync::SetIQSample(uint32_t Index, std::complex<float> sample, int Harmonic) {
    Index = Index % buffersize;
    mydsp.pushsample(sample);

    sampletab[Index * registerbysample] = (0x5A << 24) | GetMasterFrac(mydsp.frequency / Harmonic);
    int IntAmplitude = (int) (mydsp.amplitude * 8.0) - 1;

    int IntAmplitudePAD = IntAmplitude;
    if (IntAmplitude > 7)
        IntAmplitudePAD = 7;
    if (IntAmplitude < 0) {
        IntAmplitudePAD = 0;
        IntAmplitude = -1;
    }
    sampletab[Index * registerbysample + 1] = (0x5A << 24) + (IntAmplitudePAD & 0x7) + (1 << 4) + (0 << 3);

    if (IntAmplitude == -1) {
        sampletab[Index * registerbysample + 2] = (Originfsel & ~(7 << 12)) | (0 << 12);
    } else {
        sampletab[Index * registerbysample + 2] = (Originfsel & ~(7 << 12)) | (4 << 12);
    }

    PushSample(Index);
}

int SoapyRPITX_IQ_dmaSync::SetIQSamples(float *buff, size_t Size, int Harmonic = 1, const long timeoutUs = 0) {
    size_t NbWritten = 0;
    int Index = GetUserMemIndex();
    int ToWrite = (int) Size < GetBufferAvailable() ? Size : GetBufferAvailable();
    while (ToWrite < Size)
        ToWrite = (int) Size < GetBufferAvailable() ? Size : GetBufferAvailable();

    for (int i = 0; i < ToWrite; i++) {
        std::complex<float> smpl = { buff[NbWritten], buff[NbWritten + 1] };
        SetIQSample(Index + i, smpl, Harmonic);
        NbWritten += 2;
    }

    return ToWrite * 2;
}
