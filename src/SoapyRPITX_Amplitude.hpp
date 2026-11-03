/**
 * @file SoapyRPITX_Amplitude.hpp
 * @brief Hardware-independent amplitude processing of the rpitx transmit path: calibrated amplitude states of
 *        the pad drive (and of the optional second clock pin), nearest and noise-shaped (sigma-delta) state
 *        selection, fractional envelope delay, and the envelope-to-code mapping of the EER output.
 *
 * @copyright Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
 *
 * Project Site: https://github.com/hiperiondev/SoapySDR_rpitx
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING. If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

/** @brief Number of pad drive levels (2 mA to 16 mA). */
#define RPITX_PAD_LEVELS 8
/** @brief Largest envelope delay accepted, in IQ samples (positive or negative). */
#define RPITX_MAX_ENVELOPE_DELAY 32.0
/** @brief Largest number of points of the EER tables. */
#define RPITX_MAX_EER_POINTS 64

/** @brief Selection of the amplitude state for each slot. */
enum class SoapyRPITX_Quantizer {
    Nearest,    ///< State whose amplitude is nearest to the envelope.
    SigmaDelta, ///< Nearest state of the envelope plus the fed-back quantisation error (noise shaping).
};

/**
 * @brief Amplitude calibration and processing parameters.
 *
 * Empty tables select the uncalibrated model: drive level L has the relative amplitude (L + 1) / 8, no
 * AM/PM, a second clock pin weighted 1:2, and a linear EER output.
 */
struct SoapyRPITX_AmplitudeConfig {
    std::vector<double> amplitudeLut;                               ///< Relative RF amplitude of drive levels 0-7 (8 values, strictly increasing), or empty.
    std::vector<double> ampmLut;                                    ///< Phase of drive levels 0-7 relative to the reference, in radians (8 values), or empty.
    SoapyRPITX_Quantizer quantizer = SoapyRPITX_Quantizer::Nearest; ///< State selection.
    int sigmaDeltaOrder = 2;                                        ///< Order of the error feedback (1 or 2).
    double envelopeDelay = 0.5;                                     ///< Delay of the envelope relative to the phase, in IQ samples.
    double envelopeFloor = 0.0;                                     ///< Envelope below which the carrier is switched off (after a 2 ms hang); 0 disables it.
    std::vector<double> rfdacLut;   ///< Relative amplitude with GPIO4 only, GPIO20 only and both clock pins (3 values), or empty.
    std::vector<double> eerLut;     ///< RF amplitude measured at equally spaced EER codes from 0 to full scale (2-64 values, increasing), or empty.
    std::vector<double> eerAmpmLut; ///< Phase measured at equally spaced EER codes, in radians (2-64 values), or empty.
    double eerGain = 1.0;           ///< Gain applied to the EER code.
    double eerOffset = 0.0;         ///< Offset added to the EER code (fraction of full scale).
    double eerIdle = 0.0;           ///< EER code written by idle slots (fraction of full scale).
    int eerDrive = 7;               ///< Pad drive level of the clock pin in EER mode.
};

/**
 * @brief Checks an amplitude configuration.
 * @param cfg Configuration.
 * @return Empty string when valid, otherwise a description of the first problem.
 */
