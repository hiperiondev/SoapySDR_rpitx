/**
 * @file dsp.hpp
 * @brief Conversion of complex baseband samples into instantaneous amplitude and frequency.
 *
 * @copyright Copyright (C) 2018 Evariste COURJAUD F5OEO
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DEF_DSP
#define DEF_DSP

#include <stdint.h>
#include <math.h>
#include <complex>

/**
 * @brief Polar decomposition of an IQ stream for a phase/frequency-modulated clock.
 *
 * Each call to pushsample() updates @ref amplitude (|s|) and @ref frequency, the instantaneous frequency
 * offset in Hz derived from the phase difference between consecutive samples. The phase difference is
 * wrapped to [-pi, pi), so the frequency is always within [-samplerate/2, samplerate/2).
 *
 * The phase of a sample whose magnitude is below @ref magnitude_gate is meaningless: such samples report a
 * zero frequency (the carrier stays at the centre frequency) and the phase of the last sample above the gate
 * is kept. The next sample above the gate applies the whole phase change across the gap, so phase
 * reversals through zero (BPSK) are preserved and the carrier phase stays continuous.
 */
class dsp {
protected:
    double prev_phase = 0;    ///< Phase of the last sample above the gate, wrapped to [-pi, pi).
    bool phase_valid = false; ///< True when @ref prev_phase holds the phase of an ungated sample.

    /**
     * @brief Returns the signed difference @p b - @p a wrapped to [-pi, pi).
     * @param a First angle in radians.
     * @param b Second angle in radians.
     * @return Wrapped difference in radians.
     */
    double angleDiff(double a, double b);

public:
    uint32_t samplerate = 0;      ///< Sample rate in Hz used to scale the phase difference.
    double amplitude = 0;         ///< Magnitude of the last sample.
    double frequency = 0;         ///< Instantaneous frequency offset in Hz of the last sample.
    double magnitude_gate = 1e-3; ///< Magnitude below which the phase of a sample is ignored.

    /** @brief Creates a converter with a zero sample rate. */
    dsp();

    /**
     * @brief Creates a converter for the given sample rate.
     * @param srate Sample rate in Hz.
     */
    dsp(uint32_t srate);

    /** @brief Clears the phase history, the amplitude and the frequency (start of a new burst). */
    void reset();

    /**
     * @brief Processes one complex sample and updates @ref amplitude and @ref frequency.
     * @param sample Complex baseband sample (full scale is magnitude 1.0).
     */
    void pushsample(std::complex<float> sample);
};
#endif
