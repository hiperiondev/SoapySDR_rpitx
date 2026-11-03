#!/usr/bin/env python3
# Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
# Project Site: https://github.com/hiperiondev/SoapySDR_rpitx
#
# This is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3, or (at your option)
# any later version.
#
# rpitx_amp_calibrate.py: measures the amplitude path of the SoapySDR rpitx driver with a SoapySDR receiver
# (for example an RTL-SDR next to a dummy load) and prints the calibration settings:
#
#   pad    amplitude_lut and ampm_lut of the 8 pad drive levels
#   rfdac  rfdac_lut of the two clock pins (stream argument rf_dac=true)
#   eer    eer_lut and eer_ampm_lut of the envelope DAC (stream argument envelope_output=pcm|pwm)
#   delay  envelope_delay with the lowest third-order intermodulation in a two-tone test
#
# Every measurement alternates the state under test with a full-scale reference, so the phase drift between
# the two radios cancels. The transmitter must run as root; the receiver is tuned "--offset" Hz below the
# carrier so that the tone is away from its DC spur.

import argparse
import sys
import threading
import time

import numpy as np


def parse_frequency(text):
    """Parses '144.8M', '7074k' or '28074000' into Hz."""
    t = text.strip().lower().replace("hz", "")
    mult = 1.0
    if t and t[-1] in "kmg":
        mult = {"k": 1e3, "m": 1e6, "g": 1e9}[t[-1]]
        t = t[:-1]
    try:
        return float(t) * mult
    except ValueError:
        raise argparse.ArgumentTypeError("invalid frequency '%s'" % text)


# ---------------------------------------------------------------------------------------------------------------
# Analysis (independent of the radios)
# ---------------------------------------------------------------------------------------------------------------


def segment_values(x, fs, offset, start, dwell, count, guard=0.2):
    """Complex mean of each dwell segment after removing the receiver offset and the residual frequency.

    x: complex receiver samples, start: index of the first segment, count: number of segments. The residual
    frequency is estimated from the phase slope inside the segments. Returns an array of complex values.
    """
    n = np.arange(len(x))
    base = x * np.exp(-2j * np.pi * offset * n / fs)
    seg = int(round(dwell * fs))
    g = int(seg * guard)
    slopes = []
    for k in range(count):
        a = start + k * seg + g
        b = start + (k + 1) * seg - g
        if b > len(base) or b - a < 16:
            break
        ph = np.unwrap(np.angle(base[a:b]))
        slopes.append(np.polyfit(np.arange(b - a), ph, 1)[0])
    f_res = float(np.median(slopes)) * fs / (2 * np.pi) if slopes else 0.0
    base = base * np.exp(-2j * np.pi * f_res * n / fs)
    values = []
    for k in range(count):
        a = start + k * seg + g
        b = start + (k + 1) * seg - g
        values.append(np.mean(base[a:b]) if b <= len(base) and b > a else np.nan)
    return np.array(values)


def first_on(x, fs, offset):
    """Index of the first receiver sample where the carrier is clearly present."""
    mag = np.abs(x)
    win = max(1, int(fs * 0.002))
    smooth = np.convolve(mag, np.ones(win) / win, mode="same")
    floor = np.median(smooth[: max(win * 4, 1)])
    peak = np.percentile(smooth, 99)
    idx = np.nonzero(smooth > floor + 0.2 * (peak - floor))[0]
    return int(idx[0]) if len(idx) else 0


def relative_to_reference(values):
    """values alternate reference, state, reference, state, ... Returns (amplitude, phase in degrees) of each
    state relative to the mean of the references around it."""
    amps, phases = [], []
    for i in range(1, len(values), 2):
        refs = [values[i - 1]] + ([values[i + 1]] if i + 1 < len(values) else [])
        ref = np.mean(refs)
        r = values[i] / ref
        amps.append(float(np.abs(values[i])))
        phases.append(float(np.degrees(np.angle(r))))
    ref_level = float(np.mean(np.abs(values[0::2])))
    return np.array(amps) / ref_level, np.array(phases)


def imd3_db(x, fs, offset, f1, f2):
    """Worst third-order product of a two-tone signal relative to the tones, in dB."""
    n = len(x)
    w = np.hanning(n)
    spec = np.abs(np.fft.fft(x * w))
    freqs = np.fft.fftfreq(n, 1.0 / fs)

    def power(f):
        k = int(np.argmin(np.abs(freqs - (f + offset))))
        return float(np.max(spec[max(k - 2, 0) : k + 3]) ** 2)

    tones = 0.5 * (power(f1) + power(f2))
    imd = max(power(2 * f1 - f2), power(2 * f2 - f1))
    return 10 * np.log10(imd / tones)