inline std::string SoapyRPITX_CheckAmplitudeConfig(const SoapyRPITX_AmplitudeConfig &cfg) {
    auto finite = [](const std::vector<double> &v) {
        for (double x : v)
            if (!std::isfinite(x))
                return false;
        return true;
    };
    auto increasing = [](const std::vector<double> &v) {
        for (size_t i = 1; i < v.size(); i++)
            if (!(v[i] > v[i - 1]))
                return false;
        return true;
    };
    auto withinPi = [](const std::vector<double> &v) {
        for (double x : v)
            if (std::fabs(x) > M_PI + 1e-9)
                return false;
        return true;
    };

    if (!cfg.amplitudeLut.empty() &&
        (cfg.amplitudeLut.size() != RPITX_PAD_LEVELS || !finite(cfg.amplitudeLut) || !(cfg.amplitudeLut[0] > 0) || !increasing(cfg.amplitudeLut)))
        return "amplitude_lut needs 8 positive, strictly increasing values";
    if (!cfg.ampmLut.empty() && (cfg.ampmLut.size() != RPITX_PAD_LEVELS || !finite(cfg.ampmLut) || !withinPi(cfg.ampmLut)))
        return "ampm_lut needs 8 values within +/-180 degrees";
    if (cfg.sigmaDeltaOrder < 1 || cfg.sigmaDeltaOrder > 2)
        return "sigma_delta_order must be 1 or 2";
    if (!(std::fabs(cfg.envelopeDelay) <= RPITX_MAX_ENVELOPE_DELAY))
        return "envelope_delay out of range [-32, 32]";
    if (!(cfg.envelopeFloor >= 0 && cfg.envelopeFloor <= 1))
        return "envelope_floor out of range [0, 1]";
    if (!cfg.rfdacLut.empty()) {
        if (cfg.rfdacLut.size() != 3 || !finite(cfg.rfdacLut))
            return "rfdac_lut needs 3 values";
        for (double x : cfg.rfdacLut)
            if (!(x > 0))
                return "rfdac_lut values must be positive";
    }
    if (!cfg.eerLut.empty()) {
        if (cfg.eerLut.size() < 2 || cfg.eerLut.size() > RPITX_MAX_EER_POINTS || !finite(cfg.eerLut) || cfg.eerLut[0] < 0 || !increasing(cfg.eerLut))
            return "eer_lut needs 2 to 64 non-negative, strictly increasing values";
    }
    if (!cfg.eerAmpmLut.empty() &&
        (cfg.eerAmpmLut.size() < 2 || cfg.eerAmpmLut.size() > RPITX_MAX_EER_POINTS || !finite(cfg.eerAmpmLut) || !withinPi(cfg.eerAmpmLut)))
        return "eer_ampm_lut needs 2 to 64 values within +/-180 degrees";
    if (!(cfg.eerGain > 0 && cfg.eerGain <= 10))
        return "eer_gain out of range (0, 10]";
    if (!(cfg.eerOffset >= -1 && cfg.eerOffset <= 1))
        return "eer_offset out of range [-1, 1]";
    if (!(cfg.eerIdle >= 0 && cfg.eerIdle <= 1))
        return "eer_idle out of range [0, 1]";
    if (cfg.eerDrive < 0 || cfg.eerDrive > 7)
        return "eer_drive out of range [0, 7]";
    return std::string();
}

/**
 * @brief One output state of the pad-drive amplitude path.
 */
struct SoapyRPITX_AmpState {
    int level = 0;        ///< Pad drive level 0-7.
    uint32_t pins = 0;    ///< Clock pins switched on: bit 0 GPIO4, bit 1 GPIO20 (0 = carrier off).
    double amplitude = 0; ///< Relative RF amplitude, 1.0 for the strongest state.
    double phase = 0;     ///< Phase of the state in radians (AM/PM).
};

/**
 * @brief Amplitude states available at a maximum drive level, sorted by amplitude, and the EER code mapping.
 *
 * State 0 is "carrier off" (amplitude 0). The other states combine a drive level 0..drive with the clock pins;
 * their amplitudes are the calibrated (or modelled) level amplitudes multiplied by the pin weights and
 * normalised so that the strongest state is 1.0.
 */
