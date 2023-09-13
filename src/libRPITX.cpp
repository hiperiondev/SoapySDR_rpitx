/*
 * Copyright 2023 Emiliano Gonzalez (egonzalez . hiperion @ gmail . com))
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

#include <stddef.h>
#include <complex>
#include <unistd.h>

#include "libRPITX.hpp"
#include "librpitx.hpp"

using namespace std::complex_literals;

unsigned int buffer_qty = 0;
float Soapy_libRPITX_Frequency = 01e6;
float Soapy_libRPITX_PPM = 0;
int Soapy_libRPITX_SampleRate = 48000;
int Soapy_libRPITX_IQBurst = 4000;
double Soapy_libRPITX_Gain = 0;
bool Soapy_libRPITX_GainMode = false;

iqdmasync *iqsender = NULL;
int FifoSize = Soapy_libRPITX_IQBurst * 4;
float ppmpll = 0.0;
int Harmonic = 1;
std::complex<float> *CIQBuffer;

std::complex<float>* libRPITX_init(void) {
    iqsender = new iqdmasync(Soapy_libRPITX_Frequency, Soapy_libRPITX_SampleRate, 14, FifoSize, MODE_IQ);
    iqsender->Setppm(ppmpll);
    CIQBuffer = (std::complex<float>*) malloc(Soapy_libRPITX_IQBurst * sizeof(std::complex<float>));

    return CIQBuffer;
}

void libRPITX_deinit(void) {
    free(CIQBuffer);
    delete (iqsender);
}

int libRPITX_setFrequency(float frequency) {
    if (Soapy_libRPITX_Frequency != frequency) {
        Soapy_libRPITX_Frequency = frequency;
        delete (iqsender);
        usleep(1000);
        iqsender = new iqdmasync(Soapy_libRPITX_Frequency, Soapy_libRPITX_SampleRate, 14, FifoSize, MODE_IQ);
        iqsender->Setppm(ppmpll);
    }

    return 0;
}

float libRPITX_getFrequency(void) {
    return Soapy_libRPITX_Frequency;
}

int libRPITX_setPPM(float ppm) {
    Soapy_libRPITX_PPM = ppm;
    return 0;
}

float libRPITX_getPPM(void) {
    return Soapy_libRPITX_PPM;
}

int libRPITX_setSampleRate(int samplerate) {
    Soapy_libRPITX_SampleRate = samplerate;
    return 0;
}

int libRPITX_getSampleRate(void) {
    return Soapy_libRPITX_SampleRate;
}

int libRPITX_setIQBurst(unsigned int iqburst) {
    Soapy_libRPITX_IQBurst = iqburst;
    return 0;
}

unsigned int libRPITX_getIQBurst(void) {
    return Soapy_libRPITX_IQBurst;
}

void libRPITX_setGainMode(const int direction, const size_t channel, const bool automatic) {
    Soapy_libRPITX_GainMode = automatic;
}

bool libRPITX_getGainMode(const int direction, const size_t channel, const bool automatic) {
    return Soapy_libRPITX_GainMode;
}

void libRPITX_setGain(const int direction, const size_t channel, const double value) {
    Soapy_libRPITX_Gain = value;
}

double libRPITX_getGain(void) {
    return Soapy_libRPITX_Gain;
}

void libRPITX_transmit(void) {
    if (CIQBuffer == nullptr)
        return;

    iqsender->SetIQSamples(CIQBuffer, Soapy_libRPITX_IQBurst, Harmonic);
}

int libRPITX_bufferAdd(float I, float Q) { // return 1 if full
    CIQBuffer[buffer_qty++] = { I, Q };

    if (buffer_qty > libRPITX_getIQBurst() - 1) {
        buffer_qty = 0;
        return 1;
    }

    return 0;
}