def format_list(values, digits=4):
    return ",".join(("%." + str(digits) + "g") % v for v in values)


def parse_kwargs(text, option):
    """Parses 'key=value,key=value' into a dict; exits with a message naming @option for a malformed item."""
    out = {}
    for item in filter(None, (s.strip() for s in text.split(","))):
        key, sep, value = item.partition("=")
        if not sep or not key.strip():
            raise SystemExit("%s: invalid item '%s' (expected key=value)" % (option, item))
        out[key.strip()] = value.strip()
    return out


def increasing_table(name, values, first_min=None):
    """Returns @values made strictly increasing as the driver requires, warning about every value that had to be
    raised (measurement noise). With @first_min the first value is at least that much."""
    v = np.array(values, dtype=np.float64)
    fixed = []
    if first_min is not None and not v[0] >= first_min:
        v[0] = first_min
        fixed.append(0)
    eps = 1e-4 * max(float(np.max(np.abs(v))), 1e-6)
    for i in range(1, len(v)):
        if not v[i] > v[i - 1]:
            v[i] = v[i - 1] + eps
            fixed.append(i)
    if fixed:
        print("warning: %s was not strictly increasing at index %s (measurement noise); the values were raised "
              "minimally, repeat the measurement with a longer --dwell or a stronger signal" % (name, ",".join(map(str, fixed))),
              file=sys.stderr)
    return v


def phase_table(values):
    """Wraps phases in degrees into [-180, 180], as the driver requires."""
    return (np.asarray(values, dtype=np.float64) + 180.0) % 360.0 - 180.0


def self_test():
    """Runs the analysis on synthetic captures and checks the recovered values."""
    fs, offset, dwell = 240000.0, 20000.0, 0.2
    truth_amp = np.array([0.1, 0.3, 0.5, 0.65, 0.78, 0.88, 0.95, 1.0])
    truth_ph = np.array([12.0, 9.0, 7.0, 5.0, 3.0, 2.0, 1.0, 0.0])
    states = []
    for a, p in zip(truth_amp, truth_ph):
        states += [(1.0, 0.0), (a, p)]
    states.append((1.0, 0.0))
    seg = int(dwell * fs)
    lead = int(0.05 * fs)
    x = np.zeros(lead, complex)
    phase0 = 0.7
    for a, p in states:
        x = np.concatenate([x, a * np.exp(1j * (phase0 + np.radians(p))) * np.ones(seg)])
    n = np.arange(len(x))
    drift = 37.0  # Hz between the two radios
    x = x * np.exp(2j * np.pi * (offset + drift) * n / fs)
    x += 0.01 * (np.random.default_rng(1).normal(size=len(x)) + 1j * np.random.default_rng(2).normal(size=len(x)))
    start = first_on(x, fs, offset)
    values = segment_values(x, fs, offset, start, dwell, len(states))
    amp, ph = relative_to_reference(values)
    ok = np.all(np.abs(amp - truth_amp) < 0.01) and np.all(np.abs(ph - truth_ph) < 1.0)

    t = np.arange(int(fs * 0.5)) / fs
    tt = np.exp(2j * np.pi * (offset + 700) * t) + np.exp(2j * np.pi * (offset + 1900) * t)
    clean = imd3_db(tt, fs, offset, 700, 1900)
    dist = tt + 0.01 * np.exp(2j * np.pi * (offset - 500) * t)
    ok = ok and clean < -80 and abs(imd3_db(dist, fs, offset, 700, 1900) + 40) < 1
    print("self test %s" % ("passed" if ok else "FAILED"))
    print("  amplitude %s" % format_list(amp))
    print("  phase     %s" % format_list(ph))
    return 0 if ok else 1


# ---------------------------------------------------------------------------------------------------------------
# Radios
# ---------------------------------------------------------------------------------------------------------------


class Receiver:
    """Captures the SoapySDR receiver into memory in a background thread."""

    def __init__(self, SoapySDR, args, freq, rate, gain):
        from SoapySDR import SOAPY_SDR_CF32, SOAPY_SDR_RX

        self.SoapySDR = SoapySDR
        self.dev = SoapySDR.Device(args)
        self.dev.setSampleRate(SOAPY_SDR_RX, 0, rate)
        self.dev.setFrequency(SOAPY_SDR_RX, 0, freq)
        if gain is not None:
            self.dev.setGainMode(SOAPY_SDR_RX, 0, False)
            self.dev.setGain(SOAPY_SDR_RX, 0, gain)
        self.stream = self.dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32)
        self.rate = rate
        self.chunks = []
        self.running = False
        self.thread = None

    def start(self):
        self.running = True
        self.dev.activateStream(self.stream)
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        buf = np.zeros(16384, np.complex64)
        while self.running:
            sr = self.dev.readStream(self.stream, [buf], len(buf), timeoutUs=200000)
            if sr.ret > 0:
                self.chunks.append(buf[: sr.ret].copy())

    def stop(self):
        self.running = False
        self.thread.join()
        self.dev.deactivateStream(self.stream)
        self.dev.closeStream(self.stream)
        return np.concatenate(self.chunks) if self.chunks else np.zeros(0, np.complex64)


