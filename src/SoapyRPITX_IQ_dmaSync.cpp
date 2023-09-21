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
// Usermem :
// FRAC frequency
// PAD Amplitude
// FSEL for amplitude 0
    SampleRate = SR;
    tunefreq = TuneFrequency;
    clkgpio::SetAdvancedPllMode(true);
    clkgpio::SetCenterFrequency(TuneFrequency, SampleRate); // Write Mult Int and Frac : FixMe carrier is already there
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

    // Note : Spurious are at +/-(19.2MHZ/2^20)*Div*N : (N=1,2,3...) So we need to have a big div to spurious away BUT
    // Spurious are ALSO at +/-(19.2MHZ/2^20)*(2^20-Div)*N
    // Max spurious avoid is to be in the center ! Theory shoud be that spurious are set away at 19.2/2= 9.6Mhz ! But need to get account of div of PLLClock

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

        //@2 Write a frequency sample : Order of DMA CS influence maximum rate : here 0,2,1 is the best : why !!!!!!
        SetEasyCB(cbp, samplecnt * registerbysample, dma_pllc_frac, 1);
        cbp++;

        //@1
        //Set Amplitude  to FSEL for amplitude=0
        SetEasyCB(cbp, samplecnt * registerbysample + 2, dma_fsel, 1);
        cbp++;

        //@3 Delay
        SetEasyCB(cbp, samplecnt * registerbysample, syncwithpwm ? dma_pwm : dma_pcm, 1);
        //dbg_printf(1,"cbp : sample %x src %x dest %x next %x\n",samplecnt,cbp->src,cbp->dst,cbp->next);
        cbp++;

    }

    cbp--;
    cbp->next = mem_virt_to_phys(cbarray); // We loop to the first CB
    //dbg_printf(1,"Last cbp :  src %x dest %x next %x\n",cbp->src,cbp->dst,cbp->next);
}

void SoapyRPITX_IQ_dmaSync::SetIQSample(uint32_t Index, std::complex<float> sample, int Harmonic) {

    Index = Index % buffersize;
    mydsp.pushsample(sample);
    /*if(mydsp.frequency>2250) mydsp.frequency=2250;
     if(mydsp.frequency<1000) mydsp.frequency=1000;*/
    sampletab[Index * registerbysample] = (0x5A << 24) | GetMasterFrac(mydsp.frequency / Harmonic); //Frequency
    int IntAmplitude = (int) (mydsp.amplitude * 8.0) - 1; //Fixme 1e4 seems to work with SSB but should be an issue with classical IQ file

    int IntAmplitudePAD = IntAmplitude;
    if (IntAmplitude > 7)
        IntAmplitudePAD = 7;
    if (IntAmplitude < 0) {
        IntAmplitudePAD = 0;
        IntAmplitude = -1;
    }
    sampletab[Index * registerbysample + 1] = (0x5A << 24) + (IntAmplitudePAD & 0x7) + (1 << 4) + (0 << 3); // Amplitude PAD

    //sampletab[Index*registerbysample+2]=(Originfsel & ~(7 << 12)) | (4 << 12); //Alternate is CLK
    if (IntAmplitude == -1) {
        sampletab[Index * registerbysample + 2] = (Originfsel & ~(7 << 12)) | (0 << 12); //Pin is in -> Amplitude 0
    } else {
        sampletab[Index * registerbysample + 2] = (Originfsel & ~(7 << 12)) | (4 << 12); //Alternate is CLK : Fixme : do not work with clk2
    }

    //dbg_printf(1,"amp%f %d\n",mydsp.amplitude,IntAmplitudePAD);
    PushSample(Index);
}

int SoapyRPITX_IQ_dmaSync::SetIQSamples(float *buff, size_t Size, int Harmonic = 1, const long timeoutUs = 0) {
    size_t NbWritten = 0;
    int Available = 0;
    //long int start_time;
    //long time_difference = 0;
    //struct timespec gettime_now;
    int ToWrite;

    //clock_gettime(CLOCK_REALTIME, &gettime_now);
    //start_time = gettime_now.tv_nsec;

    int Index = GetUserMemIndex();
    ToWrite = (int) Size < Available ? Size - NbWritten : GetBufferAvailable();
    while (ToWrite < Size)
        ToWrite = (int) Size < Available ? Size : GetBufferAvailable();

    for (int i = 0; i < ToWrite; i++) {
        std::complex<float> smpl = { buff[NbWritten], buff[NbWritten + 1] };
        SetIQSample(Index + i, smpl, Harmonic);
        NbWritten += 2;

        //clock_gettime(CLOCK_REALTIME, &gettime_now);
        //if (gettime_now.tv_nsec - start_time > timeoutUs / 1000)
        //    return NbWritten;
    }

    return ToWrite * 2;
}
