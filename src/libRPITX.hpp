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

#pragma once

#include <stddef.h>

extern std::complex<float> *CIQBuffer;

std::complex<float>* libRPITX_init(void);
void libRPITX_deinit(void);

int libRPITX_setFrequency(float frequency);
float libRPITX_getFrequency(void);
int libRPITX_setPPM(float ppm);
float libRPITX_getPPM(void);
int libRPITX_setSampleRate(int samplerate);
int libRPITX_getSampleRate(void);
int libRPITX_setIQBurst(unsigned int iqburst);
unsigned int libRPITX_getIQBurst(void);
void libRPITX_setGainMode(const int direction, const size_t channel, const bool automatic);
bool libRPITX_getGainMode(const int direction, const size_t channel, const bool automatic);
void libRPITX_setGain(const int direction, const size_t channel, const double value);
double libRPITX_getGain(void);
void libRPITX_transmit(void);
int libRPITX_bufferAdd(float I, float Q);