class Transmitter:
    """Opens the rpitx driver and writes constant-magnitude blocks."""

    def __init__(self, SoapySDR, a, dev_args, stream_args):
        from SoapySDR import SOAPY_SDR_CF32, SOAPY_SDR_TX

        self.SoapySDR = SoapySDR
        self.TX = SOAPY_SDR_TX
        args = dict(driver="rpitx")
        args.update(dev_args)
        self.dev = SoapySDR.Device(args)
        self.dev.setSampleRate(SOAPY_SDR_TX, 0, a.rate)
        self.dev.setFrequency(SOAPY_SDR_TX, 0, a.freq)
        self.dev.setGain(SOAPY_SDR_TX, 0, 7)
        # A short FIFO keeps the settings changed during the delay sweep close to the samples on air.
        sargs = dict(fifo_ms="40", preroll_ms="20")
        sargs.update(parse_kwargs(a.stream_args, "--stream-args"))
        sargs.update(stream_args)
        self.stream = self.dev.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, [0], sargs)
        self.dev.activateStream(self.stream)
        self.rate = a.rate

    def send(self, iq):
        done = 0
        while done < len(iq):
            sr = self.dev.writeStream(self.stream, [iq[done:]], len(iq) - done, timeoutUs=1000000)
            if sr.ret < 0 and sr.ret != self.SoapySDR.SOAPY_SDR_TIMEOUT:
                raise RuntimeError("writeStream error %d" % sr.ret)
            done += max(sr.ret, 0)

    def close(self):
        self.dev.writeStream(self.stream, [np.zeros(0, np.complex64)], 0, self.SoapySDR.SOAPY_SDR_END_BURST)
        self.dev.deactivateStream(self.stream)
        self.dev.closeStream(self.stream)


def measure(a, SoapySDR, dev_args, stream_args, states, apply_state):
    """Transmits reference / state pairs (apply_state(tx, s) returns the IQ magnitude for state s) and returns
    the relative amplitude and phase of every state."""
    rx = Receiver(SoapySDR, parse_kwargs(a.rx_args, "--rx-args"), a.freq - a.offset, a.rx_rate, a.rx_gain)
    tx = Transmitter(SoapySDR, a, dev_args, stream_args)
    n = int(round(a.dwell * a.rate))
    sequence = []
    for s in states:
        sequence += ["ref", s]
    sequence.append("ref")
    rx.start()
    time.sleep(0.3)
    try:
        for s in sequence:
            mag = apply_state(tx, s)
            tx.send(np.full(n, mag, np.complex64))
    finally:
        tx.close()
        time.sleep(0.2)
        x = rx.stop()
    start = first_on(x, a.rx_rate, a.offset)
    values = segment_values(x, a.rx_rate, a.offset, start, a.dwell, len(sequence))
    return relative_to_reference(values)


def run_pad(a, SoapySDR):
    # With the uncalibrated model and no envelope delay, magnitude (L + 1) / 8 selects drive level L, so every
    # state is carried by the I/Q stream itself.
    def apply_state(tx, s):
        return 1.0 if s == "ref" else (s + 1) / 8.0

    dev_args = dict(amplitude_mode="envelope", amplitude_lut="", ampm_lut="", envelope_delay="0", amplitude_quantizer="nearest")
    amp, ph = measure(a, SoapySDR, dev_args, {}, list(range(8)), apply_state)
    print("amplitude_lut=%s" % format_list(increasing_table("amplitude_lut", amp / amp[-1], first_min=1e-4)))
    print("ampm_lut=%s" % format_list(phase_table(ph - ph[-1])))


def run_rfdac(a, SoapySDR):
    # Distinct weights make each magnitude select exactly one pin combination at drive 7.
    magnitudes = {"gpio4": 0.7, "gpio20": 0.2, "both": 1.0}

    def apply_state(tx, s):
        return 1.0 if s == "ref" else magnitudes[s]

    dev_args = dict(amplitude_mode="envelope", rfdac_lut="0.7,0.2,1", envelope_delay="0", amplitude_quantizer="nearest")
    amp, _ = measure(a, SoapySDR, dev_args, dict(rf_dac="true"), ["gpio4", "gpio20", "both"], apply_state)
    amp = np.maximum(amp / amp[-1], 1e-4)  # the driver requires positive values
    print("rfdac_lut=%s" % format_list(amp))