class SoapyRPITX_AmplitudeModel {
public:
    /**
     * @brief Builds the model.
     * @param cfg     Calibration (assumed valid, see SoapyRPITX_CheckAmplitudeConfig()).
     * @param drive   Maximum drive level 0-7.
     * @param dualPin GPIO20 is a second, weighted clock output.
     */
    SoapyRPITX_AmplitudeModel(const SoapyRPITX_AmplitudeConfig &cfg, int drive, bool dualPin) :
            config(cfg) {
        drive = std::min(std::max(drive, 0), 7);
        auto levelAmp = [&](int l) { return cfg.amplitudeLut.empty() ? (double) (l + 1) / RPITX_PAD_LEVELS : cfg.amplitudeLut[(size_t) l]; };
        const double weights[3] = {cfg.rfdacLut.empty() ? 2.0 / 3.0 : cfg.rfdacLut[0], cfg.rfdacLut.empty() ? 1.0 / 3.0 : cfg.rfdacLut[1],
                                   cfg.rfdacLut.empty() ? 1.0 : cfg.rfdacLut[2]};
        double maxWeight = dualPin ? std::max(weights[0], std::max(weights[1], weights[2])) : 1.0;
        double ref = levelAmp(drive) * maxWeight;

        states.push_back(SoapyRPITX_AmpState());
        for (int l = 0; l <= drive; l++) {
            for (uint32_t mask = 1; mask <= (dualPin ? 3u : 1u); mask++) {
                SoapyRPITX_AmpState st;
                st.level = l;
                st.pins = mask;
                st.amplitude = levelAmp(l) * (dualPin ? weights[mask - 1] : 1.0) / ref;
                st.phase = cfg.ampmLut.empty() ? 0.0 : cfg.ampmLut[(size_t) l];
                states.push_back(st);
            }
        }
        std::stable_sort(states.begin() + 1, states.end(),
                         [](const SoapyRPITX_AmpState &a, const SoapyRPITX_AmpState &b) { return a.amplitude < b.amplitude; });
        for (size_t i = 0; i + 1 < states.size(); i++)
            thresholds.push_back(0.5 * (states[i].amplitude + states[i + 1].amplitude));

        if (!cfg.eerLut.empty()) {
            double top = cfg.eerLut.back();
            for (double v : cfg.eerLut)
                eerCurve.push_back(top > 0 ? v / top : 0.0);
        }
    }

    /**
     * @brief Returns the number of states (including "off").
     * @return State count.
     */
    size_t size() const {
        return states.size();
    }

    /**
     * @brief Returns a state.
     * @param i State index (0 = off).
     * @return State.
     */
    const SoapyRPITX_AmpState &state(size_t i) const {
        return states[i];
    }

    /**
     * @brief Returns the strongest state.
     * @return Index of the state with amplitude 1.0.
     */
    size_t top() const {
        return states.size() - 1;
    }

    /**
     * @brief Returns the state whose amplitude is nearest to @p a.
     * @param a Relative amplitude (values outside [0, 1] select the weakest or strongest state).
     * @return State index.
     */
    size_t nearest(double a) const {
        if (!(a > 0))
            return 0;
        return (size_t) (std::upper_bound(thresholds.begin(), thresholds.end(), a) - thresholds.begin());
    }

    /**
     * @brief Maps a relative envelope to the EER code that produces it.
     *
     * Inverts @ref SoapyRPITX_AmplitudeConfig::eerLut by linear interpolation (identity without a table), then
     * applies the gain and the offset.
     *
     * @param a       Relative envelope in [0, 1].
     * @param clipped Set when the code had to be limited to [0, 1].
     * @return Code as a fraction of full scale, in [0, 1].
     */
    double eerCode(double a, bool &clipped) const {
        double x = std::min(std::max(a, 0.0), 1.0);
        double code = x;
        if (!eerCurve.empty()) {
            auto it = std::lower_bound(eerCurve.begin(), eerCurve.end(), x);
            size_t n = eerCurve.size();
            if (it == eerCurve.begin()) {
                code = 0;
            } else if (it == eerCurve.end()) {
                code = 1;
            } else {
                size_t i = (size_t) (it - eerCurve.begin());
                double span = eerCurve[i] - eerCurve[i - 1];
                double f = span > 0 ? (x - eerCurve[i - 1]) / span : 0.0;
                code = ((double) (i - 1) + f) / (double) (n - 1);
            }
        }
        code = code * config.eerGain + config.eerOffset;
        clipped = a > 1.0 || code > 1.0 || code < 0.0;
        return std::min(std::max(code, 0.0), 1.0);
    }

    /**
     * @brief Returns the phase of the EER output at a code.
     * @param code Code as a fraction of full scale.
     * @return Phase in radians (interpolated from @ref SoapyRPITX_AmplitudeConfig::eerAmpmLut, 0 without a table).
     */
    double eerPhase(double code) const {
        const std::vector<double> &t = config.eerAmpmLut;
        if (t.empty())
            return 0.0;
        double x = std::min(std::max(code, 0.0), 1.0) * (double) (t.size() - 1);
        size_t i = std::min((size_t) x, t.size() - 2);
        double f = x - (double) i;
        return t[i] + f * (t[i + 1] - t[i]);
    }

