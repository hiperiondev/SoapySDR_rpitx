// Copyright (C) 2018  Evariste COURJAUD F5OEO
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "dsp.hpp"

dsp::dsp() {
}

dsp::dsp(uint32_t srate) :
        samplerate(srate) {
}

// Angle helper adapted from F4GKR: https://github.com/f4gkr/RadiantBee
double dsp::angleDiff(double a, double b) {
    double dif = fmod(b - a + M_PI, 2 * M_PI);
    if (dif < 0)
        dif += 2 * M_PI;
    return dif - M_PI;
}

void dsp::reset() {
    prev_phase = 0;
    phase_valid = false;
    amplitude = 0;
    frequency = 0;
}

void dsp::pushsample(std::complex<float> sample) {
    amplitude = std::abs(sample);

    // Below the gate the phase is noise: the carrier stays at the centre frequency and the last valid phase is
    // kept, so that the next valid sample applies the phase change across the gap.
    if (!(amplitude >= magnitude_gate)) {
        frequency = 0;
        return;
    }

    double phase = atan2((double) sample.imag(), (double) sample.real());
    if (!phase_valid) {
        prev_phase = phase;
        phase_valid = true;
        frequency = 0;
        return;
    }

    double dp = angleDiff(prev_phase, phase);
    frequency = (dp * (double) samplerate) / (2.0 * M_PI);
    prev_phase = phase;
}