def run_eer(a, SoapySDR):
    codes = np.linspace(0, 1, a.points)

    def apply_state(tx, s):
        return 1.0 if s == "ref" else float(codes[s])

    dev_args = dict(amplitude_mode="envelope", envelope_delay="0")
    amp, ph = measure(a, SoapySDR, dev_args, dict(envelope_output=a.envelope_output), list(range(a.points)), apply_state)
    print("eer_lut=%s" % format_list(increasing_table("eer_lut", amp / amp[-1], first_min=0.0)))
    # The phase at code 0 is that of a carrier with (nearly) no amplitude, i.e. noise: the next code's is used.
    ph = np.array(ph, dtype=np.float64)
    ph[0] = ph[1]
    print("eer_ampm_lut=%s" % format_list(phase_table(ph - ph[-1])))


def run_delay(a, SoapySDR):
    f1, f2 = 700.0, 1900.0
    rx = Receiver(SoapySDR, parse_kwargs(a.rx_args, "--rx-args"), a.freq - a.offset, a.rx_rate, a.rx_gain)
    stream_args = dict(envelope_output=a.envelope_output) if a.envelope_output != "none" else {}
    tx = Transmitter(SoapySDR, a, dict(amplitude_mode="envelope"), stream_args)
    n = int(round(a.dwell * a.rate))
    t = np.arange(n) / a.rate
    results = []
    try:
        rx.start()
        for d in np.arange(a.delay_min, a.delay_max + 1e-9, a.delay_step):
            tx.dev.writeSetting("envelope_delay", "%g" % d)
            iq = (0.5 * (np.exp(2j * np.pi * f1 * t) + np.exp(2j * np.pi * f2 * t))).astype(np.complex64)
            mark = len(rx.chunks)
            tx.send(iq)
            tx.send(iq)
            time.sleep(a.dwell * 0.5)
            x = np.concatenate(rx.chunks[mark:]) if len(rx.chunks) > mark else np.zeros(0, np.complex64)
            m = int(len(x) * 0.6)
            imd = imd3_db(x[len(x) - m :], a.rx_rate, a.offset, f1, f2) if m > 1024 else float("nan")
            results.append((d, imd))
            print("envelope_delay=%-6g IMD3 %6.1f dB" % (d, imd))
    finally:
        tx.close()
        rx.stop()
    best = min((r for r in results if np.isfinite(r[1])), key=lambda r: r[1], default=None)
    if best is not None:
        print("envelope_delay=%g" % best[0])


def main():
    p = argparse.ArgumentParser(description="Calibrates the amplitude path of the SoapySDR rpitx driver with a SoapySDR receiver.",
                                formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    p.add_argument("mode", choices=("pad", "rfdac", "eer", "delay", "self-test"), help="what to measure")
    p.add_argument("--freq", type=parse_frequency, help="transmit frequency (e.g. 7.1M)")
    p.add_argument("--rate", type=float, default=48000, help="transmit sample rate")
    p.add_argument("--rx-args", default="driver=rtlsdr", help="SoapySDR receiver device arguments")
    p.add_argument("--rx-rate", type=float, default=240000, help="receiver sample rate")
    p.add_argument("--rx-gain", type=float, default=None, help="receiver gain (default: automatic)")
    p.add_argument("--offset", type=float, default=20000, help="receiver tuned this many Hz below the carrier")
    p.add_argument("--dwell", type=float, default=0.4, help="seconds per measured state")
    p.add_argument("--points", type=int, default=9, help="number of envelope codes measured in eer mode (2-64)")
    p.add_argument("--envelope-output", choices=("none", "pcm", "pwm"), default="pcm", help="envelope output (eer and delay modes)")
    p.add_argument("--delay-min", type=float, default=-2.0, help="first envelope_delay of the delay sweep")
    p.add_argument("--delay-max", type=float, default=2.0, help="last envelope_delay of the delay sweep")
    p.add_argument("--delay-step", type=float, default=0.25, help="step of the delay sweep")
    p.add_argument("--stream-args", default="", help="extra rpitx stream arguments, e.g. 'preroll_ms=200'")
    a = p.parse_args()

    if a.mode == "self-test":
        return self_test()
    if a.freq is None:
        p.error("--freq is required")
    if not 2 <= a.points <= 64:
        p.error("--points must be 2 to 64")

    import SoapySDR

    if a.mode == "pad":
        run_pad(a, SoapySDR)
    elif a.mode == "rfdac":
        run_rfdac(a, SoapySDR)
    elif a.mode == "eer":
        run_eer(a, SoapySDR)
    else:
        run_delay(a, SoapySDR)
    return 0


if __name__ == "__main__":
    sys.exit(main())