    const SoapyRPITX_AmplitudeConfig config; ///< Configuration the model was built from.

private:
    std::vector<SoapyRPITX_AmpState> states; ///< States sorted by amplitude, "off" first.
    std::vector<double> thresholds;          ///< Decision thresholds between consecutive states.
    std::vector<double> eerCurve;            ///< EER table normalised to 1.0 at full scale.
};

/**
 * @brief Chooses the amplitude state of each slot, optionally with noise-shaping error feedback.
 *
 * In sigma-delta mode the error between the requested envelope and the amplitude of the chosen state is fed
 * back (first order: e[n-1]; second order: 2 e[n-1] - e[n-2]), so the quantisation noise is pushed away from
 * the carrier. The error is limited to +/-1 to keep the loop stable at the ends of the range.
 */
class SoapyRPITX_AmplitudeQuantizer {
public:
    /** @brief Clears the error history. */
    void reset() {
        e1 = 0;
        e2 = 0;
    }

    /**
     * @brief Chooses the state for an envelope sample.
     * @param model      Amplitude states.
     * @param a          Requested relative envelope.
     * @param sigmaDelta Use the error feedback.
     * @param order      Order of the error feedback (1 or 2).
     * @return State index.
     */
    size_t quantize(const SoapyRPITX_AmplitudeModel &model, double a, bool sigmaDelta, int order) {
        if (!sigmaDelta)
            return model.nearest(a);
        double v = order >= 2 ? a + 2.0 * e1 - e2 : a + e1;
        size_t q = model.nearest(v);
        double e = std::min(std::max(v - model.state(q).amplitude, -1.0), 1.0);
        e2 = e1;
        e1 = e;
        return q;
    }

private:
    double e1 = 0; ///< Last error.
    double e2 = 0; ///< Error before the last one.
};

/**
 * @brief History of envelope values read back with a fractional delay (linear interpolation).
 */
class SoapyRPITX_EnvelopeDelay {
public:
    /** @brief Number of values kept; the largest usable delay is two less. */
    static constexpr size_t kLength = 256;

    /** @brief Clears the history (the delayed envelope starts from zero). */
    void reset() {
        values.fill(0.0);
        head = 0;
    }

    /**
     * @brief Appends the newest value.
     * @param v Envelope value.
     */
    void push(double v) {
        head = (head + 1) % kLength;
        values[head] = v;
    }

    /**
     * @brief Returns the value @p delay positions before the newest one.
     * @param delay Delay in positions, in [0, kLength - 2] (clamped).
     * @return Interpolated value.
     */
    double at(double delay) const {
        double d = std::min(std::max(delay, 0.0), (double) (kLength - 2));
        size_t i = (size_t) d;
        double f = d - (double) i;
        double a = values[(head + kLength - i) % kLength];
        double b = values[(head + kLength - i - 1) % kLength];
        return a + f * (b - a);
    }

private:
    std::array<double, kLength> values{}; ///< Circular history.
    size_t head = 0;                      ///< Index of the newest value.
};

/**
 * @brief Splits a requested envelope delay into an integer advance of the envelope words and a remaining
 *        fractional delay.
 *
 * The hardware delays the envelope path by @p latency slots relative to the phase path (0 for the pad drive,
 * the FIFO level for an EER DAC). To obtain a net envelope delay of @p delay slots, the envelope is written
 * @p advance slots ahead of its sample and delayed in software by the returned fractional delay.
 *
 * @param delay   Requested envelope delay relative to the phase path, in slots.
 * @param latency Envelope latency added by the hardware, in slots.
 * @param advance Receives the number of slots the envelope words are written ahead (>= 0).
 * @return Software envelope delay in slots (>= 0).
 */
inline double SoapyRPITX_SplitEnvelopeDelay(double delay, double latency, int &advance) {
    double d = delay - latency;
    if (d >= 0) {
        advance = 0;
        return d;
    }
    advance = (int) std::ceil(-d - 1e-9);
    return d + (double) advance;
}
