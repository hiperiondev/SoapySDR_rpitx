#!/usr/bin/env python3
# Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
# Project Site: https://github.com/hiperiondev/SoapySDR_rpitx
#
# This is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3, or (at your option)
# any later version.
#
# rpitx_autotune.py: closed-loop over-the-air test and parameter tuning of the SoapySDR rpitx driver.
#
# The tool transmits known test frames through the rpitx driver in every modulation family (CW, NBFM, WBFM,
# FSK, AFSK, MFSK, BPSK, QPSK, 8PSK, 16QAM, OFDM, AM, USB, LSB and a wideband robustness signal), receives them
# with a SoapySDR receiver (an RTL-SDR next to a dummy load is enough), measures the impairments against the
# known waveform and changes the driver settings and stream arguments stage by stage until every selected mode
# meets its acceptance thresholds or a configured budget is exhausted. The bare-pin path (envelope_output=none)
# and the EER path (envelope_output=pcm or pwm) are tuned separately and can be selected on the command line.
#
# Sub-commands:
#   run        pre-flight checks, tuning stages, closed-loop verification, profile and report
#   verify     verification of a saved profile only
#   replay     re-analysis of saved captures, no radio
#   self-test  analysis and optimiser checks on simulated hardware, no radio (used by CI)
#
# Every metric is compared with the measurement floor of the receiver: a threshold the bench cannot resolve is
# reported as UNVERIFIABLE, and a metric that no tunable parameter improves is reported as HARDWARE_LIMIT, so the
# search always ends. The transmitter must run as root, into a dummy load, on a frequency you are licensed for.

import argparse
import bisect
import copy
import hashlib
import json
import math
import os
import signal
import stat
import sys
import tempfile
import threading
import time

import numpy as np

TOOL_VERSION = "1.0"

NOTICE = ("GPIO4 outputs a square wave rich in harmonics. Use a band filter, a dummy load and enough attenuation "
          "in front of the receiver, and transmit only on frequencies and powers you are licensed for.")

# ---------------------------------------------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------------------------------------------


def parse_frequency(text):
    """Parses '144.8M', '7074k', '-150k' or '28074000' into Hz."""
    t = text.strip().lower().replace("hz", "")
    mult = 1.0
    if t and t[-1] in "kmg":
        mult = {"k": 1e3, "m": 1e6, "g": 1e9}[t[-1]]
        t = t[:-1]
    try:
        return float(t) * mult
    except ValueError:
        raise argparse.ArgumentTypeError("invalid frequency '%s'" % text)


def parse_kwargs(text, option):
    """Parses 'key=value,key=value' into a dict; exits with a message naming @option for a malformed item."""
    out = {}
    for item in filter(None, (s.strip() for s in text.split(","))):
        key, sep, value = item.partition("=")
        if not sep or not key.strip():
            raise SystemExit("%s: invalid item '%s' (expected key=value)" % (option, item))
        out[key.strip()] = value.strip()
    return out


def parse_stage_limits(text):
    """Parses 'S4=40,S5=20' into a dict of per-stage evaluation limits."""
    out = {}
    for key, value in parse_kwargs(text, "--stage-limit").items():
        if key not in STAGE_LIMITS:
            raise SystemExit("--stage-limit: unknown stage '%s'" % key)
        try:
            out[key] = int(value)
        except ValueError:
            raise SystemExit("--stage-limit: invalid number '%s'" % value)
    return out


def format_list(values, digits=4):
    return ",".join(("%." + str(digits) + "g") % v for v in values)


def parse_list(text):
    return [float(v) for v in text.split(",")] if text else []


def increasing_table(values, first_min=None):
    """Returns (@values made strictly increasing as the driver requires, indices that had to be raised)."""
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
    return v, fixed


def wrap_degrees(values):
    """Wraps phases in degrees into [-180, 180)."""
    return (np.asarray(values, dtype=np.float64) + 180.0) % 360.0 - 180.0


def stable_hash(obj):
    """Short deterministic hash of a JSON-serialisable object."""
    return hashlib.sha1(json.dumps(obj, sort_keys=True).encode()).hexdigest()[:16]


def seed_of(text):
    return int(hashlib.sha1(text.encode()).hexdigest()[:8], 16)


def db10(x, tiny=1e-30):
    return 10.0 * math.log10(max(float(x), tiny))


def next_pow2(n):
    return 1 << int(math.ceil(math.log2(max(int(n), 1))))


def trusted_owners():
    """User IDs whose files and directories this tool trusts: root, the effective user and, under sudo, the
    user who ran sudo (the same rule the driver applies to profile files)."""
    owners = {0, os.geteuid()}
    sudo = os.environ.get("SUDO_UID", "")
    if os.getuid() == 0 and sudo.isdigit() and int(sudo) != 0:
        owners.add(int(sudo))
    return owners


def trusted_output_dir(path):
    """Creates the output directory @path if needed and checks that nobody else can redirect the files written
    into it.

    The tool runs as root to drive the transmitter: a directory owned by another user, writable by group or
    others, or reached through a symbolic link would let another user replace a file name with a link to a
    system file. Such a directory is refused. A directory created here gets mode 0755 whatever the umask."""
    if not os.path.lexists(path):
        os.makedirs(path, exist_ok=True)
        os.chmod(path, 0o755)
    st = os.lstat(path)
    if stat.S_ISLNK(st.st_mode) or not stat.S_ISDIR(st.st_mode):
        raise SystemExit("output directory %s is a symbolic link or not a directory" % path)
    if st.st_uid not in trusted_owners():
        raise SystemExit("output directory %s is owned by another user" % path)
    if st.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
        raise SystemExit("output directory %s is writable by group or others" % path)
    return path


def _open_output(path, binary):
    """Opens @path for writing with mode 0644, never through a symbolic link, and only when it is (or becomes) a
    regular file of a trusted owner."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW | os.O_CLOEXEC, 0o644)
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode) or st.st_uid not in trusted_owners():
            raise OSError("%s is not a regular file of a trusted user" % path)
        os.fchmod(fd, 0o644)
        return os.fdopen(fd, "wb" if binary else "w")
    except BaseException:
        os.close(fd)
        raise


def open_mode_644(path):
    """Opens a text file for writing with mode 0644 whatever the umask (and whatever mode an old file had).

    The driver refuses profile files that group or others can write, so the mode of the files this tool writes
    must not depend on the umask (002 is common on Debian / Raspberry Pi OS and in some sudo setups). Symbolic
    links are not followed (the call fails with ELOOP)."""
    return _open_output(path, False)


def open_binary_644(path):
    """Binary variant of open_mode_644()."""
    return _open_output(path, True)


def write_json_atomic(path, obj):
    """Writes @obj as JSON to @path through a temporary file with an unpredictable name in the same directory."""
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(os.path.abspath(path)), prefix="." + os.path.basename(path) + ".", suffix=".tmp")
    try:
        os.fchmod(fd, 0o644)
        with os.fdopen(fd, "w") as f:
            json.dump(obj, f, indent=1, sort_keys=True, default=float)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


class AnalysisError(Exception):
    """The capture cannot be analysed (no signal, no sync, frame not complete)."""


class Infeasible(Exception):
    """The driver or the signal generator refuses a configuration."""


class BudgetExhausted(Exception):
    """A global limit of the run was reached."""


class Interrupted(Exception):
    """The user asked the run to stop."""


# ---------------------------------------------------------------------------------------------------------------
# DSP primitives (numpy only)
# ---------------------------------------------------------------------------------------------------------------


def fft_resample(x, n_out):
    """Band-limited resampling of a whole block to @n_out samples (spectrum truncated or zero-padded)."""
    n = len(x)
    if n_out == n:
        return np.array(x, dtype=np.complex128)
    X = np.fft.fft(x)
    Y = np.zeros(n_out, dtype=np.complex128)
    m = min(n, n_out)
    h = (m + 1) // 2
    Y[:h] = X[:h]
    if m - h > 0:
        Y[-(m - h):] = X[-(m - h):]
    return np.fft.ifft(Y) * (n_out / n)


def warp(y, factor, steps=8):
    """Samples @y at the positions k * @factor, |factor - 1| * len(y) of the order of one sample.

    The advance grows linearly along the block; it is interpolated between @steps + 1 copies of @y shifted by
    equally spaced fractional delays (band-limited, in the frequency domain)."""
    n = len(y)
    adv = np.arange(n) * (factor - 1.0)
    total = adv[-1] if n else 0.0
    if abs(total) < 1e-6:
        return np.asarray(y, np.complex128)
    Y = np.fft.fft(y)
    k = np.fft.fftfreq(n)
    step = total / steps
    pos = adv / step
    seg = np.clip(np.floor(pos).astype(int), 0, steps - 1)
    lam = pos - seg
    out = np.empty(n, np.complex128)
    prev = np.fft.ifft(Y)
    for m in range(steps):
        nxt = np.fft.ifft(Y * np.exp(2j * np.pi * k * (m + 1) * step))
        sel = np.nonzero(seg == m)[0]
        if len(sel):
            a, b = sel[0], sel[-1] + 1
            out[a:b] = (1 - lam[a:b]) * prev[a:b] + lam[a:b] * nxt[a:b]
        prev = nxt
    return out


def is_smooth(n):
    """True when @n has no prime factor above 5 (fast FFT length)."""
    for p in (2, 3, 5):
        while n % p == 0 and n > 1:
            n //= p
    return n == 1


def smooth_lengths(lo, hi):
    out = []
    p2 = 1
    while p2 <= hi:
        p3 = p2
        while p3 <= hi:
            p5 = p3
            while p5 <= hi:
                if p5 >= lo:
                    out.append(p5)
                p5 *= 5
            p3 *= 3
        p2 *= 2
    return sorted(out)


def resample_rate(x, rate_in, rate_out):
    """Resamples @x from @rate_in to @rate_out; returns the samples and the output rate.

    The block is zero-padded to a fast FFT length whose resampled length is fast too, resampled in the frequency
    domain, and the remaining ratio (below one sample over the block) is removed by a time warp, so the output rate
    equals @rate_out."""
    n = len(x)
    rho = rate_out / rate_in
    m, q = n, max(int(round(n * rho)), 1)
    for c in smooth_lengths(n, int(n * 1.3) + 64):
        if is_smooth(max(int(round(c * rho)), 1)):
            m, q = c, max(int(round(c * rho)), 1)
            break
    xp = np.concatenate([np.asarray(x, np.complex128), np.zeros(m - n, np.complex128)]) if m > n else x
    y = fft_resample(xp, q)
    factor = (q * rate_in / m) / rate_out
    if abs(factor - 1.0) * q > 1e-4:
        y = warp(y, factor)
    return y[: max(int(round(n * rho)), 1)], float(rate_out)


def frac_shift(x, delay):
    """Delays @x by @delay samples (fractional, circular, band-limited)."""
    n = len(x)
    k = np.fft.fftfreq(n)
    return np.fft.ifft(np.fft.fft(x) * np.exp(-2j * np.pi * k * delay))


def band_mask_filter(x, rate, lo, hi):
    """Keeps the frequencies lo <= |f| <= hi of a real or complex signal (FFT mask)."""
    X = np.fft.fft(x)
    f = np.abs(np.fft.fftfreq(len(x), 1.0 / rate))
    X[(f < lo) | (f > hi)] = 0
    return np.fft.ifft(X)


def xcorr_mag(x, ref):
    """|sum x[n + lag] conj(ref[n])| for lag 0 .. len(x) - len(ref)."""
    n = len(x) + len(ref)
    nf = next_pow2(n)
    c = np.fft.ifft(np.fft.fft(x, nf) * np.conj(np.fft.fft(ref, nf)))
    return np.abs(c[: len(x) - len(ref) + 1])


def parabolic(v, i):
    if 0 < i < len(v) - 1:
        a, b, c = v[i - 1], v[i], v[i + 1]
        d = a - 2 * b + c
        if d != 0:
            return float(i) + 0.5 * (a - c) / d
    return float(i)


def periodogram(x, rate):
    """Hann-window power spectral density (per Hz) and frequencies, both fft-shifted."""
    w = np.hanning(len(x))
    X = np.fft.fftshift(np.fft.fft(x * w))
    psd = np.abs(X) ** 2 / (np.sum(w ** 2) * rate)
    f = np.fft.fftshift(np.fft.fftfreq(len(x), 1.0 / rate))
    return psd, f


def band_power(psd, f, lo, hi):
    df = f[1] - f[0] if len(f) > 1 else 1.0
    m = (f >= lo) & (f <= hi)
    return float(np.sum(psd[m]) * df)


def tone_power(psd, f, f0, half_bins=3):
    df = f[1] - f[0]
    return band_power(psd, f, f0 - (half_bins + 0.5) * df, f0 + (half_bins + 0.5) * df)


def rrc_taps(beta, sps, span):
    """Root-raised-cosine taps normalised to unit energy."""
    t = (np.arange(span * sps + 1) - span * sps / 2.0) / sps
    h = np.zeros(len(t))
    for i, ti in enumerate(t):
        if abs(ti) < 1e-9:
            h[i] = 1.0 - beta + 4 * beta / np.pi
        elif abs(abs(4 * beta * ti) - 1.0) < 1e-9:
            h[i] = beta / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * beta)) + (1 - 2 / np.pi) * np.cos(np.pi / (4 * beta)))
        else:
            h[i] = (np.sin(np.pi * ti * (1 - beta)) + 4 * beta * ti * np.cos(np.pi * ti * (1 + beta))) / (np.pi * ti * (1 - (4 * beta * ti) ** 2))
    return h / np.sqrt(np.sum(h ** 2))


def m_sequence(length=127):
    """Maximal-length sequence of 2^7 - 1 chips (x^7 + x^6 + 1) as 0/1 values."""
    state = [1] * 7
    out = []
    for _ in range(length):
        out.append(state[-1])
        fb = state[6] ^ state[5]
        state = [fb] + state[:-1]
    return np.array(out)


def msk(chips, sps):
    """Constant-envelope MSK of @chips (0/1), @sps samples per chip."""
    d = 2 * chips - 1
    inc = np.repeat(d * (np.pi / 2) / sps, sps)
    ph = np.concatenate([[0.0], np.cumsum(inc)[:-1]])
    return np.exp(1j * ph)


def cpfsk(freqs, rate):
    """Continuous-phase signal following the instantaneous frequencies @freqs (Hz per sample)."""
    ph = 2 * np.pi * np.concatenate([[0.0], np.cumsum(freqs)[:-1]]) / rate
    return np.exp(1j * ph)


def inst_freq(y, rate):
    return np.angle(y[1:] * np.conj(y[:-1])) * rate / (2 * np.pi)


def mean_freq(y, rate):
    """Power-weighted mean frequency of a short segment."""
    return float(np.angle(np.sum(y[1:] * np.conj(y[:-1]))) * rate / (2 * np.pi))


def sine_fit(x, rate, f0, harmonics=1):
    """Least-squares fit of a DC term and @harmonics harmonics of @f0; returns (amplitudes, fitted signal)."""
    t = np.arange(len(x)) / rate
    cols = [np.ones(len(x))]
    for k in range(1, harmonics + 1):
        cols += [np.sin(2 * np.pi * k * f0 * t), np.cos(2 * np.pi * k * f0 * t)]
    A = np.stack(cols, axis=1)
    c, *_ = np.linalg.lstsq(A, x, rcond=None)
    amps = [float(np.hypot(c[2 * k - 1], c[2 * k])) for k in range(1, harmonics + 1)]
    return amps, A @ c


# ---------------------------------------------------------------------------------------------------------------
# Modes, metrics and default thresholds
# ---------------------------------------------------------------------------------------------------------------

# Amplitude mode used for every signal (user modes and internal measurement signals).
MODE_AMPLITUDE = {
    "carrier": "constant", "cw": "envelope", "nbfm": "constant", "wbfm": "constant", "fsk": "constant",
    "afsk": "constant", "mfsk": "constant", "bpsk": "envelope", "qpsk": "envelope", "8psk": "envelope",
    "qam16": "envelope", "ofdm": "envelope", "am": "envelope", "usb": "envelope", "lsb": "envelope",
    "iq_raw": "envelope", "staircase": "envelope", "pauses": "envelope",
}

USER_MODES = ["cw", "nbfm", "wbfm", "fsk", "afsk", "mfsk", "bpsk", "qpsk", "8psk", "qam16", "ofdm", "am", "usb", "lsb",
              "iq_raw"]
DEFAULT_MODES = [m for m in USER_MODES if m not in ("wbfm", "ofdm")]

# Modes that decide the outcome of a path with --required-modes default: on the bare pin the constant-envelope
# modes, BPSK and SSB (the 8 pad levels limit QPSK / 8PSK / 16QAM / OFDM / AM); on the EER path every mode.
DEFAULT_REQUIRED = {
    "non_eer": ["cw", "nbfm", "wbfm", "fsk", "afsk", "mfsk", "bpsk", "usb", "lsb", "iq_raw"],
    "eer": list(USER_MODES),
}
DEFAULT_ALLOW_HL = {"non_eer": "qam16,ofdm", "eer": "none"}

# name: (sense, unit, description). "max": the value must not exceed the limit; "min": it must reach it.
METRICS = {
    "freq_err_hz": ("max", "Hz", "carrier frequency error"),
    "clock_ppm": ("max", "ppm", "sample clock error after the crystal correction"),
    "counters": ("max", "", "underflows + late + dma_errors + envelope_late during the burst"),
    "clip": ("max", "slots", "envelope_clip during the burst"),
    "key_timing_ms": ("max", "ms", "worst CW keying edge error"),
    "leak_dbc": ("max", "dBc", "carrier leakage while the key or envelope is off"),
    "click_dbc": ("max", "dBc", "key-click power 500 Hz - 3 kHz from the carrier"),
    "dev_err_pct": ("max", "%", "FM deviation error"),
    "sinad_db": ("min", "dB", "demodulated audio SINAD"),
    "ber": ("max", "", "bit error ratio"),
    "ser": ("max", "", "symbol error ratio"),
    "tone_err_hz": ("max", "Hz", "FSK tone frequency error"),
    "evm_db": ("max", "dB", "error vector magnitude"),
    "phase_wander_deg": ("max", "deg", "phase wander across the burst"),
    "acpr_db": ("max", "dBc", "adjacent channel power ratio (worse side)"),
    "thd_pct": ("max", "%", "total harmonic distortion of the demodulated audio"),
    "imd3_dbc": ("max", "dBc", "worst third-order intermodulation product of the two-tone test"),
    "oss_db": ("min", "dB", "opposite sideband suppression"),
    "carrier_dbc": ("max", "dBc", "residual carrier of the SSB signal"),
    "nmse_db": ("max", "dB", "in-band waveform error against the transmitted reference"),
}

# Default acceptance thresholds per path and mode. They are initial values: adjust them with --thresholds after
# the first measurements of a real board.
THRESHOLDS = {
    "global": {"freq_abs_hz": 1.0, "freq_ppm": 0.1, "clock_ppm": 2.0, "counters": 0, "clip": 0},
    "non_eer": {
        "cw": {"key_timing_ms": 1.0, "leak_dbc": -50, "click_dbc": -35},
        "nbfm": {"dev_err_pct": 2.0, "sinad_db": 35},
        "wbfm": {"dev_err_pct": 2.0, "sinad_db": 30},
        "fsk": {"ber": 0, "tone_err_hz": 1.0},
        "afsk": {"ber": 0},
        "mfsk": {"ser": 0, "tone_err_hz": 0.2},
        "bpsk": {"evm_db": -20, "ber": 0, "phase_wander_deg": 5},
        "qpsk": {"evm_db": -18, "ber": 0, "acpr_db": -30},
        "8psk": {"evm_db": -16, "ser": 0},
        "qam16": {"evm_db": -16, "ser": 0},
        "ofdm": {"evm_db": -15},
        "am": {"thd_pct": 10},
        "usb": {"imd3_dbc": -25, "oss_db": 30, "carrier_dbc": -35},
        "lsb": {"imd3_dbc": -25, "oss_db": 30, "carrier_dbc": -35},
        "iq_raw": {"nmse_db": -15},
    },
    "eer": {
        "cw": {"key_timing_ms": 1.0, "leak_dbc": -40, "click_dbc": -35},
        "nbfm": {"dev_err_pct": 2.0, "sinad_db": 35},
        "wbfm": {"dev_err_pct": 2.0, "sinad_db": 30},
        "fsk": {"ber": 0, "tone_err_hz": 1.0},
        "afsk": {"ber": 0},
        "mfsk": {"ser": 0, "tone_err_hz": 0.2},
        "bpsk": {"evm_db": -26, "ber": 0, "phase_wander_deg": 5},
        "qpsk": {"evm_db": -25, "ber": 0, "acpr_db": -35},
        "8psk": {"evm_db": -24, "ser": 0},
        "qam16": {"evm_db": -24, "ser": 0},
        "ofdm": {"evm_db": -22},
        "am": {"thd_pct": 3},
        "usb": {"imd3_dbc": -30, "oss_db": 35, "carrier_dbc": -40},
        "lsb": {"imd3_dbc": -30, "oss_db": 35, "carrier_dbc": -40},
        "iq_raw": {"nmse_db": -20},
    },
}

# Outcome states.
PASS, FAIL, UNVERIFIABLE, HARDWARE_LIMIT, INFEASIBLE, INVALID, NOT_RUN = (
    "PASS", "FAIL", "UNVERIFIABLE", "HARDWARE_LIMIT", "INFEASIBLE", "INVALID", "NOT_RUN")

# Stages and their default evaluation limits per invocation.
STAGE_LIMITS = {"S0": 8, "S1": 5, "S2": 16, "S3": 8, "S4": 30, "S5": 40, "S6": 30, "S7": 25}
STAGE_NAMES = {
    "S0": "robustness (ring, FIFO, pre-roll, real-time priority, CPU)",
    "S1": "frequency (ppm)",
    "S2": "drive level (gain / eer_drive)",
    "S3": "static amplitude and AM/PM tables",
    "S4": "envelope delay",
    "S5": "quantiser and oversampling",
    "S6": "envelope scale and EER mapping",
    "S7": "silence behaviour (envelope_floor, eer_idle)",
}
PATH_STAGES = {
    "non_eer": ["S0", "S1", "S2", "S3", "S4", "S5", "S6", "S7"],
    "eer": ["S0", "S1", "S2", "S3", "S4", "S6", "S7"],
}

# Metric -> stages whose parameters can change it (closed-loop diagnosis).
DIAGNOSIS = {
    "freq_err_hz": ["S1"], "clock_ppm": ["S1"], "counters": ["S0"], "clip": ["S6"],
    "key_timing_ms": ["S0", "S4"], "leak_dbc": ["S7"], "click_dbc": ["S5", "S4"],
    "dev_err_pct": [], "sinad_db": ["S0", "S2"], "ber": ["S0", "S1", "S3", "S4", "S5", "S6"],
    "ser": ["S0", "S1", "S3", "S4", "S5", "S6"], "tone_err_hz": ["S1"],
    "evm_db": ["S3", "S4", "S5", "S6"], "phase_wander_deg": ["S1", "S3"], "acpr_db": ["S5", "S6"],
    "thd_pct": ["S3", "S6", "S5"], "imd3_dbc": ["S4", "S3", "S5", "S6"], "oss_db": ["S4", "S3"],
    "carrier_dbc": ["S3", "S7"], "nmse_db": ["S3", "S4", "S5", "S6"],
}
CONSTANT_STAGES = {"S0", "S1", "S2"}

MAX_SLOT_RATE = 200000.0


def merge_thresholds(base, override):
    out = copy.deepcopy(base)
    for k, v in override.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = merge_thresholds(out[k], v)
        else:
            out[k] = v
    return out


def classify(value, floor, limit, sense, margin):
    """Outcome of one metric. @floor is the best value the bench can resolve (None: no floor)."""
    if value is None or not np.isfinite(value):
        return INVALID
    if sense == "max":
        if floor is None or limit >= floor + margin:
            return PASS if value <= limit else FAIL
        return FAIL if (value > floor + margin and value > limit) else UNVERIFIABLE
    if floor is None or limit <= floor - margin:
        return PASS if value >= limit else FAIL
    return FAIL if (value < floor - margin and value < limit) else UNVERIFIABLE


# ---------------------------------------------------------------------------------------------------------------
# Test frames
# ---------------------------------------------------------------------------------------------------------------


class Frame:
    """A test burst: lead-in CW, gap, sync, guard, payload, guard, sync, tail CW."""

    def __init__(self, mode, fs, iq, segs, meta, args):
        self.mode = mode
        self.fs = fs
        self.iq = iq
        self.segs = segs
        self.meta = meta
        self.args = args


SYNC_CHIP_SPS = 8


def _payload(mode, fs, seconds, rng, extra):
    n = max(int(round(seconds * fs)), 64)
    t = np.arange(n) / fs
    meta = {}
    if mode == "carrier":
        x = np.ones(n, complex)
    elif mode == "cw":
        code = {"P": ".--.", "A": ".-", "R": ".-.", "I": "..", "S": "..."}
        units = []
        for ch in "PARIS":
            for s in code[ch]:
                units += [1] * (1 if s == "." else 3) + [0]
            units += [0, 0]
        units += [0] * 4
        wpm = 20.0
        unit_n = max(int(round(1.2 / wpm * fs)), 4)
        reps = int(math.ceil(n / (unit_n * len(units)))) + 1
        key = np.repeat(np.tile(units, reps), unit_n)[:n].astype(float)
        key[-int(0.03 * fs):] = 0
        w = np.hanning(max(int(0.005 * fs), 3))
        env = np.clip(np.convolve(key, w / w.sum(), mode="same"), 0, 1)
        x = env.astype(complex)
        meta["bw"] = 600.0
    elif mode in ("nbfm", "wbfm"):
        dev, tone = (2500.0, 1000.0) if mode == "nbfm" else (75000.0, 1000.0)
        if mode == "wbfm" and fs < 180000:
            raise Infeasible("wbfm needs a sample rate of at least 180 kHz")
        x = np.exp(1j * (dev / tone) * np.sin(2 * np.pi * tone * t))
        meta.update(dev=dev, tone=tone, bw=2 * (dev + 3 * tone))
    elif mode == "fsk":
        sps = max(int(round(fs / 300.0)), 4)
        nsym = max(n // sps, 32)
        bits = rng.integers(0, 2, nsym)
        shift = 200.0
        x = cpfsk(np.repeat(np.where(bits > 0, shift, -shift), sps), fs)
        meta.update(bits=bits.tolist(), sps=sps, shift=shift, bw=2 * shift + 2 * fs / sps)
    elif mode == "afsk":
        sps = max(int(round(fs / 1200.0)), 4)
        nsym = max(n // sps, 64)
        bits = rng.integers(0, 2, nsym)
        fa = np.repeat(np.where(bits > 0, 1200.0, 2200.0), sps)
        audio = np.sin(2 * np.pi * np.concatenate([[0.0], np.cumsum(fa)[:-1]]) / fs)
        x = cpfsk(3000.0 * audio, fs)
        meta.update(bits=bits.tolist(), sps=sps, bw=2 * (3000 + 2200))
    elif mode == "mfsk":
        sps = max(int(round(fs / 12.5)), 8)
        spacing = fs / sps
        nsym = max(n // sps, 6)
        syms = rng.integers(0, 8, nsym)
        base = 300.0
        x = cpfsk(np.repeat(base + syms * spacing, sps), fs)
        meta.update(syms=syms.tolist(), sps=sps, spacing=spacing, base=base, bw=base + 9 * spacing)
    elif mode == "bpsk":
        sps = max(int(round(fs / 31.25)), 8)
        nsym = max(n // sps, 16)
        a = 2.0 * rng.integers(0, 2, nsym) - 1.0
        imp = np.zeros((nsym + 2) * sps)
        imp[sps + np.arange(nsym) * sps] = a
        p = 0.5 * (1 + np.cos(np.pi * np.arange(-sps, sps + 1) / sps))
        x = np.convolve(imp, p, mode="same").astype(complex)
        meta.update(a=a.tolist(), sps=sps, first=sps, bw=2 * fs / sps)
    elif mode in ("qpsk", "8psk", "qam16"):
        sps = max(int(round(fs / 2400.0)), 4)
        beta, span = 0.35, 8
        nsym = max(n // sps, 64)
        if mode == "qpsk":
            const = np.array([1 + 1j, -1 + 1j, 1 - 1j, -1 - 1j]) / np.sqrt(2)
            syms = rng.integers(0, 4, nsym)
        elif mode == "8psk":
            const = np.exp(2j * np.pi * np.arange(8) / 8)
            syms = rng.integers(0, 8, nsym)
        else:
            lv = np.array([-3, -1, 1, 3])
            const = (lv[None, :] + 1j * lv[:, None]).ravel() / np.sqrt(10)
            syms = rng.integers(0, 16, nsym)
        h = rrc_taps(beta, sps, span)
        imp = np.zeros(nsym * sps + span * sps, complex)
        imp[np.arange(nsym) * sps] = const[syms]
        x = np.convolve(imp, h)[: len(imp)]
        scale = 1.0 / np.max(np.abs(x))
        x = x * scale
        meta.update(syms=syms.tolist(), const=[[c.real, c.imag] for c in const], sps=sps, beta=beta, span=span,
                    scale=float(scale), bw=fs / sps * (1 + beta))
    elif mode == "ofdm":
        nfft = int(round(fs / 46.875))
        nfft += nfft % 2
        cp = nfft // 8
        spacing = fs / nfft
        kmax = max(int(1200.0 / spacing), 4)
        carriers = [k for k in range(-kmax, kmax + 1) if k != 0]
        nsym = max(n // (nfft + cp), 4)
        X = (2 * rng.integers(0, 2, (nsym, len(carriers))) - 1 + 1j * (2 * rng.integers(0, 2, (nsym, len(carriers))) - 1)) / np.sqrt(2)
        blocks = []
        for i in range(nsym):
            F = np.zeros(nfft, complex)
            F[np.array(carriers) % nfft] = X[i]
            s = np.fft.ifft(F)
            blocks.append(np.concatenate([s[-cp:], s]))
        x = np.concatenate(blocks)
        scale = 1.0 / np.max(np.abs(x))
        x = x * scale
        meta.update(nfft=nfft, cp=cp, carriers=carriers, X=[[v.real, v.imag] for v in X.ravel()], nsym=nsym,
                    bw=2 * kmax * spacing)
    elif mode == "am":
        x = ((1 + 0.8 * np.sin(2 * np.pi * 1000 * t)) / 1.8).astype(complex)
        meta.update(tone=1000.0, bw=2000.0)
    elif mode in ("usb", "lsb"):
        s = 1.0 if mode == "usb" else -1.0
        x = 0.5 * (np.exp(2j * np.pi * 700 * s * t) + np.exp(2j * np.pi * 1900 * s * t))
        meta.update(f1=700 * s, f2=1900 * s, bw=3000.0)
    elif mode == "iq_raw":
        g = rng.normal(size=n) + 1j * rng.normal(size=n)
        g = band_mask_filter(g, fs, 0, 0.4 * fs)
        g *= 0.3 / np.sqrt(np.mean(np.abs(g) ** 2))
        m = np.abs(g)
        x = np.where(m > 1, g / np.maximum(m, 1e-12), g)
        meta.update(bw=0.8 * fs)
    elif mode == "staircase":
        mags = extra["mags"]
        dwell = max(int(round(extra["dwell"] * fs)), 32)
        x = np.concatenate([np.full(dwell, m, complex) for m in mags])
        meta.update(mags=list(mags), dwell=dwell, bw=200.0)
    elif mode == "pauses":
        q = max(n // 4, 64)
        tq = np.arange(q) / fs
        fade = np.clip(np.minimum(tq, tq[::-1]) / (0.2 * tq[-1]), 0, 1)
        active = 0.5 * (np.exp(2j * np.pi * 700 * tq) + np.exp(2j * np.pi * 1900 * tq)) * fade * 0.5
        noise = band_mask_filter(rng.normal(size=q) + 1j * rng.normal(size=q), fs, 300, 2700)
        noise *= 0.005 / np.sqrt(np.mean(np.abs(noise) ** 2))
        x = np.concatenate([active, noise, active, noise])
        meta.update(q=q, bw=3000.0)
    else:
        raise ValueError("unknown mode %s" % mode)
    return x.astype(np.complex128), meta


def build_frame(mode, fs, seconds, seed, extra=None):
    """Builds the test burst of @mode at sample rate @fs (deterministic for a given @seed)."""
    extra = extra or {}
    rng = np.random.default_rng(seed)
    payload, meta = _payload(mode, fs, seconds, rng, extra)
    sync = msk(m_sequence(127), SYNC_CHIP_SPS)
    lead = np.ones(int(0.05 * fs), complex)
    gap = np.zeros(int(0.01 * fs), complex)
    guard = np.ones(int(0.005 * fs), complex)
    tail = np.ones(int(0.02 * fs), complex)
    parts = [("lead", lead), ("gap", gap), ("sync1", sync), ("guard1", guard), ("payload", payload),
             ("guard2", guard), ("sync2", sync), ("tail", tail)]
    segs, pos = {}, 0
    for name, p in parts:
        segs[name] = (pos, pos + len(p))
        pos += len(p)
    iq = np.concatenate([p for _, p in parts]).astype(np.complex64)
    args = dict(mode=mode, fs=fs, seconds=seconds, seed=seed, extra=extra)
    return Frame(mode, fs, iq, segs, meta, args)


# ---------------------------------------------------------------------------------------------------------------
# Generic analysis: synchronisation, frequency / clock estimation and reference fit
# ---------------------------------------------------------------------------------------------------------------


def analysis_os(fs, rx_rate):
    """Oversampling of the analysis rate relative to the transmit rate (1-4)."""
    os_ = int(min(4, math.floor(0.8 * rx_rate / fs)))
    if os_ < 1:
        raise AnalysisError("the receiver rate must be at least 1.25 times the transmit rate")
    return os_


def _detect_on(y, rate, lead):
    w = max(int(0.002 * rate), 1)
    sm = np.convolve(np.abs(y), np.ones(w) / w, mode="same")
    nf = max(int(0.8 * lead * rate), w * 4)
    if nf >= len(sm) - w:
        raise AnalysisError("capture too short")
    floor = float(np.median(sm[:nf]))
    peak = float(np.percentile(sm[nf:], 99))
    if not peak > 4 * floor + 1e-12:
        raise AnalysisError("no signal above the receiver noise")
    idx = np.nonzero(sm[nf:] > floor + 0.3 * (peak - floor))[0]
    if not len(idx):
        raise AnalysisError("no signal above the receiver noise")
    return nf + int(idx[0]), nf


def _coarse_freq(seg, rate):
    if len(seg) < 16:
        raise AnalysisError("lead-in carrier too short")
    nf = next_pow2(16 * len(seg))
    X = np.abs(np.fft.fft(seg * np.hanning(len(seg)), nf))
    f = np.fft.fftfreq(nf, 1.0 / rate)
    k = int(np.argmax(X))
    f0 = float(f[k])
    n = np.arange(len(seg))
    ph = np.unwrap(np.angle(seg * np.exp(-2j * np.pi * f0 * n / rate)))
    return f0 + float(np.polyfit(n, ph, 1)[0]) * rate / (2 * np.pi)


def _find_sync(y, ref, center, half, chip):
    a = max(0, int(center - half))
    b = min(len(y), int(center + half) + len(ref))
    if b - a < len(ref) + 2:
        raise AnalysisError("sync sequence outside the capture")
    c = xcorr_mag(y[a:b], ref)
    i = int(np.argmax(c))
    excl = np.ones(len(c), bool)
    excl[max(0, i - 2 * chip): i + 2 * chip + 1] = False
    side = float(np.max(c[excl])) if np.any(excl) else 0.0
    if side > 0 and c[i] / side < 2.0:
        raise AnalysisError("sync sequence not found")
    return a + parabolic(c, i)


def _fine_delay(y, r):
    """Delay of @y relative to @r in samples (|delay| well below one sample), from the cross-spectrum phase."""
    n = len(r)
    w = np.hanning(n)
    c = np.fft.fft(y * w) * np.conj(np.fft.fft(r * w))
    k = np.fft.fftfreq(n)
    m = np.abs(c) > 0.02 * np.max(np.abs(c))
    c = c * np.exp(-1j * np.angle(np.sum(c[m])))
    wt = np.sqrt(np.abs(c[m]))
    A = np.stack([k[m], np.ones(np.count_nonzero(m))], axis=1) * wt[:, None]
    sol, *_ = np.linalg.lstsq(A, np.angle(c[m]) * wt, rcond=None)
    return float(-sol[0] / (2 * np.pi))


def analyze(cap, frame, rx_rate, rx_offset, rx_lead):
    """Aligns a capture with its frame; returns the corrected received signal and the reference fit."""
    fs = frame.fs
    L = len(frame.iq)
    OS = analysis_os(fs, rx_rate)
    fa = OS * fs
    cap = np.asarray(cap, np.complex128)
    if len(cap) < int((rx_lead * 0.5 + L / fs) * rx_rate):
        raise AnalysisError("capture too short")
    t = np.arange(len(cap)) / rx_rate
    base = cap * np.exp(-2j * np.pi * rx_offset * t)
    y1, fa1 = resample_rate(base, rx_rate, fa)
    on, noise_end = _detect_on(y1, fa1, rx_lead)
    f1 = _coarse_freq(y1[on + int(0.004 * fa1): on + int(0.045 * fa1)], fa1)
    y1 = y1 * np.exp(-2j * np.pi * f1 * np.arange(len(y1)) / fa1)
    ref_fa = fft_resample(frame.iq.astype(np.complex128), L * OS)
    s1a, s1b = frame.segs["sync1"]
    s2a = frame.segs["sync2"][0]
    sync_ref = ref_fa[s1a * OS: s1b * OS]
    chip = SYNC_CHIP_SPS * OS
    p1 = _find_sync(y1, sync_ref, on + s1a * OS, int(0.03 * fa1), chip)
    dist = (s2a - s1a) * OS
    p2 = _find_sync(y1, sync_ref, p1 + dist, int(3e-4 * dist) + 4 * chip, chip)
    eps = (p2 - p1) / dist - 1.0

    # Second pass at a rate that undoes the clock difference, so that one received sample is one reference sample.
    # The delays of the two sync words, measured on the cross spectrum, refine the clock until they agree.
    base = base * np.exp(-2j * np.pi * f1 * t)
    s2b = frame.segs["sync2"][1]
    for it in range(3):
        y2, fa2 = resample_rate(base, rx_rate, fa / (1.0 + eps))
        on2 = int(on * fa2 / fa1)
        p = _find_sync(y2, sync_ref, on2 + s1a * OS, int(0.01 * fa2) + 4 * chip, chip)
        start = p - s1a * OS
        pad = 8 * OS
        i0 = int(math.floor(start)) - pad
        if i0 < 0 or i0 + L * OS + 2 * pad > len(y2):
            raise AnalysisError("frame not fully captured")
        seg = frac_shift(y2[i0: i0 + L * OS + 2 * pad], -(start - math.floor(start)))
        y = seg[pad: pad + L * OS]
        d1 = _fine_delay(y[s1a * OS: s1b * OS], sync_ref)
        d2 = _fine_delay(y[s2a * OS: s2b * OS], sync_ref)
        if abs(d2 - d1) < 0.005 or it == 2:
            break
        eps = (1.0 + eps) * (1.0 + (d2 - d1) / dist) - 1.0
    eps = (1.0 + eps) * (1.0 + (d2 - d1) / dist) - 1.0
    y = frac_shift(y, -0.5 * (d1 + d2))
    noise = y2[: int(noise_end * fa2 / fa1)]
    n0 = float(np.mean(np.abs(noise) ** 2)) / fa2

    # Residual frequency and phase from the samples where the reference is strong.
    z = y * np.conj(ref_fa)
    w = (np.abs(ref_fa) > 0.3).astype(float)
    B = max(8 * OS, int(0.002 * fa))
    nb = len(z) // B
    zs = (z[: nb * B] * w[: nb * B]).reshape(nb, B).sum(axis=1)
    ws = w[: nb * B].reshape(nb, B).sum(axis=1)
    keep = ws > 0.25 * B
    if np.count_nonzero(keep) < 4:
        raise AnalysisError("not enough carrier for the phase fit")
    centers = (np.arange(nb) + 0.5) * B
    ph = np.unwrap(np.angle(zs[keep]))
    slope, icpt = np.polyfit(centers[keep], ph, 1, w=np.sqrt(ws[keep]))
    y = y * np.exp(-1j * (slope * np.arange(len(y)) + icpt))
    f_err = f1 + slope * fa2 / (2 * np.pi)

    # Short linear filter that absorbs the passband of the receiver and the residual delay.
    T = 5
    c = T // 2
    g0, g1 = frame.segs["gap"]
    rows = np.arange(T, L * OS - T)
    rows = rows[(rows < g0 * OS - T) | (rows >= g1 * OS + T)]
    if len(rows) > 60000:
        # A random subset: a fixed stride can lock onto the slot grid, where the quantisation error differs.
        rows = np.sort(np.random.default_rng(len(rows)).choice(rows, 60000, replace=False))
    shifts = [np.roll(ref_fa, j - c) for j in range(T)]
    A = np.stack([s[rows] for s in shifts], axis=1)
    h, *_ = np.linalg.lstsq(A, y[rows], rcond=None)
    yhat = sum(h[j] * shifts[j] for j in range(T))
    g = complex(np.sum(h))
    if abs(g) < 1e-12:
        raise AnalysisError("no signal")
    y = y / g
    yhat = yhat / g
    n0 /= abs(g) ** 2
    y_fs = fft_resample(y, L)
    yhat_fs = fft_resample(yhat, L)
    la, lb = frame.segs["lead"]
    p_cw = float(np.mean(np.abs(y_fs[la + int(0.01 * fs): lb - int(0.005 * fs)]) ** 2))
    return dict(fs=fs, OS=OS, fa=fa, y_fa=y, ref_fa=ref_fa, yhat_fa=yhat, y_fs=y_fs, yhat_fs=yhat_fs,
                f_err=float(f_err), clock_ppm=float(-eps * 1e6), n0=n0, gain=g, power_db=20 * math.log10(abs(g)),
                p_cw=p_cw, snr_db=db10(p_cw / max(n0 * fs, 1e-30)))


# ---------------------------------------------------------------------------------------------------------------
# Mode metrics. Each returns ({name: (value, floor)}, extra) with values in the units of METRICS.
# ---------------------------------------------------------------------------------------------------------------


def _spur_floor(sfdr, noise_power, ref_power):
    """Smallest product that can be seen next to a strong signal: receiver SFDR plus the noise in the window."""
    return db10(10 ** (-sfdr / 10.0) + noise_power / max(ref_power, 1e-30))


def _off_floor(noise_power, n, ref_power):
    """Smallest power that can be detected while the signal is off: the uncertainty of the subtracted noise."""
    return db10(noise_power * max(3.0 / math.sqrt(max(n, 1)), 0.01) / max(ref_power, 1e-30))


def _pay(A, frame, trim=0.0):
    a, b = frame.segs["payload"]
    k = int(trim * frame.fs)
    return a + k, b - k


def metrics_common(A, frame, sfdr):
    fs = frame.fs
    a, b = _pay(A, frame)
    y, yh = A["y_fs"][a:b], A["yhat_fs"][a:b]
    p = float(np.mean(np.abs(yh) ** 2))
    nmse = db10(np.sum(np.abs(y - yh) ** 2) / max(np.sum(np.abs(yh) ** 2), 1e-30))
    out = {"nmse_db": (nmse, db10(A["n0"] * fs / max(p, 1e-30) + 10 ** (-sfdr / 10.0)))}
    extra = {}
    if MODE_AMPLITUDE[frame.mode] == "envelope":
        r = np.abs(frame.iq[a:b])
        edges = np.linspace(0.05, 1.0, 17)
        amam, ampm = [], []
        for lo, hi in zip(edges[:-1], edges[1:]):
            m = (r >= lo) & (r < hi)
            if np.count_nonzero(m) > 32:
                amam.append(float(np.mean(np.abs(y[m])) / max(np.mean(np.abs(yh[m])), 1e-12) - 1.0))
                ampm.append(float(np.degrees(np.angle(np.sum(y[m] * np.conj(yh[m]))))))
        extra["amam_err_pct"] = 100 * max(np.abs(amam)) if amam else 0.0
        extra["ampm_err_deg"] = max(np.abs(ampm)) if ampm else 0.0
        d = np.gradient(np.abs(yh))
        ae = np.abs(y) - np.abs(yh)
        den = float(np.sum(d * d))
        extra["delay_est"] = float(-np.sum(ae * d) / den) if den > 0 else 0.0
    return out, extra


def metrics_carrier(A, frame, sfdr):
    return {}, {"power_db": A["power_db"]}


def metrics_cw(A, frame, sfdr):
    fs = frame.fs
    a, b = _pay(A, frame)
    y = A["y_fs"][a:b]
    r = np.abs(frame.iq[a:b])
    w = max(int(0.001 * fs), 1)
    ey = np.convolve(np.abs(y), np.ones(w) / w, mode="same")
    er = np.convolve(r, np.ones(w) / w, mode="same")

    def edges(e):
        s = (e > 0.5).astype(int)
        d = np.diff(s)
        return np.nonzero(d > 0)[0], np.nonzero(d < 0)[0]

    ry, fy = edges(ey)
    rr, fr = edges(er)
    worst = 0.0
    for ref, got in ((rr, ry), (fr, fy)):
        for e in ref:
            dmin = np.min(np.abs(got - e)) if len(got) else 10 * fs
            worst = max(worst, float(dmin))
    timing = min(worst / fs * 1000.0, 1000.0)
    up = r < 1e-4
    k = int(0.004 * fs)
    upm = up.copy()
    for s in range(1, k + 1):
        upm[s:] &= up[:-s]
        upm[:-s] &= up[s:]
    down = r > 0.99
    p_down = float(np.mean(np.abs(y[down]) ** 2)) if np.any(down) else 1.0
    out = {"key_timing_ms": (timing, None)}
    nfs = A["n0"] * fs
    if np.count_nonzero(upm) > 32:
        p_up = float(np.mean(np.abs(y[upm]) ** 2))
        out["leak_dbc"] = (db10(max(p_up - nfs, 1e-15 * p_down) / p_down), _off_floor(nfs, np.count_nonzero(upm), p_down))
    psd, f = periodogram(y, fs)
    p_in = band_power(psd, f, -300, 300)
    hi = min(3000.0, 0.45 * fs)
    p_out = band_power(psd, f, 500, hi) + band_power(psd, f, -hi, -500)
    out["click_dbc"] = (db10(p_out / max(p_in, 1e-30)), _spur_floor(sfdr, A["n0"] * 2 * (hi - 500), p_in))
    return out, {}


def metrics_fm(A, frame, sfdr):
    fs = frame.fs
    a, b = _pay(A, frame, 0.01)
    f = inst_freq(A["y_fs"][a:b], fs)
    dev, tone = frame.meta["dev"], frame.meta["tone"]
    amps, fit = sine_fit(f, fs, tone)
    res = (f - fit).astype(complex)
    hi = min(3000.0 if frame.mode == "nbfm" else 15000.0, 0.45 * fs)
    res = band_mask_filter(res, fs, 300 if frame.mode == "nbfm" else 50, hi).real
    p_res = float(np.mean(res ** 2))
    p_tone = amps[0] ** 2 / 2
    return {"dev_err_pct": (abs(amps[0] / dev - 1.0) * 100, None),
            "sinad_db": (db10((p_tone + p_res) / max(p_res, 1e-30)), None)}, {}


def metrics_fsk(A, frame, sfdr):
    fs = frame.fs
    a, _ = _pay(A, frame)
    y = A["y_fs"]
    sps, shift = frame.meta["sps"], frame.meta["shift"]
    bits = np.array(frame.meta["bits"])
    fm = []
    for k in range(len(bits)):
        s = y[a + k * sps + sps // 4: a + k * sps + 3 * sps // 4]
        fm.append(mean_freq(s, fs))
    fm = np.array(fm)
    sel = slice(1, len(bits) - 1)
    ber = float(np.mean((fm[sel] > 0).astype(int) != bits[sel]))
    errs = []
    if np.any(bits[sel] == 1):
        errs.append(abs(np.mean(fm[sel][bits[sel] == 1]) - shift))
    if np.any(bits[sel] == 0):
        errs.append(abs(np.mean(fm[sel][bits[sel] == 0]) + shift))
    return {"ber": (ber, None), "tone_err_hz": (max(errs) if errs else 0.0, None)}, {}


def metrics_afsk(A, frame, sfdr):
    fs = frame.fs
    a, _ = _pay(A, frame)
    y = A["y_fs"]
    sps = frame.meta["sps"]
    bits = np.array(frame.meta["bits"])
    audio = np.concatenate([[0.0], inst_freq(y[a: a + len(bits) * sps + 1], fs)])
    n = np.arange(sps)
    em = np.exp(-2j * np.pi * 1200 * n / fs)
    es = np.exp(-2j * np.pi * 2200 * n / fs)
    lo, hi = sps // 10, sps - sps // 10
    got = []
    for k in range(len(bits)):
        s = audio[k * sps: (k + 1) * sps]
        got.append(1 if abs(np.sum(s[lo:hi] * em[lo:hi])) > abs(np.sum(s[lo:hi] * es[lo:hi])) else 0)
    got = np.array(got)
    sel = slice(1, len(bits) - 1)
    return {"ber": (float(np.mean(got[sel] != bits[sel])), None)}, {}


def metrics_mfsk(A, frame, sfdr):
    fs = frame.fs
    a, _ = _pay(A, frame)
    y = A["y_fs"]
    m = frame.meta
    sps, spacing, base = m["sps"], m["spacing"], m["base"]
    syms = np.array(m["syms"])
    fm = np.array([mean_freq(y[a + k * sps + sps // 10: a + (k + 1) * sps - sps // 10], fs) for k in range(len(syms))])
    expect = base + syms * spacing
    got = np.clip(np.round((fm - base) / spacing), 0, 7).astype(int)
    return {"ser": (float(np.mean(got != syms)), None),
            "tone_err_hz": (float(np.sqrt(np.mean((fm - expect) ** 2))), None)}, {}


def metrics_bpsk(A, frame, sfdr):
    fs = frame.fs
    a, _ = _pay(A, frame)
    y = A["y_fs"]
    m = frame.meta
    sps, first = m["sps"], m["first"]
    ref = np.array(m["a"])
    w = max(sps // 16, 1)
    c = a + first + np.arange(len(ref)) * sps
    s = np.array([np.mean(y[i - w: i + w + 1]) for i in c])
    g = np.sum(s * ref) / np.sum(ref * ref)
    e = s - g * ref
    evm = db10(np.sum(np.abs(e) ** 2) / np.sum(np.abs(g * ref) ** 2))
    ber = float(np.mean(np.sign((s / g).real) != ref))
    blk = 8
    phs = [np.degrees(np.angle(np.sum(s[i: i + blk] * np.conj(g * ref[i: i + blk])))) for i in range(0, len(ref) - blk + 1, blk)]
    floor = db10(A["n0"] * fs / (2 * w + 1) + 10 ** (-sfdr / 10.0))
    return {"evm_db": (evm, floor), "ber": (ber, None), "phase_wander_deg": (float(np.max(np.abs(phs))) if phs else 0.0, None)}, {}


def _acpr(A, frame, bw, sfdr):
    a, b = _pay(A, frame)
    OS, fa = A["OS"], A["fa"]
    psd, f = periodogram(A["y_fa"][a * OS: b * OS], fa)
    main = band_power(psd, f, -bw / 2, bw / 2)
    lo, hi = 0.6 * bw, 1.6 * bw
    if hi > 0.48 * fa:
        return None
    adj = max(band_power(psd, f, lo, hi), band_power(psd, f, -hi, -lo))
    return db10(adj / max(main, 1e-30)), _spur_floor(sfdr, A["n0"] * (hi - lo), main)


def metrics_linear(A, frame, sfdr):
    fs = frame.fs
    a, b = _pay(A, frame)
    m = frame.meta
    sps, span = m["sps"], m["span"]
    const = np.array([complex(*c) for c in m["const"]])
    syms = np.array(m["syms"])
    h = rrc_taps(m["beta"], sps, span)
    mf = np.convolve(A["y_fs"][a:b], h)
    mr = np.convolve(frame.iq[a:b].astype(np.complex128), h)
    D = span * sps
    idx = D + np.arange(len(syms)) * sps
    ok = idx < len(mf)
    sel = np.arange(len(syms))[ok][span: -span]
    s = mf[idx[sel]]
    # The matched-filtered reference holds the residual ISI of the truncated filters, so it is the ideal point.
    ref = mr[idx[sel]]
    ideal = const[syms[sel]]
    gr = np.sum(s * np.conj(ref)) / np.sum(np.abs(ref) ** 2)
    evm = db10(np.sum(np.abs(s - gr * ref) ** 2) / np.sum(np.abs(gr * ref) ** 2))
    g = np.sum(s * np.conj(ideal)) / np.sum(np.abs(ideal) ** 2)
    dec = np.argmin(np.abs((s / g)[:, None] - const[None, :]), axis=1)
    ser = float(np.mean(dec != syms[sel]))
    floor = db10(A["n0"] * fs / (np.mean(np.abs(gr * ref) ** 2)) + 10 ** (-sfdr / 10.0))
    out = {"evm_db": (evm, floor)}
    if frame.mode == "qpsk":
        bits_tx = np.stack([const[syms[sel]].real < 0, const[syms[sel]].imag < 0], axis=1)
        bits_rx = np.stack([(s / g).real < 0, (s / g).imag < 0], axis=1)
        out["ber"] = (float(np.mean(bits_tx != bits_rx)), None)
        ac = _acpr(A, frame, m["bw"], sfdr)
        if ac is not None:
            out["acpr_db"] = ac
    else:
        out["ser"] = (ser, None)
    return out, {}


def metrics_ofdm(A, frame, sfdr):
    fs = frame.fs
    a, _ = _pay(A, frame)
    m = frame.meta
    nfft, cp, nsym = m["nfft"], m["cp"], m["nsym"]
    car = np.array(m["carriers"]) % nfft
    X = np.array([complex(*v) for v in m["X"]]).reshape(nsym, len(car))
    Y = np.zeros_like(X)
    for i in range(nsym):
        s = a + i * (nfft + cp) + cp
        Y[i] = np.fft.fft(A["y_fs"][s: s + nfft])[car]
    H = np.sum(Y * np.conj(X), axis=0) / np.sum(np.abs(X) ** 2, axis=0)
    E = Y - H[None, :] * X
    evm = db10(np.sum(np.abs(E) ** 2) / np.sum(np.abs(H[None, :] * X) ** 2))
    floor = db10(A["n0"] * fs * nfft / np.mean(np.abs(H[None, :] * X) ** 2) + 10 ** (-sfdr / 10.0))
    out = {"evm_db": (evm, floor)}
    return out, {}


def metrics_am(A, frame, sfdr):
    fs = frame.fs
    a, b = _pay(A, frame, 0.01)
    env = np.abs(A["y_fs"][a:b])
    amps, _ = sine_fit(env, fs, frame.meta["tone"], harmonics=5)
    thd = 100.0 * math.sqrt(sum(x * x for x in amps[1:])) / max(amps[0], 1e-12)
    return {"thd_pct": (thd, None)}, {}


def metrics_ssb(A, frame, sfdr):
    fs = frame.fs
    a, b = _pay(A, frame, 0.01)
    psd, f = periodogram(A["y_fs"][a:b], fs)
    f1, f2 = frame.meta["f1"], frame.meta["f2"]
    pt = 0.5 * (tone_power(psd, f, f1) + tone_power(psd, f, f2))
    imd = max(tone_power(psd, f, 2 * f1 - f2), tone_power(psd, f, 2 * f2 - f1))
    opp = 0.5 * (tone_power(psd, f, -f1) + tone_power(psd, f, -f2))
    car = tone_power(psd, f, 0.0)
    nb = A["n0"] * 7.0 * (f[1] - f[0])
    # Offset tuning keeps the image and the DC spike of the receiver away from the mirror and the carrier
    # frequencies, so only the noise limits those two measurements; intermodulation is limited by the SFDR too.
    fl = _spur_floor(sfdr, nb, pt)
    fn = _spur_floor(200.0, nb, pt)
    return {"imd3_dbc": (db10(imd / pt), fl), "oss_db": (db10(pt / max(opp, 1e-30)), -fn),
            "carrier_dbc": (db10(car / pt), fn)}, {}


def metrics_staircase(A, frame, sfdr):
    """Complex mean of every step (20 % guard on each side), relative to the full-scale steps around it."""
    a, _ = _pay(A, frame)
    m = frame.meta
    dwell = m["dwell"]
    g = int(0.2 * dwell)
    vals = np.array([np.mean(A["y_fs"][a + k * dwell + g: a + (k + 1) * dwell - g]) for k in range(len(m["mags"]))])
    amps, phases = [], []
    for i in range(1, len(vals), 2):
        refs = [vals[i - 1]] + ([vals[i + 1]] if i + 1 < len(vals) else [])
        ref = np.mean(refs)
        amps.append(float(np.abs(vals[i]) / np.mean(np.abs(refs))))
        phases.append(float(np.degrees(np.angle(vals[i] / ref))))
    return {}, {"amps": amps, "phases": phases}


def metrics_pauses(A, frame, sfdr):
    fs = frame.fs
    a, _ = _pay(A, frame)
    q = frame.meta["q"]
    y, yh = A["y_fs"], A["yhat_fs"]
    act = np.concatenate([np.arange(a, a + q), np.arange(a + 2 * q, a + 3 * q)])
    k = int(0.005 * fs)
    pau = np.concatenate([np.arange(a + q + k, a + 2 * q - k), np.arange(a + 3 * q + k, a + 4 * q - k)])
    p_act = float(np.mean(np.abs(yh[act]) ** 2))
    p_pau = float(np.mean(np.abs(y[pau]) ** 2))
    nfs = A["n0"] * fs
    leak = db10(max(p_pau - nfs, 1e-12 * p_act) / p_act)
    nmse = db10(np.sum(np.abs(y[act] - yh[act]) ** 2) / np.sum(np.abs(yh[act]) ** 2))
    return {"leak_dbc": (leak, _off_floor(nfs, len(pau), p_act))}, {"active_nmse_db": nmse}


MODE_METRICS = {
    "carrier": metrics_carrier, "cw": metrics_cw, "nbfm": metrics_fm, "wbfm": metrics_fm, "fsk": metrics_fsk,
    "afsk": metrics_afsk, "mfsk": metrics_mfsk, "bpsk": metrics_bpsk, "qpsk": metrics_linear,
    "8psk": metrics_linear, "qam16": metrics_linear, "ofdm": metrics_ofdm, "am": metrics_am, "usb": metrics_ssb,
    "lsb": metrics_ssb, "iq_raw": lambda A, f, s: ({}, {}), "staircase": metrics_staircase,
    "pauses": metrics_pauses,
}


def measure_frame(cap, frame, rx_rate, rx_offset, rx_lead, sfdr):
    """Analyses one capture of @frame; returns (metrics, extra)."""
    A = analyze(cap, frame, rx_rate, rx_offset, rx_lead)
    met, extra = metrics_common(A, frame, sfdr)
    m2, e2 = MODE_METRICS[frame.mode](A, frame, sfdr)
    met.update(m2)
    extra.update(e2)
    extra.update(f_err=A["f_err"], clock_ppm=A["clock_ppm"], snr_db=A["snr_db"], power_db=A["power_db"])
    return met, extra


# ---------------------------------------------------------------------------------------------------------------
# Profiles
# ---------------------------------------------------------------------------------------------------------------

COMMON_SETTINGS = ["ppm", "ppm_ntp", "harmonic", "iq_scale", "envelope_delay", "envelope_floor"]
NON_EER_SETTINGS = ["amplitude_lut", "ampm_lut", "amplitude_quantizer", "sigma_delta_order", "rfdac_lut"]
EER_SETTINGS = ["eer_lut", "eer_ampm_lut", "eer_gain", "eer_offset", "eer_idle", "eer_drive"]


def default_profile(path, a):
    s = dict(ppm=a.ppm, ppm_ntp=True, harmonic=a.harmonic, iq_scale=1.0, envelope_delay=0.5, envelope_floor=0.0)
    stream = dict(fifo_ms=250, preroll_ms=100, rt_priority=50, cpu=-1, slots_per_sample=1)
    stream.update(parse_kwargs(a.stream_args, "--stream-args"))
    if path == "non_eer":
        s.update(amplitude_lut="", ampm_lut="", amplitude_quantizer="nearest", sigma_delta_order=2, rfdac_lut="")
        stream.update(envelope_output="none", rf_dac=False)
    else:
        s.update(eer_lut="", eer_ampm_lut="", eer_gain=1.0, eer_offset=0.0, eer_idle=0.0, eer_drive=7)
        stream.update(envelope_output=a.envelope_output)
        if a.envelope_output == "pwm":
            stream.update(pwm_gpio=a.pwm_gpio, pwm_bits=a.pwm_bits)
    return dict(path=path, settings=s, stream=stream, gain_envelope=7, gain_constant=7, rx_ppm=a.rx_ppm,
                notes=[])


def profile_settings(profile, mode):
    """Settings to write for a burst of @mode (the amplitude mode follows the signal)."""
    keys = COMMON_SETTINGS + (NON_EER_SETTINGS if profile["path"] == "non_eer" else EER_SETTINGS)
    s = {k: profile["settings"][k] for k in keys if k in profile["settings"]}
    s["amplitude_mode"] = MODE_AMPLITUDE[mode]
    return s


def to_text(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, float):
        return "%.10g" % v
    return str(v)


def profile_lines(profile):
    """Device arguments (scalars), list settings and stream arguments of a profile, as text."""
    keys = COMMON_SETTINGS + (NON_EER_SETTINGS if profile["path"] == "non_eer" else EER_SETTINGS)
    scal, lists = ["driver=rpitx"], []
    for k in keys:
        v = profile["settings"].get(k)
        if v is None or v == "":
            continue
        if isinstance(v, str) and "," in v:
            lists.append("%s=%s" % (k, v))
        else:
            scal.append("%s=%s" % (k, to_text(v)))
    stream = ",".join("%s=%s" % (k, to_text(v)) for k, v in sorted(profile["stream"].items()))
    return ",".join(scal), lists, stream


# ---------------------------------------------------------------------------------------------------------------
# Simulated bench: a model of the driver, the output stage and an RTL-SDR-like receiver
# ---------------------------------------------------------------------------------------------------------------

SIM_SCENARIOS = {
    "good": {},
    "bad": {"pad_amp": [0.05, 0.3, 0.33, 0.6, 0.62, 0.9, 0.92, 1.0], "pad_ph": [40, 30, 28, 15, 14, 4, 3, 0],
            "tx_phase_noise_deg": 4.0},
    "noisy": {"rx_noise_dbfs": -25.0},
    "robust-bad": {"min_fifo_ms": 400, "min_rt": 70},
}


class SimBench:
    """Transmitter + receiver model with hidden impairments. All randomness comes from the burst identifier."""

    DRIVER_AMPM_SIGN = 1.0  # the driver adds the AM/PM table to the carrier phase

    def __init__(self, a, scenario="good"):
        t = dict(xo_ppm=2.0, rx_xo_ppm=-1.5,
                 pad_amp=[0.12, 0.27, 0.42, 0.56, 0.69, 0.80, 0.90, 1.0],
                 pad_ph=[14.0, 11.0, 8.0, 6.0, 4.0, 2.5, 1.0, 0.0],
                 true_delay=0.8, rfdac_w=[0.66, 0.36, 1.0],
                 eer_gamma=1.25, eer_feed=0.01, eer_offset=0.03, eer_lpf_hz=40000.0, eer_delay=2.5, eer_ph_deg=10.0,
                 rx_noise_dbfs=-45.0, adc_bits=8, iq_eps=0.003, dc=0.01, rx_cubic=0.01,
                 min_fifo_ms=0, min_preroll_ms=0, min_rt=0, eer_hardware=True, tx_phase_noise_deg=0.0,
                 band_ref_hz=144.39e6, band_scaling=True)
        t.update(SIM_SCENARIOS[scenario])
        self.truth = t
        self.freq = a.freq
        self.rx_rate = a.rx_rate
        self.rx_offset = a.rx_offset
        self.rx_lead, self.rx_tail = 0.05, 0.04
        self.rx_gain = 30.0
        self.rx_corr = 0.0
        self.fs = a.rate
        self.profile = None
        self.mode = None
        self.duty = 1.0
        self.log = []
        self.a = a
        self.forced = None

    # -- receiver controls
    def rx_gain_range(self):
        return 0.0, 49.6

    def set_rx_gain(self, g):
        self.rx_gain = float(g)

    def set_rx_ppm(self, ppm):
        self.rx_corr = float(ppm)

    def set_rx_freq(self, f):
        self.rx_freq = f

    def close(self):
        pass

    # -- transmitter controls
    def set_freq(self, f):
        self.freq = f

    def use_driver_profile(self, path):
        """Emulates profile=<path>: the file replaces the settings, the default stream arguments and the gain."""
        self.forced = profile_from_driver_file(path, self.a) if path else None

    def configure(self, profile, mode):
        if self.forced is not None:
            profile = self.forced
        s = profile_settings(profile, mode)
        st = profile["stream"]
        slots = int(st.get("slots_per_sample", 1))
        if not 1 <= slots <= 4 or self.fs * slots > MAX_SLOT_RATE:
            raise Infeasible("slots_per_sample %d not possible at %g S/s" % (slots, self.fs))
        if not 0 <= float(s["iq_scale"]) <= 100:
            raise Infeasible("iq_scale out of range")
        if abs(float(s["envelope_delay"])) > 32:
            raise Infeasible("envelope_delay out of range")
        for key in ("amplitude_lut", "eer_lut"):
            if s.get(key):
                v = parse_list(s[key])
                if any(v[i] <= v[i - 1] for i in range(1, len(v))):
                    raise Infeasible("%s must be strictly increasing" % key)
        if profile["path"] == "eer":
            if not 0 < float(s["eer_gain"]) <= 10 or not -1 <= float(s["eer_offset"]) <= 1:
                raise Infeasible("EER mapping out of range")
        self.profile = profile
        self.mode = mode

    def sensors_sample_rate(self):
        return float(self.fs)

    def _states(self, s, drive, dual):
        lut = parse_list(s.get("amplitude_lut", "")) or [(l + 1) / 8.0 for l in range(8)]
        ampm = parse_list(s.get("ampm_lut", "")) or [0.0] * 8
        w = parse_list(s.get("rfdac_lut", "")) or [2 / 3.0, 1 / 3.0, 1.0]
        maxw = max(w) if dual else 1.0
        ref = lut[drive] * maxw
        st = [(0.0, -1, 0, 0.0)]
        for l in range(drive + 1):
            for mask in ((1, 2, 3) if dual else (1,)):
                st.append((lut[l] * (w[mask - 1] if dual else 1.0) / ref, l, mask, math.radians(ampm[l])))
        st = [st[0]] + sorted(st[1:], key=lambda x: x[0])
        return st

    def _tx(self, iq, rng):
        """Complex envelope at the antenna, at the slot rate, plus the driver counters."""
        p = self.profile
        s = profile_settings(p, self.mode)
        st = p["stream"]
        t = self.truth
        slots = int(st.get("slots_per_sample", 1))
        rate = self.fs * slots
        n = len(iq) * slots
        xi = np.interp(np.arange(n) / slots, np.arange(len(iq)), iq.real) + 1j * np.interp(np.arange(n) / slots, np.arange(len(iq)), iq.imag)
        raw = np.abs(xi)
        on_in = raw >= 1e-3
        ph = np.angle(xi)
        idx = np.where(on_in, np.arange(n), 0)
        np.maximum.accumulate(idx, out=idx)
        ph = ph[idx]
        constant = s["amplitude_mode"] == "constant"
        counters = dict(underflows=0, late=0, dma_errors=0, envelope_late=0, envelope_clip=0)
        eer = p["path"] == "eer"
        hang = max(1, int(0.002 * rate))
        floor = float(s.get("envelope_floor", 0.0))
        if eer:
            delay = (t["eer_delay"] - float(s["envelope_delay"])) * slots
        else:
            delay = (t["true_delay"] - float(s["envelope_delay"])) * slots
        a = np.interp(np.arange(n) - delay, np.arange(n), raw) * float(s["iq_scale"])
        if constant:
            on = on_in
        else:
            below = a < floor if floor > 0 else np.zeros(n, bool)
            run = np.zeros(n, int)
            c = 0
            for i in range(n):
                c = c + 1 if below[i] else 0
                run[i] = c
            on = run < hang
        extra_ph = np.zeros(n)
        if not eer:
            drive = int(p["gain_constant"] if constant else p["gain_envelope"])
            dual = bool(st.get("rf_dac", False))
            states = self._states(s, drive, dual)
            amps = np.array([x[0] for x in states])
            thr = 0.5 * (amps[:-1] + amps[1:])
            if constant:
                q = np.where(on, len(states) - 1, 0)
            else:
                counters["envelope_clip"] = int(np.count_nonzero(a > 1.0))
                if s["amplitude_quantizer"] == "sigma_delta":
                    order = int(s["sigma_delta_order"])
                    q = np.zeros(n, int)
                    e1 = e2 = 0.0
                    thl, aml, al, onl = thr.tolist(), amps.tolist(), a.tolist(), on.tolist()
                    for i in range(n):
                        if not onl[i]:
                            e1 = e2 = 0.0
                            continue
                        v = al[i] + (2 * e1 - e2 if order >= 2 else e1)
                        k = bisect.bisect_right(thl, v) if v > 0 else 0
                        e = min(max(v - aml[k], -1.0), 1.0)
                        e2, e1 = e1, e
                        q[i] = k
                else:
                    q = np.where(a > 0, np.searchsorted(thr, a, side="right"), 0)
                    q = np.where(on, q, 0)
            lvl = np.array([x[1] for x in states])[q]
            mask = np.array([x[2] for x in states])[q]
            comp = np.array([x[3] for x in states])[q]
            pa = np.array(t["pad_amp"])
            # The AM/PM of the output stage grows with the frequency (band-dependent calibration).
            pp = np.radians(np.array(t["pad_ph"]) * (self.freq / t["band_ref_hz"] if t["band_scaling"] else 1.0))
            w = np.array([0.0] + list(t["rfdac_w"]))
            amp = np.where(lvl >= 0, pa[np.maximum(lvl, 0)] * (w[mask] if dual else 1.0), 0.0)
            # The phase compensation of the driver is applied while the carrier is on; it is held while off.
            comp_held = comp.copy()
            last = 0.0
            for i in np.nonzero(lvl < 0)[0]:
                comp_held[i] = comp_held[i - 1] if i > 0 else last
            extra_ph = self.DRIVER_AMPM_SIGN * comp_held + np.where(lvl >= 0, pp[np.maximum(lvl, 0)], 0.0)
        else:
            lut = parse_list(s.get("eer_lut", ""))
            curve = np.array(lut) / lut[-1] if lut else None
            x = np.clip(a, 0, 1)
            if curve is not None:
                code = np.interp(x, curve, np.linspace(0, 1, len(curve)))
            else:
                code = x
            code = code * float(s["eer_gain"]) + float(s["eer_offset"])
            if not constant:
                counters["envelope_clip"] = int(np.count_nonzero((a > 1) | (code > 1) | (code < 0)))
            code = np.clip(code, 0, 1)
            if constant:
                code = np.where(on, 1.0, float(s["eer_idle"]))
            else:
                code = np.where(on, code, float(s["eer_idle"]))
            ampm = parse_list(s.get("eer_ampm_lut", ""))
            comp = np.radians(np.interp(code, np.linspace(0, 1, len(ampm)), ampm)) if ampm else np.zeros(n)
            # Supply modulator: one-pole low-pass (the residual delay is part of the alignment delay above).
            pad = int(0.02 * rate)
            cz = np.concatenate([np.full(pad, float(s["eer_idle"])), code, np.full(pad, float(s["eer_idle"]))])
            f = np.fft.fftfreq(len(cz), 1.0 / rate)
            cf = np.fft.ifft(np.fft.fft(cz) / (1 + 1j * f / t["eer_lpf_hz"])).real[pad: pad + n]
            cf = np.clip(cf, 0, 1)
            if t["eer_hardware"]:
                lin = np.clip((cf - t["eer_offset"]) / (1 - t["eer_offset"]), 0, 1)
                env = t["eer_feed"] + (1 - t["eer_feed"]) * lin ** t["eer_gamma"]
            else:
                env = np.full(n, 0.5)
            drive = int(s["eer_drive"])
            env = env * min(1.0, t["pad_amp"][drive] / 0.55)
            amp = np.where(on, env, 0.0)
            extra_ph = self.DRIVER_AMPM_SIGN * comp + np.radians(t["eer_ph_deg"] * (1 - cf) ** 2)
        if t["tx_phase_noise_deg"] > 0:
            # Close-in phase noise (300 Hz wide): inside the signal band, so no driver parameter removes it.
            pn = band_mask_filter(rng.normal(size=n).astype(complex), rate, 0, 300.0).real
            extra_ph = extra_ph + np.radians(t["tx_phase_noise_deg"]) * pn / max(np.std(pn), 1e-12)
        out = amp * np.exp(1j * (ph + extra_ph))
        if (float(st.get("fifo_ms", 250)) < t["min_fifo_ms"] or float(st.get("preroll_ms", 100)) < t["min_preroll_ms"]
                or float(st.get("rt_priority", 50)) < t["min_rt"]):
            k = 1 + int(2 * n / rate)
            counters["underflows"] = k
            for _ in range(k):
                i = int(rng.integers(0, max(n - int(0.003 * rate), 1)))
                out[i: i + int(0.003 * rate)] = 0
        return out, rate, counters

    def burst(self, iq, burst_id):
        rng = np.random.default_rng(seed_of(burst_id))
        t = self.truth
        out, rate, counters = self._tx(np.asarray(iq, np.complex128), rng)
        s = self.profile["settings"]
        e_tx = (t["xo_ppm"] - float(s["ppm"])) * 1e-6
        e_rx = (t["rx_xo_ppm"] - self.rx_corr) * 1e-6
        rx_true = self.rx_rate * (1 + e_rx)
        y, _ = resample_rate(out, rate * (1 - e_tx), rx_true)
        df = -self.freq * e_tx - (self.freq - self.rx_offset) * e_rx
        jitter = int(rng.integers(0, int(0.01 * self.rx_rate)))
        lead = int(self.rx_lead * self.rx_rate) + jitter
        tail = int(self.rx_tail * self.rx_rate)
        cap = np.zeros(lead + len(y) + tail, complex)
        cap[lead: lead + len(y)] = y
        k = np.arange(len(cap))
        cap *= np.exp(2j * np.pi * (self.rx_offset + df) * k / self.rx_rate + 1j * rng.uniform(0, 2 * np.pi))
        g = 10 ** ((self.rx_gain - 30.0) / 20.0)
        sig = cap * 0.316 * g
        noise = (rng.normal(size=len(cap)) + 1j * rng.normal(size=len(cap))) * (10 ** (t["rx_noise_dbfs"] / 20.0) * g / math.sqrt(2))
        x = sig + noise
        x = x * (1 - t["rx_cubic"] * np.abs(x) ** 2) + t["iq_eps"] * np.conj(x) + t["dc"]
        clipped = np.max(np.abs(np.concatenate([x.real, x.imag]))) >= 1.0
        x = np.clip(x.real, -1, 1) + 1j * np.clip(x.imag, -1, 1)
        if t["adc_bits"]:
            q = 2.0 / 2 ** t["adc_bits"]
            x = np.round(x.real / q) * q + 1j * np.round(x.imag / q) * q
        info = dict(counters=counters, tx_seconds=len(iq) / self.fs, rx_overflow=False, pll_locked=True,
                    rx_clipped=bool(clipped), sample_rate=float(self.fs), peak=float(np.max(np.abs(np.concatenate([x.real, x.imag])))))
        return x.astype(np.complex64), info

    def capture_idle(self, seconds, burst_id, freq):
        """Receives an external reference carrier at @freq (for --freq-ref external)."""
        rng = np.random.default_rng(seed_of(burst_id))
        n = int(seconds * self.rx_rate)
        e_rx = (self.truth["rx_xo_ppm"] - self.rx_corr) * 1e-6
        df = -(freq - self.rx_offset) * e_rx
        k = np.arange(n)
        x = 0.3 * np.exp(2j * np.pi * (self.rx_offset + df) * k / self.rx_rate)
        x += (rng.normal(size=n) + 1j * rng.normal(size=n)) * 0.003
        return x.astype(np.complex64)


# ---------------------------------------------------------------------------------------------------------------
# Real bench: rpitx transmitter + SoapySDR receiver
# ---------------------------------------------------------------------------------------------------------------


class RxRadio:
    """Background capture of a SoapySDR receiver; captures are delimited by begin() / end()."""

    def __init__(self, SoapySDR, args, freq, rate, gain, ppm):
        from SoapySDR import SOAPY_SDR_CF32, SOAPY_SDR_RX

        self.S = SoapySDR
        self.RX = SOAPY_SDR_RX
        self.dev = SoapySDR.Device(args)
        self.dev.setSampleRate(SOAPY_SDR_RX, 0, rate)
        self.set_ppm(ppm)
        self.dev.setFrequency(SOAPY_SDR_RX, 0, freq)
        self.dev.setGainMode(SOAPY_SDR_RX, 0, False)
        if gain is not None:
            self.dev.setGain(SOAPY_SDR_RX, 0, gain)
        self.stream = self.dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32)
        self.dev.activateStream(self.stream)
        self.lock = threading.Lock()
        self.recording = False
        self.chunks = []
        self.overflow = False
        self.running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def set_ppm(self, ppm):
        try:
            if self.dev.hasFrequencyCorrection(self.RX, 0):
                self.dev.setFrequencyCorrection(self.RX, 0, float(ppm))
                return True
        except Exception:
            pass
        return abs(ppm) < 1e-9

    def gain_range(self):
        r = self.dev.getGainRange(self.RX, 0)
        return r.minimum(), r.maximum()

    def set_gain(self, g):
        self.dev.setGain(self.RX, 0, g)

    def set_freq(self, f):
        self.dev.setFrequency(self.RX, 0, f)

    def _run(self):
        buf = np.zeros(65536, np.complex64)
        while self.running:
            sr = self.dev.readStream(self.stream, [buf], len(buf), timeoutUs=200000)
            if sr.ret == self.S.SOAPY_SDR_OVERFLOW:
                with self.lock:
                    if self.recording:
                        self.overflow = True
            elif sr.ret > 0:
                with self.lock:
                    if self.recording:
                        self.chunks.append(buf[: sr.ret].copy())

    def begin(self):
        with self.lock:
            self.chunks = []
            self.overflow = False
            self.recording = True

    def end(self):
        with self.lock:
            self.recording = False
            x = np.concatenate(self.chunks) if self.chunks else np.zeros(0, np.complex64)
            self.chunks = []
            return x, self.overflow

    def close(self):
        self.running = False
        self.thread.join()
        self.dev.deactivateStream(self.stream)
        self.dev.closeStream(self.stream)


class RealBench:
    """The rpitx driver plus a SoapySDR receiver in the same process."""

    def __init__(self, a, log):
        import SoapySDR
        from SoapySDR import SOAPY_SDR_TX

        self.S = SoapySDR
        self.TX = SOAPY_SDR_TX
        self.a = a
        self.log = log
        self.freq = a.freq
        self.fs = a.rate
        self.rx_rate = a.rx_rate
        self.rx_offset = a.rx_offset
        self.rx_lead, self.rx_tail = 0.3, 0.25
        self.duty = a.duty
        self.driver_messages = []
        try:
            SoapySDR.registerLogHandler(self._log_handler)
        except Exception:
            pass
        dev_args = dict(driver="rpitx")
        dev_args.update(parse_kwargs(a.dev_args, "--dev-args"))
        self.dev_args = dev_args
        self.profile_file = None
        self.tx = SoapySDR.Device(dev_args)
        self.written = {}
        self.tx.setSampleRate(SOAPY_SDR_TX, 0, a.rate)
        self.tx.setFrequency(SOAPY_SDR_TX, 0, a.freq)
        self.stream = None
        self.stream_args = None
        gain = None if a.rx_gain == "auto" else float(a.rx_gain)
        self.rx = RxRadio(SoapySDR, rx_device_args(a), a.freq - a.rx_offset, a.rx_rate, gain, a.rx_ppm)
        self.rx_freq = a.freq - a.rx_offset

    def set_freq(self, f):
        self.freq = f
        self.tx.setFrequency(self.TX, 0, f)
        self.set_rx_freq(f - self.rx_offset)

    def use_driver_profile(self, path):
        """Re-opens the transmitter with profile=<path> (None: without a profile); only the amplitude mode is
        written afterwards, so the file alone defines the settings, the stream defaults and the gain."""
        if self.stream is not None:
            self.tx.closeStream(self.stream)
            self.stream = None
        self.stream_args = None
        self.tx = None
        args = dict(self.dev_args)
        if path:
            args["profile"] = os.path.abspath(path)
        self.tx = self.S.Device(args)
        self.tx.setSampleRate(self.TX, 0, self.fs)
        self.tx.setFrequency(self.TX, 0, self.freq)
        self.written = {}
        self.profile_file = path

    def _log_handler(self, level, message):
        self.driver_messages.append(message)
        if level <= 4:
            self.log("driver: %s" % message)

    def rx_gain_range(self):
        return self.rx.gain_range()

    def set_rx_gain(self, g):
        self.rx.set_gain(g)

    def set_rx_ppm(self, ppm):
        if not self.rx.set_ppm(ppm):
            raise Infeasible("the receiver does not support a frequency correction")

    def set_rx_freq(self, f):
        self.rx.set_freq(f)
        self.rx_freq = f

    def sensors_sample_rate(self):
        try:
            return float(self.tx.readSensor("sample_rate"))
        except Exception:
            return float(self.fs)

    def _sensors(self):
        out = {}
        for k in ("underflows", "late", "dma_errors", "envelope_late", "envelope_clip"):
            try:
                out[k] = int(self.tx.readSensor(k))
            except Exception:
                out[k] = 0
        return out

    def configure(self, profile, mode):
        from SoapySDR import SOAPY_SDR_CF32

        if self.profile_file:
            stream = parse_kwargs(self.a.stream_args, "--stream-args")
        else:
            stream = {k: to_text(v) for k, v in profile["stream"].items()}
        stream["signal_cleanup"] = "true"
        if self.stream is None or stream != self.stream_args:
            if self.stream is not None:
                self.tx.closeStream(self.stream)
                self.stream = None
            try:
                self.stream = self.tx.setupStream(self.TX, SOAPY_SDR_CF32, [0], stream)
            except RuntimeError as e:
                self.stream_args = None
                raise Infeasible(str(e))
            self.stream_args = stream
        try:
            # Only changed settings are written: a change of ppm, ppm_ntp or harmonic retunes the transmitter.
            items = profile_settings(profile, mode).items()
            if self.profile_file:
                items = [("amplitude_mode", MODE_AMPLITUDE[mode])]
            for k, v in items:
                text = to_text(v)
                if self.written.get(k) != text:
                    self.written.pop(k, None)
                    self.tx.writeSetting(k, text)
                    self.written[k] = text
            if not self.profile_file:
                gain = profile["gain_constant"] if MODE_AMPLITUDE[mode] == "constant" else profile["gain_envelope"]
                self.tx.setGain(self.TX, 0, gain)
        except RuntimeError as e:
            raise Infeasible(str(e))

    def burst(self, iq, burst_id):
        S = self.S
        self.rx.begin()
        time.sleep(self.rx_lead)
        self.tx.activateStream(self.stream)
        before = self._sensors()
        done, stalled = 0, 0
        t0 = time.monotonic()
        try:
            while done < len(iq):
                sr = self.tx.writeStream(self.stream, [iq[done:]], len(iq) - done, timeoutUs=1000000)
                if sr.ret < 0 and sr.ret != S.SOAPY_SDR_TIMEOUT:
                    raise RuntimeError("writeStream error %d" % sr.ret)
                if sr.ret <= 0:
                    stalled += 1
                    if stalled > 5:
                        raise RuntimeError("writeStream timed out")
                done += max(sr.ret, 0)
            self.tx.writeStream(self.stream, [np.zeros(0, np.complex64)], 0, S.SOAPY_SDR_END_BURST)
        finally:
            self.tx.deactivateStream(self.stream)
        after = self._sensors()
        try:
            pll = self.tx.readSensor("pll_locked") == "true"
        except Exception:
            pll = True
        time.sleep(self.rx_tail)
        cap, overflow = self.rx.end()
        on = time.monotonic() - t0
        if self.duty < 1.0:
            time.sleep(on * (1.0 - self.duty) / self.duty)
        counters = {k: max(after[k] - before[k], 0) for k in after}
        peak = float(np.max(np.abs(np.concatenate([cap.real, cap.imag])))) if len(cap) else 0.0
        return cap, dict(counters=counters, tx_seconds=len(iq) / self.fs, rx_overflow=overflow, pll_locked=pll,
                         rx_clipped=peak >= 0.99, sample_rate=self.sensors_sample_rate(), peak=peak)

    def capture_idle(self, seconds, burst_id, freq):
        self.rx.begin()
        time.sleep(seconds)
        cap, _ = self.rx.end()
        return cap

    def close(self):
        try:
            if self.stream is not None:
                self.tx.closeStream(self.stream)
                self.stream = None
        finally:
            self.rx.close()


# ---------------------------------------------------------------------------------------------------------------
# Run context: budget, checkpoint and the evaluation of one parameter set
# ---------------------------------------------------------------------------------------------------------------


class Budget:
    def __init__(self, a):
        self.max_trials = a.max_trials
        self.max_minutes = a.max_minutes
        self.max_tx = a.max_tx_seconds
        self.trials = 0
        self.tx_seconds = 0.0
        self.cached_elapsed = 0.0
        self.t0 = time.monotonic()
        self.reason = None

    def elapsed(self):
        return self.cached_elapsed + time.monotonic() - self.t0

    def check(self):
        if self.trials >= self.max_trials:
            self.reason = "max-trials (%d)" % self.max_trials
        elif self.elapsed() >= 60.0 * self.max_minutes:
            self.reason = "max-minutes (%g)" % self.max_minutes
        elif self.tx_seconds >= self.max_tx:
            self.reason = "max-tx-seconds (%g)" % self.max_tx
        if self.reason:
            raise BudgetExhausted(self.reason)


class Checkpoint:
    def __init__(self, path, fingerprint):
        self.path = path
        self.data = dict(version=TOOL_VERSION, fingerprint=fingerprint, cache={})

    @classmethod
    def load(cls, path, fingerprint):
        cp = cls(path, fingerprint)
        if path and os.path.exists(path):
            with open(path) as f:
                d = json.load(f)
            if d.get("fingerprint") != fingerprint:
                raise SystemExit("--resume: the checkpoint was written for a different frequency, rate, path, "
                                 "mode list or seed")
            cp.data = d
        return cp

    def save(self):
        if self.path:
            write_json_atomic(self.path, self.data)


class Context:
    def __init__(self, a, bench, out_dir, log, budget=None, stopper=None):
        self.a = a
        self.bench = bench
        self.out = out_dir
        self.log = log
        self.budget = budget or Budget(a)
        self.stopper = stopper if stopper is not None else {"stop": False}
        self.thr = merge_thresholds(THRESHOLDS, a.thresholds_data)
        self.stage_seq = {}
        self.findings = []
        self.history = []
        self.stage_reports = []
        fp = dict(freq=a.freq, rate=a.rate, path=a.path, modes=a.mode_list, seed=a.seed, harmonic=a.harmonic,
                  envelope_output=a.envelope_output, simulate=a.simulate)
        own = os.path.join(out_dir, "checkpoint.json") if out_dir else None
        if a.resume == "auto":
            cp_path = own
        else:
            cp_path = a.resume or own
        self.cp = Checkpoint.load(cp_path, fp) if a.resume else Checkpoint(cp_path, fp)
        self.limits = dict(STAGE_LIMITS)
        self.limits.update(a.stage_limits)

    @property
    def stop(self):
        return self.stopper["stop"]

    def note(self, text):
        if text not in self.findings:
            self.findings.append(text)
            self.log("finding: " + text)

    def payload_seconds(self, mode):
        if mode == "iq_raw":
            return self.a.robust_seconds
        if mode == "carrier":
            return min(self.a.payload_seconds, 0.5)
        return self.a.payload_seconds

    def evaluate(self, profile, mode, stage, repeats=None, agg="median", extra=None, purpose=""):
        """Transmits @mode with @profile @repeats times and returns the aggregated metrics."""
        repeats = repeats or self.a.repeats
        seq = self.stage_seq.get(stage, 0)
        self.stage_seq[stage] = seq + 1
        key = "%s#%d:%s:%s:%s" % (stage, seq, mode, stable_hash([profile, extra, repeats, agg]), self.a.seed)
        cache = self.cp.data["cache"]
        if key in cache:
            r = cache[key]
            self.budget.trials += 1
            self.budget.tx_seconds += r["cost"]["tx_seconds"]
            self.budget.cached_elapsed += r["cost"]["elapsed"]
            self.history.append(dict(stage=stage, mode=mode, key=key, cached=True))
            return r
        if self.stop:
            raise Interrupted()
        self.budget.check()
        t0 = time.monotonic()
        self.budget.trials += 1
        result = dict(valid=False, infeasible=None, metrics={}, extra={}, counters={}, cost=dict(tx_seconds=0.0, elapsed=0.0))
        try:
            self.bench.configure(profile, mode)
        except Infeasible as e:
            result["infeasible"] = str(e)
        runs = []
        if result["infeasible"] is None:
            for r in range(repeats):
                for attempt in range(3):
                    if self.stop:
                        raise Interrupted()
                    bid = "%s|%d|%d" % (key, r, attempt)
                    try:
                        frame = build_frame(mode, self.a.rate, self.payload_seconds(mode), seed_of(bid), extra)
                    except Infeasible as e:
                        result["infeasible"] = str(e)
                        break
                    cap, info = self.bench.burst(frame.iq, bid)
                    result["cost"]["tx_seconds"] += info["tx_seconds"]
                    self.budget.tx_seconds += info["tx_seconds"]
                    ok = not info["rx_overflow"] and info["pll_locked"]
                    err = None
                    if ok:
                        try:
                            met, ex = measure_frame(cap, frame, self.bench.rx_rate, self.bench.rx_offset, self.bench.rx_lead, self.a.rx_sfdr)
                        except AnalysisError as e:
                            ok, err = False, str(e)
                    else:
                        err = "receiver overflow" if info["rx_overflow"] else "PLL not locked"
                    self._save_capture(bid, cap, frame, ok)
                    if ok:
                        exp = (info["sample_rate"] / self.a.rate - 1.0) * 1e6
                        met["freq_err_hz"] = (abs(ex["f_err"]), None)
                        met["clock_ppm"] = (abs(ex["clock_ppm"] - exp), None)
                        c = info["counters"]
                        met["counters"] = (float(c.get("underflows", 0) + c.get("late", 0) + c.get("dma_errors", 0) + c.get("envelope_late", 0)), None)
                        if MODE_AMPLITUDE[mode] == "envelope":
                            met["clip"] = (float(c.get("envelope_clip", 0)), None)
                        ex["rx_clipped"] = info["rx_clipped"]
                        ex["peak"] = info["peak"]
                        runs.append((met, ex, c))
                        break
                    self.log("  invalid capture (%s), retrying" % err)
                if result["infeasible"]:
                    break
        if runs:
            result["valid"] = True
            names = set.intersection(*[set(m) for m, _, _ in runs])
            for name in sorted(names):
                vals = np.array([m[name][0] for m, _, _ in runs], dtype=float)
                floors = [m[name][1] for m, _, _ in runs if m[name][1] is not None]
                sense = METRICS[name][0]
                if agg == "worst":
                    srt = np.sort(vals) if sense == "max" else -np.sort(-vals)
                    v = float(srt[-2] if len(srt) >= 5 else srt[-1])
                else:
                    v = float(np.median(vals))
                spread = float(1.4826 * np.median(np.abs(vals - np.median(vals)))) if len(vals) > 1 else 0.0
                result["metrics"][name] = [v, float(np.median(floors)) if floors else None, spread]
            ex0 = {}
            for k in runs[0][1]:
                vs = [e[k] for _, e, _ in runs if k in e]
                if isinstance(vs[0], list):
                    ex0[k] = np.median(np.array(vs, dtype=float), axis=0).tolist()
                elif isinstance(vs[0], bool):
                    ex0[k] = any(vs)
                else:
                    ex0[k] = float(np.median(vs))
            result["extra"] = ex0
            result["counters"] = {k: int(sum(c.get(k, 0) for _, _, c in runs)) for k in runs[0][2]}
        result["cost"]["elapsed"] = time.monotonic() - t0
        cache[key] = result
        self.cp.save()
        self.history.append(dict(stage=stage, mode=mode, key=key, cached=False))
        return result

    def _save_capture(self, bid, cap, frame, ok):
        mode = self.a.save_captures
        if not self.out or mode == "none" or (mode == "fail" and ok):
            return
        d = trusted_output_dir(os.path.join(self.out, "captures"))
        name = hashlib.sha1(bid.encode()).hexdigest()[:12]
        with open_binary_644(os.path.join(d, name + ".npz")) as fz:
            np.savez_compressed(fz, cap=cap, rx_rate=self.bench.rx_rate, rx_offset=self.bench.rx_offset,
                                rx_lead=self.bench.rx_lead, sfdr=self.a.rx_sfdr, frame=json.dumps(frame.args), burst=bid, valid=ok)


def metric(r, name, default=float("inf")):
    m = r["metrics"].get(name) if r and r.get("valid") else None
    return m[0] if m else default


def spread(r, name):
    m = r["metrics"].get(name) if r and r.get("valid") else None
    return m[2] if m else 0.0


class StageBudget:
    def __init__(self, ctx, stage):
        self.left = ctx.limits[stage]

    def take(self):
        if self.left <= 0:
            return False
        self.left -= 1
        return True


def significant(ctx, new, old, sigma):
    """True when a score @new (lower is better) beats @old by more than the noise margin."""
    return new < old - max(ctx.a.min_delta, ctx.a.sigma_k * sigma)


# ---------------------------------------------------------------------------------------------------------------
# Search primitives
# ---------------------------------------------------------------------------------------------------------------


def search_points(ctx, sb, points, score_fn, incumbent=None):
    """Evaluates the candidate @points (in order) with @score_fn(point) -> (score, sigma); returns the best,
    keeping @incumbent unless a candidate beats it significantly."""
    scored = []
    for pt in points:
        if not sb.take():
            break
        s, sg = score_fn(pt)
        scored.append((s, sg, pt))
        ctx.log("    %-40s score %8.2f" % (json.dumps(pt, sort_keys=True), s))
    if not scored:
        return incumbent, float("inf"), 0.0, scored
    best = min(scored, key=lambda x: x[0])
    if incumbent is not None:
        inc = [x for x in scored if x[2] == incumbent]
        if inc and not significant(ctx, best[0], inc[0][0], math.hypot(best[1], inc[0][1])):
            return incumbent, inc[0][0], inc[0][1], scored
    return best[2], best[0], best[1], scored


def golden_1d(ctx, sb, lo, hi, score_fn, tol, cache, show=lambda x: x):
    """Golden-section refinement of a scalar in [lo, hi]; @cache maps already evaluated points to scores."""
    gr = (math.sqrt(5) - 1) / 2

    def f(x):
        x = round(x, 6)
        if x not in cache:
            if not sb.take():
                return None
            cache[x] = score_fn(x)
            ctx.log("    x=%-10g score %8.2f" % (show(x), cache[x][0]))
        return cache[x][0]

    a, b = lo, hi
    c = b - gr * (b - a)
    d = a + gr * (b - a)
    fc, fd = f(c), f(d)
    while abs(b - a) > tol and fc is not None and fd is not None:
        if fc < fd:
            b, d, fd = d, c, fc
            c = b - gr * (b - a)
            fc = f(c)
        else:
            a, c, fc = c, d, fd
            d = a + gr * (b - a)
            fd = f(d)
    best = min(cache.items(), key=lambda kv: kv[1][0])
    return best[0], best[1]


def tune_scalar(ctx, sb, current, grid, refine_tol, score_fn, log_scale=False):
    """Grid then golden-section search of a scalar; returns the accepted value."""
    cache = {}
    for x in grid:
        x = round(x, 6)
        if x in cache or not sb.take():
            continue
        cache[x] = score_fn(x)
        ctx.log("    x=%-10g score %8.2f" % (x, cache[x][0]))
    if not cache:
        return current
    xs = sorted(cache)
    bx = min(cache, key=lambda k: cache[k][0])
    i = xs.index(bx)
    lo, hi = xs[max(i - 1, 0)], xs[min(i + 1, len(xs) - 1)]
    if hi > lo:
        if log_scale:
            lcache = {round(math.log(k), 6): v for k, v in cache.items()}
            lx, _ = golden_1d(ctx, sb, math.log(lo), math.log(hi), lambda u: score_fn(round(math.exp(u), 6)), refine_tol, lcache, show=math.exp)
            cache.update({round(math.exp(k), 6): v for k, v in lcache.items()})
        else:
            golden_1d(ctx, sb, lo, hi, score_fn, refine_tol, cache)
    bx = min(cache, key=lambda k: cache[k][0])
    cur = round(current, 6)
    if cur in cache and not significant(ctx, cache[bx][0], cache[cur][0], math.hypot(cache[bx][1], cache[cur][1])):
        return current
    return bx


# ---------------------------------------------------------------------------------------------------------------
# Stages
# ---------------------------------------------------------------------------------------------------------------


def with_settings(profile, **kw):
    p = copy.deepcopy(profile)
    p["settings"].update(kw)
    return p


def with_stream(profile, **kw):
    p = copy.deepcopy(profile)
    p["stream"].update(kw)
    return p


def max_slots(ctx):
    return max(s for s in range(1, ctx.a.max_slots + 1) if ctx.a.rate * s <= MAX_SLOT_RATE) if ctx.a.rate <= MAX_SLOT_RATE else 1


def stage_s0(ctx, profile):
    """Robustness: escalates the real-time and buffering parameters until two long bursts run clean."""
    sb = StageBudget(ctx, "S0")
    slots = max_slots(ctx) if profile["path"] == "non_eer" and "S5" in ctx.a.stage_list else profile["stream"].get("slots_per_sample", 1)
    ncpu = os.cpu_count() or 1
    slot_rate = ctx.a.rate * slots
    ring = int(profile["stream"].get("ring_size", max(4096, int(math.ceil(0.085 * slot_rate / 512.0)) * 512)))
    ladder = [dict(rt_priority=80)]
    if ncpu > 1:
        ladder.append(dict(cpu=ncpu - 1))
    ladder += [dict(fifo_ms=2), dict(preroll_ms=2), dict(ring_size=2), dict(fifo_ms=2), dict(preroll_ms=2), dict(ring_size=2)]
    clean = 0
    status = "FAIL"
    while sb.take():
        test = with_stream(profile, slots_per_sample=slots)
        r = ctx.evaluate(test, "iq_raw", "S0", repeats=1)
        bad = metric(r, "counters") if r["valid"] else float("inf")
        ctx.log("  S0 counters %s %s" % (r.get("counters"), "" if r["valid"] else "(invalid)"))
        if bad == 0:
            clean += 1
            if clean >= 2:
                status = PASS
                break
            continue
        clean = 0
        if not ladder:
            break
        step = ladder.pop(0)
        st = dict(profile["stream"])
        for k, v in step.items():
            if k in ("fifo_ms", "preroll_ms"):
                st[k] = min(int(float(st.get(k, 250 if k == "fifo_ms" else 100)) * v), 5000)
            elif k == "ring_size":
                ring = min(ring * v, 32768)
                st[k] = ring
            else:
                st[k] = v
        profile = copy.deepcopy(profile)
        profile["stream"] = st
        ctx.log("  S0 escalating: %s" % json.dumps({k: st[k] for k in step}))
    if status != PASS:
        ctx.note("robustness: counters still non-zero after the escalation ladder (consider isolcpus, the "
                 "performance governor, or the receiver on another host)")
    return profile, status


def ppm_loop(ctx, sb, measure, apply, ppm0, expected_sign, tol_hz, label):
    """Drives a frequency error to zero by adjusting a ppm value; detects a sign opposite to @expected_sign."""
    ppm = ppm0
    sign = expected_sign  # ppm += sign * error / frequency * 1e6
    last = None
    for _ in range(8):
        if not sb.take():
            break
        err = measure(ppm)
        if err is None:
            return ppm, False
        ctx.log("  %s ppm=%+.4f frequency error %+.3f Hz" % (label, ppm, err))
        if abs(err) <= tol_hz:
            return ppm, True
        if last is not None and abs(err) > abs(last[1]) * 1.5 and abs(ppm - last[0]) > 1e-6:
            sign = -sign
            ctx.note("%s: the frequency correction acts with the opposite sign than expected" % label)
            ppm = last[0]
            err = last[1]
        last = (ppm, err)
        ppm = ppm + sign * err / ctx.a.freq * 1e6
        ppm = apply(ppm)
    return ppm, False


def stage_s1(ctx, profile):
    """Frequency: closes the loop on the crystal correction of the transmitter (or of the receiver)."""
    sb = StageBudget(ctx, "S1")
    tol = ctx.thr["global"]["freq_abs_hz"] + ctx.thr["global"]["freq_ppm"] * 1e-6 * ctx.a.freq
    tol = min(tol, 0.5 * tol + 0.5)

    def measure_with(prof):
        r = ctx.evaluate(prof, "carrier", "S1")
        if not r["valid"]:
            return None
        return r["extra"]["f_err"]

    if ctx.a.freq_ref == "tx":
        def apply_rx(ppm):
            ctx.bench.set_rx_ppm(ppm)
            return ppm

        def meas_rx(ppm):
            p = copy.deepcopy(profile)
            p["rx_ppm"] = ppm
            return measure_with(p)

        ppm, ok = ppm_loop(ctx, sb, meas_rx, apply_rx, profile["rx_ppm"], -1.0, tol, "receiver")
        profile = copy.deepcopy(profile)
        profile["rx_ppm"] = ppm
        ctx.bench.set_rx_ppm(ppm)
        return profile, PASS if ok else FAIL

    def meas_tx(ppm):
        return measure_with(with_settings(profile, ppm=round(ppm, 4)))

    ppm, ok = ppm_loop(ctx, sb, meas_tx, lambda p: round(min(max(p, -200), 200), 4), float(profile["settings"]["ppm"]), -1.0, tol, "transmitter")
    return with_settings(profile, ppm=round(ppm, 4)), PASS if ok else FAIL


def stage_s2(ctx, profile):
    """Drive level: the lowest pad drive (constant modes) or eer_drive within --drive-margin of the maximum."""
    sb = StageBudget(ctx, "S2")
    power = {}
    for lvl in range(8):
        if not sb.take():
            break
        if profile["path"] == "eer":
            p = with_settings(profile, eer_drive=lvl)
        else:
            p = copy.deepcopy(profile)
            p["gain_constant"] = lvl
        r = ctx.evaluate(p, "carrier", "S2")
        if r["valid"]:
            power[lvl] = r["extra"]["power_db"]
            ctx.log("  S2 drive %d: %.2f dB" % (lvl, power[lvl]))
    if not power:
        return profile, FAIL
    top = max(power.values())
    lvl = min(k for k, v in power.items() if v >= top - ctx.a.drive_margin)
    if profile["path"] == "eer":
        return with_settings(profile, eer_drive=lvl), PASS
    p = copy.deepcopy(profile)
    p["gain_constant"] = lvl
    return p, PASS


def measure_staircase(ctx, profile, mags, stage, stream=None):
    seq = []
    for m in mags:
        seq += [1.0, float(m)]
    seq.append(1.0)
    p = profile if stream is None else with_stream(profile, **stream)
    r = ctx.evaluate(p, "staircase", stage, extra=dict(mags=seq, dwell=ctx.a.dwell))
    if not r["valid"]:
        return None, None
    return np.array(r["extra"]["amps"]), np.array(r["extra"]["phases"])


def table_loop(ctx, sb, profile, n, build_settings, select_mags, first_min, stage_label, stream=None):
    """Measures and refines an amplitude table and its AM/PM table.

    @build_settings(lut, ampm) returns the settings for the measurement, @select_mags(lut) the magnitudes that
    select every table entry. The sign with which the driver applies the AM/PM table is measured, not assumed."""
    if not sb.take():
        return None, None, False
    amp, ph = measure_staircase(ctx, with_settings(profile, **build_settings(None, None)), select_mags(None), "S3", stream)
    if amp is None:
        return None, None, False
    lut, fixed = increasing_table(amp / amp[-1], first_min)
    if fixed:
        ctx.log("  %s: raised entries %s to keep the table increasing (measurement noise)" % (stage_label, fixed))
    phys = wrap_degrees(np.array(ph) - ph[-1])
    if stage_label == "eer":
        phys[0] = phys[1]
    ampm = phys.copy()
    k_sign = None
    converged = False
    for it in range(6):
        if not sb.take():
            break
        amp2, ph2 = measure_staircase(ctx, with_settings(profile, **build_settings(lut, ampm)), select_mags(lut), "S3", stream)
        if amp2 is None:
            break
        res = wrap_degrees(np.array(ph2) - ph2[-1])
        if stage_label == "eer":
            res[0] = res[1]
        a_res = float(np.max(np.abs(amp2 / amp2[-1] / lut - 1.0)))
        if k_sign is None:
            big = np.abs(ampm) > 2.0
            if np.any(big):
                k = float(np.median((phys[big] - res[big]) / ampm[big]))
                k_sign = 1.0 if k >= 0 else -1.0
                if k_sign < 0:
                    ctx.note("%s AM/PM table: the driver applies the phase table with the sign opposite to the measured "
                             "phase (the table must hold the negated phase; rpitx_amp_calibrate.py prints the measured "
                             "phase)" % stage_label)
            else:
                k_sign = 1.0
        ctx.log("  %s refinement %d: amplitude residual %.2f %%, phase residual %.2f deg" % (stage_label, it + 1, 100 * a_res, float(np.max(np.abs(res)))))
        if a_res <= 0.01 and float(np.max(np.abs(res))) <= 1.0:
            converged = True
            break
        lut, _ = increasing_table(0.5 * (lut + amp2 / amp2[-1]), first_min)
        ampm = wrap_degrees(ampm + 0.8 * res / k_sign)
    return lut, ampm, converged


def stage_s3(ctx, profile):
    """Static tables: measured with the alternating reference / state method, then refined in a closed loop."""
    sb = StageBudget(ctx, "S3")
    if profile["path"] == "non_eer":
        base = dict(envelope_delay=0.0, amplitude_quantizer="nearest", envelope_floor=0.0, iq_scale=1.0, rfdac_lut="")
        p = copy.deepcopy(profile)
        p["gain_envelope"] = 7

        def build(lut, ampm):
            s = dict(base)
            s["amplitude_lut"] = "" if lut is None else format_list(lut, 6)
            s["ampm_lut"] = "" if ampm is None else format_list(ampm, 5)
            return s

        def mags(lut):
            return [(l + 1) / 8.0 for l in range(8)] if lut is None else list(lut / lut[-1])

        lut, ampm, ok = table_loop(ctx, sb, p, 8, build, mags, 1e-4, "pad", stream=dict(rf_dac=False))
        if lut is None:
            return profile, FAIL
        profile = with_settings(profile, amplitude_lut=format_list(lut, 6), ampm_lut=format_list(ampm, 5))
        if ctx.a.rf_dac_available and sb.take():
            q = with_settings(profile, envelope_delay=0.0, amplitude_quantizer="nearest", rfdac_lut="0.7,0.2,1",
                              amplitude_lut="", ampm_lut="")
            q["gain_envelope"] = 7
            amp, _ = measure_staircase(ctx, q, [0.7, 0.2, 1.0], "S3", stream=dict(rf_dac=True))
            if amp is not None:
                profile = with_settings(profile, rfdac_lut=format_list(np.maximum(amp / amp[-1], 1e-4), 5))
        return profile, PASS if ok else FAIL
    pts = ctx.a.eer_points
    base = dict(envelope_delay=0.0, eer_gain=1.0, eer_offset=0.0, iq_scale=1.0, envelope_floor=0.0)

    def build_e(lut, ampm):
        s = dict(base)
        s["eer_lut"] = "" if lut is None else format_list(lut, 6)
        s["eer_ampm_lut"] = "" if ampm is None else format_list(ampm, 5)
        return s

    def mags_e(lut):
        return list(np.linspace(0, 1, pts)) if lut is None else list(lut / lut[-1])

    lut, ampm, ok = table_loop(ctx, sb, profile, pts, build_e, mags_e, 0.0, "eer")
    if lut is None:
        return profile, FAIL
    return with_settings(profile, eer_lut=format_list(lut, 6), eer_ampm_lut=format_list(ampm, 5)), PASS if ok else FAIL


def stage_s4(ctx, profile, local=False):
    """Envelope delay: minimum IMD3 of the two-tone test."""
    sb = StageBudget(ctx, "S4")
    cur = float(profile["settings"]["envelope_delay"])
    if local:
        lo, hi = cur - 0.5, cur + 0.5
    else:
        lo, hi = (-2.0, 2.0) if profile["path"] == "non_eer" else (-8.0, 8.0)
        if ctx.a.delay_range:
            lo, hi = ctx.a.delay_range
    lo, hi = max(lo, -32.0), min(hi, 32.0)

    def score(d):
        r = ctx.evaluate(with_settings(profile, envelope_delay=round(d, 4)), "usb", "S4")
        if not r["valid"]:
            return float("inf"), 0.0
        return metric(r, "imd3_dbc"), spread(r, "imd3_dbc")

    grid = list(np.linspace(lo, hi, 5 if local else 9))
    if not any(abs(g - cur) < 1e-9 for g in grid):
        grid = [cur] + grid
    best = tune_scalar(ctx, sb, cur, grid, 0.05, score)
    return with_settings(profile, envelope_delay=round(best, 4)), PASS


def s5_score(ctx, prof):
    rq = ctx.evaluate(prof, "qpsk", "S5")
    ru = ctx.evaluate(prof, "usb", "S5")
    if not rq["valid"] or not ru["valid"]:
        return float("inf"), 0.0
    acpr_thr = ctx.thr[threshold_table(ctx, prof["path"])]["qpsk"]["acpr_db"]
    s = 0.5 * metric(rq, "evm_db") + 0.5 * metric(ru, "imd3_dbc") + 3.0 * max(0.0, metric(rq, "acpr_db", 0.0) - acpr_thr)
    return s, 0.5 * math.hypot(spread(rq, "evm_db"), spread(ru, "imd3_dbc"))


def stage_s5(ctx, profile):
    """Quantiser and oversampling: exhaustive search of the small discrete grid."""
    sb = StageBudget(ctx, "S5")
    quant = [("nearest", 2), ("sigma_delta", 1), ("sigma_delta", 2)]
    slots = [s for s in range(1, ctx.a.max_slots + 1) if ctx.a.rate * s <= MAX_SLOT_RATE]
    dacs = [False, True] if ctx.a.rf_dac_available else [False]
    cur = dict(q=profile["settings"]["amplitude_quantizer"], o=int(profile["settings"]["sigma_delta_order"]),
               s=int(profile["stream"].get("slots_per_sample", 1)), d=bool(profile["stream"].get("rf_dac", False)))
    if cur["q"] == "nearest":
        cur["o"] = 2
    pts = [dict(cur)] + [dict(q=q, o=o, s=s, d=d) for d in dacs for s in slots for q, o in quant if dict(q=q, o=o, s=s, d=d) != cur]

    def build(pt):
        p = with_settings(profile, amplitude_quantizer=pt["q"], sigma_delta_order=pt["o"])
        return with_stream(p, slots_per_sample=pt["s"], rf_dac=pt["d"])

    def score(pt):
        # Each point costs two evaluations (QPSK and two-tone).
        sb.left -= 1
        return s5_score(ctx, build(pt))

    best, _, _, _ = search_points(ctx, sb, pts, score, incumbent=dict(cur))
    newp = build(best)
    if best["s"] != cur["s"] and "S4" in ctx.a.stage_list:
        newp, _ = stage_s4(ctx, newp, local=True)
    return newp, PASS


def clip_penalty(r):
    c = metric(r, "clip", 0.0)
    return 0.0 if c <= 0 else 10.0 + 10.0 * math.log10(1.0 + c)


def stage_s6(ctx, profile):
    """Envelope scale (and EER gain / offset): best OFDM EVM without envelope clipping."""
    sb = StageBudget(ctx, "S6")

    def score_p(p):
        r = ctx.evaluate(p, "ofdm", "S6")
        if not r["valid"]:
            return float("inf"), 0.0
        return metric(r, "evm_db") + clip_penalty(r), spread(r, "evm_db")

    cur = float(profile["settings"]["iq_scale"])
    grid = list(np.exp(np.linspace(math.log(0.5), math.log(2.0), 7)))
    if not any(abs(g - cur) < 1e-6 for g in grid):
        grid = [cur] + grid
    best = tune_scalar(ctx, sb, cur, grid, 0.03, lambda x: score_p(with_settings(profile, iq_scale=round(x, 4))), log_scale=True)
    profile = with_settings(profile, iq_scale=round(best, 4))
    if profile["path"] == "eer":
        cg = float(profile["settings"]["eer_gain"])
        bg = tune_scalar(ctx, sb, cg, [cg] + list(np.linspace(0.85, 1.15, 7)), 0.01,
                         lambda x: score_p(with_settings(profile, eer_gain=round(x, 4))))
        profile = with_settings(profile, eer_gain=round(bg, 4))
        co = float(profile["settings"]["eer_offset"])
        bo = tune_scalar(ctx, sb, co, [co] + list(np.linspace(-0.05, 0.05, 5)), 0.005,
                         lambda x: score_p(with_settings(profile, eer_offset=round(x, 4))))
        profile = with_settings(profile, eer_offset=round(bo, 4))
    return profile, PASS


def stage_s7(ctx, profile):
    """Silence: envelope_floor (and eer_idle) that removes the noise of the pauses without cutting the signal."""
    sb = StageBudget(ctx, "S7")
    results = {}

    def ev(p):
        r = ctx.evaluate(p, "pauses", "S7")
        if not r["valid"]:
            return None
        return metric(r, "leak_dbc"), r["metrics"]["leak_dbc"][1], r["extra"].get("active_nmse_db", 0.0), spread(r, "leak_dbc")

    floors = [0.0, 0.003, 0.01, 0.02, 0.04]
    idles = [float(profile["settings"].get("eer_idle", 0.0))] + ([0.0, 0.02, 0.05] if profile["path"] == "eer" else [])
    idles = sorted(set(idles), key=idles.index)
    cur = (float(profile["settings"]["envelope_floor"]), idles[0])
    for idle in idles:
        for fl in floors:
            if not sb.take():
                break
            kw = dict(envelope_floor=fl)
            if profile["path"] == "eer":
                kw["eer_idle"] = idle
            res = ev(with_settings(profile, **kw))
            if res is not None:
                results[(fl, idle)] = res
                ctx.log("    floor=%-6g idle=%-5g leak %7.1f dBc active nmse %6.1f dB" % (fl, idle, res[0], res[2]))
    if not results:
        return profile, FAIL
    base_nmse = min(v[2] for v in results.values())

    def score(k):
        leak, floor, nmse, _ = results[k]
        return max(leak, (floor if floor is not None else -200) - 10) + 3 * max(0.0, nmse - base_nmse - 0.5)

    best = min(results, key=score)
    if cur in results and not significant(ctx, score(best), score(cur), results[best][3]):
        best = cur
    kw = dict(envelope_floor=best[0])
    if profile["path"] == "eer":
        kw["eer_idle"] = best[1]
    return with_settings(profile, **kw), PASS


STAGES = {"S0": stage_s0, "S1": stage_s1, "S2": stage_s2, "S3": stage_s3, "S4": stage_s4, "S5": stage_s5,
          "S6": stage_s6, "S7": stage_s7}


def run_stage(ctx, name, profile):
    ctx.log("[%s] %s (path %s)" % (name, STAGE_NAMES[name], profile["path"]))
    before = copy.deepcopy(profile)
    t0 = ctx.budget.trials
    profile, status = STAGES[name](ctx, profile)
    changed = {k: profile["settings"][k] for k in profile["settings"] if profile["settings"][k] != before["settings"].get(k)}
    changed.update({"stream." + k: v for k, v in profile["stream"].items() if before["stream"].get(k) != v})
    for k in ("gain_constant", "gain_envelope", "rx_ppm"):
        if profile[k] != before[k]:
            changed[k] = profile[k]
    ctx.stage_reports.append(dict(stage=name, path=profile["path"], status=status, evaluations=ctx.budget.trials - t0, changed=changed))
    ctx.log("[%s] %s, %d evaluations, changed %s" % (name, status, ctx.budget.trials - t0, json.dumps(changed, default=str) if changed else "nothing"))
    return profile, changed


# ---------------------------------------------------------------------------------------------------------------
# Verification and closed loop
# ---------------------------------------------------------------------------------------------------------------


def threshold_table(ctx, path):
    """Threshold table used for @path: its own one, or the one chosen with --threshold-set."""
    ts = ctx.a.threshold_set
    return path if ts == "auto" else ts.replace("-", "_")


def limits_for(ctx, path, mode):
    lim = dict(ctx.thr[threshold_table(ctx, path)].get(mode, {}))
    g = ctx.thr["global"]
    lim["freq_err_hz"] = g["freq_abs_hz"] + g["freq_ppm"] * 1e-6 * ctx.a.freq
    lim["clock_ppm"] = g["clock_ppm"]
    lim["counters"] = g["counters"]
    if MODE_AMPLITUDE[mode] == "envelope":
        lim["clip"] = g["clip"]
    return lim


def metric_items(res):
    return [(k, e) for k, e in res.items() if k not in ("_extra", "_policy")]


def mode_state(res):
    """Worst state of the metrics of one mode."""
    states = [e.get("state") for _, e in metric_items(res)]
    for s in (FAIL, INVALID, INFEASIBLE, HARDWARE_LIMIT, UNVERIFIABLE):
        if s in states:
            return s
    return PASS


def verify(ctx, profile, modes, stage="V"):
    """Runs every mode and classifies every metric; returns {mode: {metric: entry}}."""
    out = {}
    pol = ctx.a.policy[profile["path"]]
    for mode in modes:
        r = ctx.evaluate(profile, mode, stage, repeats=ctx.a.verify_repeats, agg="worst")
        lim = limits_for(ctx, profile["path"], mode)
        res = {}
        if r["infeasible"]:
            res["_mode"] = dict(state=INFEASIBLE, reason=r["infeasible"])
        elif not r["valid"]:
            res["_mode"] = dict(state=INVALID, reason="no valid capture")
        else:
            for name, limit in lim.items():
                if name not in r["metrics"]:
                    continue
                v, fl, sp = r["metrics"][name]
                state = classify(v, fl, limit, METRICS[name][0], ctx.a.floor_margin)
                res[name] = dict(value=v, floor=fl, spread=sp, limit=limit, state=state)
            res["_extra"] = {k: v for k, v in r["extra"].items() if not isinstance(v, list)}
        res["_policy"] = dict(required=mode in pol["required"], allow_hl=mode in pol["allow_hl"])
        out[mode] = res
        bad = ", ".join("%s=%s" % (k, e["state"]) for k, e in metric_items(res) if e.get("state") != PASS)
        ctx.log("  verify %-6s %s%s" % (mode, bad or "all PASS", "" if res["_policy"]["required"] else "  (optional)"))
    return out


def failing(ctx, verdict):
    """(mode, metric) pairs in FAIL; optional modes only with --tune-optional yes."""
    out = []
    for m, res in verdict.items():
        if ctx.a.tune_optional == "no" and not res.get("_policy", {}).get("required", True):
            continue
        out += [(m, k) for k, e in metric_items(res) if e.get("state") == FAIL]
    return out


def stages_for(ctx, profile, mode, name):
    st = [s for s in DIAGNOSIS.get(name, []) if s in PATH_STAGES[profile["path"]] and s in ctx.a.stage_list]
    if MODE_AMPLITUDE[mode] == "constant":
        st = [s for s in st if s in CONSTANT_STAGES]
    return st


def closed_loop(ctx, profile, modes):
    """Verifies, re-tunes the stages that can improve the failing metrics, and repeats."""
    tried = set()
    verdict = None
    for rnd in range(ctx.a.max_rounds + 1):
        ctx.log("[V] verification round %d" % (rnd + 1))
        verdict = verify(ctx, profile, modes)
        fails = failing(ctx, verdict)
        if not fails:
            break
        todo = []
        for mode, name in fails:
            todo += [s for s in stages_for(ctx, profile, mode, name) if s not in tried and s not in todo]
        if not todo or rnd == ctx.a.max_rounds:
            break
        for s in [s for s in PATH_STAGES[profile["path"]] if s in todo]:
            profile, _ = run_stage(ctx, s, profile)
            tried.add(s)
    for mode, res in verdict.items():
        for name, e in metric_items(res):
            if e.get("state") != FAIL:
                continue
            if all(s in tried for s in stages_for(ctx, profile, mode, name)):
                e["state"] = HARDWARE_LIMIT
                if name == "dev_err_pct":
                    ctx.note("FM deviation error is not tunable in the driver: check the 'crosses a PLL integer step' "
                             "warning and move the frequency slightly or lower the sample rate")
    return profile, verdict


# ---------------------------------------------------------------------------------------------------------------
# Pre-flight
# ---------------------------------------------------------------------------------------------------------------


def rx_gain_search(ctx, profile):
    """Highest receiver gain with the carrier peak at or below -6 dBFS."""
    lo, hi = ctx.bench.rx_gain_range()
    best = lo
    g = lo
    while g <= hi + 1e-9:
        ctx.bench.set_rx_gain(g)
        r = ctx.evaluate(profile, "carrier", "P-gain", repeats=1)
        peak = r["extra"].get("peak", 1.0) if r["valid"] else (1.0 if r.get("extra", {}).get("rx_clipped") else 0.0)
        ctx.log("  receiver gain %5.1f dB: peak %.3f" % (g, peak))
        if r["valid"] and peak <= 0.5 and not r["extra"].get("rx_clipped"):
            best = g
        elif r["valid"]:
            break
        g += 3.0
    ctx.bench.set_rx_gain(best)
    return best


def external_reference(ctx):
    """Calibrates the receiver against an external carrier at --ref-freq."""
    f = ctx.a.ref_freq
    ppm = ctx.a.rx_ppm
    ctx.bench.set_rx_freq(f - ctx.bench.rx_offset)
    try:
        for it in range(5):
            ctx.bench.set_rx_ppm(ppm)
            cap = ctx.bench.capture_idle(1.0, "ref|%d" % it, f)
            x = np.asarray(cap, np.complex128)
            x = x * np.exp(-2j * np.pi * ctx.bench.rx_offset * np.arange(len(x)) / ctx.bench.rx_rate)
            y, r = resample_rate(x, ctx.bench.rx_rate, 8000.0)
            df = _coarse_freq(y[len(y) // 10:], r)
            ctx.log("  reference carrier error %+.3f Hz (receiver ppm %+.4f)" % (df, ppm))
            if abs(df) < 0.05 + 0.01e-6 * f:
                break
            ppm = ppm + df / f * 1e6
    finally:
        ctx.bench.set_rx_freq(ctx.a.freq - ctx.bench.rx_offset)
    return ppm


def preflight(ctx, profile):
    a = ctx.a
    ctx.log(NOTICE)
    os_ = analysis_os(a.rate, a.rx_rate)
    if abs(a.rx_offset) + os_ * a.rate / 2 > 0.45 * a.rx_rate:
        raise SystemExit("--rx-offset %g Hz with an analysis band of %g Hz does not fit in --rx-rate %g" % (a.rx_offset, os_ * a.rate, a.rx_rate))
    if a.freq_ref == "external":
        profile["rx_ppm"] = external_reference(ctx)
    if a.rx_gain == "auto":
        g = rx_gain_search(ctx, profile)
        ctx.log("  receiver gain fixed at %.1f dB" % g)
        profile["notes"].append("receiver gain %.1f dB" % g)
    r = ctx.evaluate(profile, "carrier", "P")
    if r["infeasible"]:
        raise SystemExit("pre-flight: the driver refused the configuration: %s" % r["infeasible"])
    if not r["valid"]:
        raise SystemExit("pre-flight: no usable carrier received (check the receiver frequency, the attenuator and "
                         "--rx-offset)")
    snr = r["extra"]["snr_db"]
    ctx.log("  carrier SNR %.1f dB in %g Hz, frequency error %+.2f Hz" % (snr, a.rate, r["extra"]["f_err"]))
    if snr < 25:
        ctx.note("low SNR (%.1f dB): most thresholds will be UNVERIFIABLE" % snr)
    if profile["path"] == "eer":
        codes = [0.25, 0.5, 0.75, 1.0]
        p = with_settings(profile, eer_lut="", eer_ampm_lut="", envelope_delay=0.0, eer_gain=1.0, eer_offset=0.0)
        amp, _ = measure_staircase(ctx, p, codes, "P-eer")
        if amp is None or not np.all(np.diff(amp) > 0) or 20 * math.log10(max(amp[-1], 1e-9) / max(amp[0], 1e-9)) < 6.0:
            raise SystemExit("EER hardware not responding: the RF amplitude does not follow the envelope code (check the "
                             "DAC, the supply modulator, --envelope-output and the wiring)")
        ctx.log("  EER envelope response %s" % format_list(amp))
    return profile


# ---------------------------------------------------------------------------------------------------------------
# Outputs
# ---------------------------------------------------------------------------------------------------------------


def path_state(verdict, policy=None):
    """State of a path: the worst state of its required modes (all modes without @policy); a HARDWARE_LIMIT of a
    mode listed in the allowed set counts as PASS."""
    if verdict is None:
        return NOT_RUN
    states = []
    for mode, res in verdict.items():
        st = mode_state(res)
        if policy is not None:
            if mode not in policy["required"]:
                continue
            if st == HARDWARE_LIMIT and mode in policy["allow_hl"]:
                st = PASS
        states.append(st)
    for s in (FAIL, INVALID, INFEASIBLE, HARDWARE_LIMIT, UNVERIFIABLE):
        if s in states:
            return s
    return PASS


def worst_status(states):
    for s in ("ABORTED", "BUDGET_EXHAUSTED", FAIL, INVALID, INFEASIBLE, HARDWARE_LIMIT, UNVERIFIABLE, NOT_RUN):
        if s in states:
            return s
    return PASS


def board_model():
    try:
        with open("/proc/device-tree/model") as f:
            return f.read().strip("\x00\n ")
    except OSError:
        return "unknown"


SLOW_USB_PREFIXES = ("Raspberry Pi Zero", "Raspberry Pi Model A", "Raspberry Pi Model B", "Raspberry Pi 2", "Raspberry Pi 3",
                     "Raspberry Pi Compute Module Rev", "Raspberry Pi Compute Module 3")


def slow_usb_board(model):
    """True for the boards whose USB shares one controller with Ethernet or has a single OTG port and core
    (Pi Zero / 1 / 2 / 3, CM1 / CM3): a USB receiver there can disturb the real-time feeder thread."""
    return any((model or "").startswith(p) for p in SLOW_USB_PREFIXES)


def rx_device_args(a):
    """Device arguments of the receiver: --rx-args, wrapped for SoapyRemote with --rx-host remote."""
    kw = parse_kwargs(a.rx_args, "--rx-args")
    if a.rx_host == "local":
        return kw
    out = dict(driver="remote", remote=a.rx_remote)
    for k, v in kw.items():
        if k == "driver":
            out["remote:driver"] = v
        else:
            out[k] = v
    return out


def parse_driver_profile(text):
    """Parses a profile file in the format of the driver (see SoapyRPITX_Profile.hpp)."""
    settings, stream, gain = {}, {}, None
    for number, line in enumerate(text.splitlines(), 1):
        t = line.strip()
        if not t or t.startswith("#"):
            continue
        key, sep, value = t.partition("=")
        key, value = key.strip(), value.strip()
        if not sep:
            raise ValueError("line %d: expected key=value" % number)
        if not key:
            raise ValueError("line %d: empty key" % number)
        if key.startswith("stream."):
            if not key[7:]:
                raise ValueError("line %d: empty stream argument name" % number)
            stream[key[7:]] = value
        elif key == "gain":
            gain = value
        else:
            settings[key] = value
    return dict(settings=settings, stream=stream, gain=gain)


def coerce(text):
    if text in ("true", "false"):
        return text == "true"
    for f in (int, float):
        try:
            return f(text)
        except ValueError:
            pass
    return text


def profile_from_driver_file(path, a):
    """Tool profile equivalent to a driver profile file (the driver defaults for the keys the file omits)."""
    with open(path) as f:
        d = parse_driver_profile(f.read())
    eer = d["stream"].get("envelope_output", "none") in ("pcm", "pwm")
    prof = default_profile("eer" if eer else "non_eer", a)
    prof["settings"].update({k: coerce(v) for k, v in d["settings"].items() if k != "amplitude_mode"})
    prof["stream"].update({k: coerce(v) for k, v in d["stream"].items()})
    if d["gain"] is not None:
        prof["gain_envelope"] = prof["gain_constant"] = int(round(float(d["gain"])))
    return prof


def driver_profile_text(profile, a):
    """Profile file for the profile=<file> device argument / setting of the driver."""
    keys = COMMON_SETTINGS + (NON_EER_SETTINGS if profile["path"] == "non_eer" else EER_SETTINGS)
    lines = ["# rpitx profile written by rpitx_autotune.py %s" % TOOL_VERSION,
             "# path %s, %.6f MHz, %g S/s; open the device with profile=<this file>" % (profile["path"], a.freq / 1e6, a.rate)]
    for k in keys:
        if k in profile["settings"]:
            lines.append("%s=%s" % (k, to_text(profile["settings"][k])))
    if a.profile_amplitude_mode != "none":
        lines.append("amplitude_mode=%s" % a.profile_amplitude_mode)
    for k, v in sorted(profile["stream"].items()):
        lines.append("stream.%s=%s" % (k, to_text(v)))
    gain = profile["gain_envelope"] if a.profile_gain == "envelope" else profile["gain_constant"]
    lines.append("# gain of the %s modes; the %s modes were tuned with %d" % (
        a.profile_gain, "constant-envelope" if a.profile_gain == "envelope" else "envelope",
        profile["gain_constant"] if a.profile_gain == "envelope" else profile["gain_envelope"]))
    lines.append("gain=%d" % gain)
    return "\n".join(lines) + "\n"


def verdict_table(verdict, md):
    md += ["| Mode | Role | Metric | Value | Limit | Floor | State |", "|---|---|---|---|---|---|---|"]
    for mode, res in verdict.items():
        pol = res.get("_policy", {})
        role = ("required" if pol.get("required", True) else "optional") + (", HW limit allowed" if pol.get("allow_hl") else "")
        if "_mode" in res:
            md.append("| %s | %s | — | — | — | — | %s (%s) |" % (mode, role, res["_mode"]["state"], res["_mode"]["reason"]))
            continue
        for name, e in metric_items(res):
            fl = "%.4g" % e["floor"] if e["floor"] is not None else "—"
            md.append("| %s | %s | %s | %.4g %s | %s %.4g | %s | %s |" % (mode, role, name, e["value"], METRICS[name][1],
                                                                       "≤" if METRICS[name][0] == "max" else "≥", e["limit"], fl, e["state"]))
    md.append("")


def write_outputs(ctx, results, status):
    if not ctx.out:
        return
    trusted_output_dir(ctx.out)
    a = ctx.a
    rep = dict(tool_version=TOOL_VERSION, status=status, board=board_model() if not a.simulate else "simulated (%s)" % a.simulate,
               freq=a.freq, rate=a.rate, harmonic=a.harmonic, rx_args=rx_device_args(a), rx_host=a.rx_host, rx_rate=a.rx_rate,
               rx_offset=a.rx_offset, freq_ref=a.freq_ref, threshold_set=a.threshold_set,
               policy={p: dict(required=v["required"], allow_hardware_limit=v["allow_hl"]) for p, v in a.policy.items()},
               budget=dict(trials=ctx.budget.trials, tx_seconds=ctx.budget.tx_seconds, elapsed_s=ctx.budget.elapsed(),
                           exhausted=ctx.budget.reason),
               findings=ctx.findings, stages=ctx.stage_reports, paths={})
    md = ["# rpitx_autotune report", "",
          "| Item | Value |", "|---|---|",
          "| Status | **%s** |" % status, "| Board | %s |" % rep["board"],
          "| Frequency | %.6f MHz (harmonic %d) |" % (a.freq / 1e6, a.harmonic), "| Sample rate | %g S/s |" % a.rate,
          "| Receiver | `%s` (%s), %g S/s, offset %g Hz, reference %s |" % (
              ",".join("%s=%s" % kv for kv in rep["rx_args"].items()), a.rx_host, a.rx_rate, a.rx_offset, a.freq_ref),
          "| Thresholds | %s |" % a.threshold_set,
          "| Budget | %d evaluations, %.0f s on air, %.1f min%s |" % (ctx.budget.trials, ctx.budget.tx_seconds, ctx.budget.elapsed() / 60,
                                                                    ", stopped by " + ctx.budget.reason if ctx.budget.reason else ""), ""]
    for path, entry in results.items():
        profile, verdict = entry[0], entry[1]
        dp_verdict = entry[2] if len(entry) > 2 else None
        dev, lists, stream = profile_lines(profile)
        pol = a.policy[path]
        pst = path_state(verdict, pol)
        rep["paths"][path] = dict(state=pst, profile=profile, verdict=verdict, driver_profile_verdict=dp_verdict,
                                  driver_profile_state=path_state(dp_verdict, pol) if dp_verdict else None)
        write_json_atomic(os.path.join(ctx.out, "profile_%s.json" % path),
                          dict(profile=profile, board=rep["board"], freq=a.freq, rate=a.rate, tool_version=TOOL_VERSION,
                               date=time.strftime("%Y-%m-%d %H:%M:%S")))
        if a.profile_format in ("args", "both"):
            with open_mode_644(os.path.join(ctx.out, "profile_%s.args" % path)) as f:
                f.write("# device arguments\n%s\n" % dev)
                f.write("# list settings: writeSetting(key, value) after opening the device, or keys of the device "
                        "argument dict in Python\n")
                for line in lists:
                    f.write(line + "\n")
                f.write("# stream arguments\n%s\n" % stream)
                f.write("# gain: setGain(TX, 0, %d) for envelope modes, %d for constant-envelope modes\n" % (profile["gain_envelope"], profile["gain_constant"]))
        if a.profile_format in ("driver", "both"):
            with open_mode_644(os.path.join(ctx.out, "profile_%s.rpitx" % path)) as f:
                f.write(driver_profile_text(profile, a))
        md += ["## Path `%s`: %s" % (path, pst), "",
               "Required modes: %s; HARDWARE_LIMIT allowed for: %s." % (", ".join(pol["required"]) or "none", ", ".join(pol["allow_hl"]) or "none"), "",
               "```", dev] + lists + ["# stream: " + stream, "# gain: envelope %d, constant %d" % (profile["gain_envelope"], profile["gain_constant"]), "```", ""]
        if a.profile_format in ("driver", "both"):
            md += ["Driver profile: `profile_%s.rpitx` (device argument `profile=<file>`)." % path, ""]
        if verdict:
            verdict_table(verdict, md)
        if dp_verdict:
            md += ["### Re-verification through `profile=`: %s" % path_state(dp_verdict, pol), ""]
            verdict_table(dp_verdict, md)
    md += ["## Stages", "", "| Stage | Path | Status | Evaluations | Changed |", "|---|---|---|---|---|"]
    for s in ctx.stage_reports:
        md.append("| %s %s | %s | %s | %d | %s |" % (s["stage"], STAGE_NAMES[s["stage"]], s["path"], s["status"], s["evaluations"],
                                                    ", ".join("`%s`=%s" % (k, v) for k, v in s["changed"].items()) or "—"))
    if ctx.findings:
        md += ["", "## Findings", ""] + ["- " + f for f in ctx.findings]
    md += ["", "Parameters are specific to this board, frequency, load and temperature."]
    write_json_atomic(os.path.join(ctx.out, "report.json"), rep)
    with open_mode_644(os.path.join(ctx.out, "report.md")) as f:
        f.write("\n".join(md) + "\n")
    ctx.log("results written to %s" % ctx.out)


# ---------------------------------------------------------------------------------------------------------------
# Multi-band comparison
# ---------------------------------------------------------------------------------------------------------------


def band_label(f):
    return "%sMHz" % ("%.6f" % (f / 1e6)).rstrip("0").rstrip(".")


def profile_params(profile):
    """Flat {parameter: value} view of a profile for the band comparison."""
    keys = COMMON_SETTINGS + (NON_EER_SETTINGS if profile["path"] == "non_eer" else EER_SETTINGS)
    out = {k: profile["settings"].get(k) for k in keys}
    out.update({"stream." + k: v for k, v in sorted(profile["stream"].items())})
    out.update(gain_envelope=profile["gain_envelope"], gain_constant=profile["gain_constant"], rx_ppm=profile.get("rx_ppm", 0.0))
    return out


def same_across(values, tol):
    """True when the values of one parameter agree across the bands: |difference| <= tol * max(1, |value|) for
    numbers and element-wise for tables, equality otherwise."""
    def as_list(v):
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            return [float(v)]
        if isinstance(v, str):
            try:
                return [float(x) for x in v.split(",")] if v else []
            except ValueError:
                return None
        return None

    lists = [as_list(v) for v in values]
    if all(x is not None for x in lists):
        if len(set(len(x) for x in lists)) > 1:
            return False
        if not lists[0]:
            return True
        arr = np.array(lists, dtype=float)
        scale = max(1.0, float(np.max(np.abs(arr))))
        return float(np.max(np.max(arr, axis=0) - np.min(arr, axis=0))) <= tol * scale
    return all(v == values[0] for v in values)


def short(v):
    t = to_text(v) if v is not None else "—"
    if t == "":
        return "(empty)"
    parts = t.split(",")
    return t if len(parts) <= 3 else ",".join(parts[:3]) + ",…"


def write_band_table(a, bands, status):
    """bands.md / bands.json: profiles of every band and which parameters are band-independent."""
    paths = []
    for _, ctx in bands:
        for p in ctx.results:
            if p not in paths:
                paths.append(p)
    rep = dict(tool_version=TOOL_VERSION, status=status, tolerance=a.band_tolerance, reuse=a.band_reuse,
               bands=[dict(freq=f, label=band_label(f), status=ctx.final_status,
                           paths={p: path_state(v[1], a.policy[p]) for p, v in ctx.results.items()}) for f, ctx in bands],
               paths={})
    md = ["# rpitx_autotune band comparison", "", "Status: **%s**, band reuse `%s`, tolerance %g." % (status, a.band_reuse, a.band_tolerance), ""]
    for path in paths:
        cols = [(f, ctx.results[path][0]) for f, ctx in bands if path in ctx.results]
        params = profile_params(cols[0][1])
        table = {}
        md += ["## Path `%s`" % path, "", "| Parameter | " + " | ".join(band_label(f) for f, _ in cols) + " | Class |",
               "|---|" + "---|" * (len(cols) + 1)]
        md.append("| state | " + " | ".join(path_state(ctx.results[path][1], a.policy[path]) for f, ctx in bands if path in ctx.results) + " | |")
        for k in params:
            vals = [profile_params(p).get(k) for _, p in cols]
            cls = "independent" if same_across(vals, a.band_tolerance) else "dependent"
            table[k] = dict(values=vals, cls=cls)
            md.append("| `%s` | %s | %s |" % (k, " | ".join(short(v) for v in vals), cls))
        md.append("")
        rep["paths"][path] = table
    write_json_atomic(os.path.join(a.out, "bands.json"), rep)
    with open_mode_644(os.path.join(a.out, "bands.md")) as f:
        f.write("\n".join(md) + "\n")
    return rep


# ---------------------------------------------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------------------------------------------


def make_bench(a, log):
    if a.simulate:
        return SimBench(a, a.simulate)
    return RealBench(a, log)


def make_logger(quiet):
    def log(text):
        if not quiet:
            print(text, flush=True)
    return log


def run_path(ctx, path, results, start=None, skip=()):
    a = ctx.a
    profile = copy.deepcopy(start) if start else default_profile(path, a)
    if start:
        profile["notes"] = list(profile.get("notes", [])) + ["started from the profile of the previous band"]
    results[path] = (profile, None)
    profile = preflight(ctx, profile)
    results[path] = (profile, None)
    if profile.get("rx_ppm"):
        ctx.bench.set_rx_ppm(profile["rx_ppm"])
    for st in PATH_STAGES[path]:
        if st not in a.stage_list:
            continue
        if st in skip:
            ctx.stage_reports.append(dict(stage=st, path=path, status="SKIPPED (band reuse)", evaluations=0, changed={}))
            ctx.log("[%s] skipped: parameters reused from the previous band" % st)
            continue
        profile, _ = run_stage(ctx, st, profile)
        results[path] = (profile, None)
        if st == "S2":
            const = [m for m in a.mode_list if MODE_AMPLITUDE[m] == "constant"]
            if const:
                v = verify(ctx, profile, const, stage="V-early")
                bad = [m for m, res in v.items() if mode_state(res) == FAIL]
                if bad:
                    ctx.log("  constant-envelope modes failing after S2: %s" % ", ".join(bad))
    profile, verdict = closed_loop(ctx, profile, a.mode_list)
    results[path] = (profile, verdict)
    if a.check_driver_profile:
        d = os.path.join(ctx.out, "profile_%s.rpitx" % path)
        with open_mode_644(d) as f:
            f.write(driver_profile_text(profile, a))
        ctx.log("[VP] re-verification through profile=%s" % d)
        ctx.bench.use_driver_profile(d)
        try:
            dpv = verify(ctx, profile, a.mode_list, stage="VP")
        finally:
            ctx.bench.use_driver_profile(None)
        results[path] = (profile, verdict, dpv)
        if path_state(dpv, a.policy[path]) != path_state(verdict, a.policy[path]):
            ctx.note("path %s: the verification through the driver profile file differs from the tuned verification" % path)
    return profile, verdict


def install_signal_handlers(stopper, log):
    def handler(signum, frame):
        if stopper["stop"]:
            raise KeyboardInterrupt()
        stopper["stop"] = True
        log("stop requested: finishing the current burst")

    for s in (signal.SIGINT, signal.SIGTERM):
        try:
            signal.signal(s, handler)
        except (ValueError, OSError):
            pass


def rx_placement_check(a, log):
    """Warns about (or refuses, --rx-local-policy refuse) a USB receiver on a board with a slow USB path."""
    if a.rx_host != "local" or a.simulate or a.rx_local_policy == "ignore":
        return
    model = board_model()
    if slow_usb_board(model):
        text = ("%s: a receiver on the local USB bus can disturb the real-time feeder thread; consider "
                "--rx-host remote --rx-remote tcp://<host>:55132" % model)
        if a.rx_local_policy == "refuse":
            raise SystemExit("run: " + text + " (or --rx-local-policy warn)")
        log("warning: " + text)


def run_band(a, bench, out, log, quiet, stopper, budget=None, start=None, skip=()):
    trusted_output_dir(out)
    ctx = Context(a, bench, out, log, budget=budget, stopper=stopper)
    ctx.results = {}
    status = "ABORTED"
    try:
        for path in a.path_list:
            run_path(ctx, path, ctx.results, start=(start or {}).get(path), skip=skip)
        status = worst_status([path_state(v[1], a.policy[p]) for p, v in ctx.results.items()])
    except BudgetExhausted as e:
        status = "BUDGET_EXHAUSTED"
        log("budget exhausted: %s" % e)
    except Interrupted:
        status = "ABORTED"
        log("interrupted")
    finally:
        try:
            write_outputs(ctx, ctx.results, status)
        except Exception as e:  # the report must not hide the original error
            log("could not write the report: %s" % e)
    ctx.final_status = status
    return ctx


def cmd_run(a, quiet=False):
    if not a.simulate and not a.confirm_dummy_load:
        raise SystemExit("run: transmitting needs --confirm-dummy-load (dummy load and attenuator in place)")
    log = make_logger(quiet)
    rx_placement_check(a, log)
    trusted_output_dir(a.out)
    stopper = {"stop": False}
    if not quiet:
        install_signal_handlers(stopper, log)
    bench = make_bench(a, log)
    multi = len(a.freq_list) > 1
    shared = Budget(a) if a.budget_scope == "run" else None
    bands = []
    prev = None
    try:
        for i, f in enumerate(a.freq_list):
            ab = copy.copy(a)
            ab.freq = f
            if multi:
                log("=== band %s (%d of %d) ===" % (band_label(f), i + 1, len(a.freq_list)))
                bench.set_freq(f)
            out = os.path.join(a.out, band_label(f)) if multi else a.out
            start = prev if (a.band_reuse != "none" and prev) else None
            skip = a.band_stage_list if (a.band_reuse == "skip" and prev) else ()
            ctx = run_band(ab, bench, out, log, quiet, stopper, budget=shared, start=start, skip=skip)
            bands.append((f, ctx))
            prev = {p: v[0] for p, v in ctx.results.items()}
            if ctx.final_status == "ABORTED" or (shared is not None and shared.reason):
                break
    finally:
        bench.close()
    status = worst_status([c.final_status for _, c in bands]) if bands else "ABORTED"
    if len(bands) < len(a.freq_list) and status == PASS:
        status = "ABORTED"
    last = bands[-1][1] if bands else None
    if multi and bands:
        write_band_table(a, bands, status)
        log("band comparison written to %s" % os.path.join(a.out, "bands.md"))
    log("status: %s" % status)
    if last is None:
        raise SystemExit("run: nothing was executed")
    last.final_status = status
    last.bands = bands
    return last


def cmd_verify(a):
    if not a.simulate and not a.confirm_dummy_load:
        raise SystemExit("verify: transmitting needs --confirm-dummy-load")
    log = make_logger(False)
    rx_placement_check(a, log)
    if a.profile.endswith(".rpitx"):
        prof = profile_from_driver_file(a.profile, a)
        rpitx_file = a.profile
    else:
        with open(a.profile) as f:
            prof = json.load(f)["profile"]
        rpitx_file = os.path.splitext(a.profile)[0] + ".rpitx"
    a.path = prof["path"].replace("_", "-")
    a.path_list = [prof["path"]]
    trusted_output_dir(a.out)
    stopper = {"stop": False}
    install_signal_handlers(stopper, log)
    bench = make_bench(a, log)
    ctx = Context(a, bench, a.out, log, stopper=stopper)
    status = "ABORTED"
    results = {}
    try:
        if a.apply == "driver-profile":
            if not os.path.exists(rpitx_file):
                raise SystemExit("verify: --apply driver-profile needs %s (run with --profile-format driver|both)" % rpitx_file)
            bench.use_driver_profile(rpitx_file)
        if prof.get("rx_ppm"):
            bench.set_rx_ppm(prof["rx_ppm"])
        if a.rx_gain == "auto":
            rx_gain_search(ctx, prof)
        verdict = verify(ctx, prof, a.mode_list)
        results[prof["path"]] = (prof, verdict)
        status = path_state(verdict, a.policy[prof["path"]])
    except (BudgetExhausted, Interrupted) as e:
        log(str(e))
    finally:
        bench.close()
        write_outputs(ctx, results, status)
    log("status: %s" % status)
    return 0 if status in (PASS,) or (status == UNVERIFIABLE and a.allow_unverifiable) else 1


def cmd_replay(a):
    files = []
    for root, _, names in os.walk(a.out):
        if os.path.basename(root) == "captures":
            files += [os.path.join(root, n) for n in sorted(names) if n.endswith(".npz")]
    if not files:
        raise SystemExit("replay: no captures under %s (run with --save-captures fail|all)" % a.out)
    for path in files:
        z = np.load(path, allow_pickle=False)
        fa = json.loads(str(z["frame"]))
        frame = build_frame(fa["mode"], fa["fs"], fa["seconds"], fa["seed"], fa["extra"])
        name = os.path.relpath(path, a.out)
        try:
            met, ex = measure_frame(z["cap"], frame, float(z["rx_rate"]), float(z["rx_offset"]), float(z["rx_lead"]), float(z["sfdr"]))
            txt = ", ".join("%s=%.4g" % (k, v[0]) for k, v in sorted(met.items()))
            print("%s %-9s f_err %+.3f Hz  %s" % (name, fa["mode"], ex["f_err"], txt), flush=True)
        except AnalysisError as e:
            print("%s %-9s analysis failed: %s" % (name, fa["mode"], e), flush=True)
    return 0


# ---------------------------------------------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------------------------------------------


def _sim_args(extra):
    p = build_parser()
    return finalize_args(p, p.parse_args(["run", "--simulate", "good", "--freq", "144.39M", "--rate", "24000", "--rx-rate", "240000",
                                          "--rx-offset", "60k", "--payload-seconds", "0.8", "--robust-seconds", "1",
                                          "--dwell", "0.05", "--repeats", "1", "--verify-repeats", "2", "--eer-points", "9",
                                          "--rx-gain", "30"] + extra))


def _sim_verify_args(extra):
    p = build_parser()
    return finalize_args(p, p.parse_args(["verify", "--simulate", "good", "--freq", "144.39M", "--rate", "24000", "--rx-rate", "240000",
                                          "--rx-offset", "60k", "--payload-seconds", "0.8", "--robust-seconds", "1",
                                          "--verify-repeats", "2", "--rx-gain", "30"] + extra))


def self_test(quick, keep):
    ok_all = True
    tmp = keep or tempfile.mkdtemp(prefix="rpitx_autotune_")

    def check(name, cond, detail=""):
        nonlocal ok_all
        ok_all = ok_all and bool(cond)
        print("  %-58s %s %s" % (name, "ok" if cond else "FAILED", detail))

    print("analysis on simulated captures")
    a = _sim_args(["--out", os.path.join(tmp, "analysis")])
    bench = SimBench(a, "good")
    prof = default_profile("non_eer", a)
    bench.configure(prof, "carrier")
    fr = build_frame("carrier", a.rate, 1.5, 1)
    cap, _ = bench.burst(fr.iq, "t1")
    met, ex = measure_frame(cap, fr, a.rx_rate, a.rx_offset, bench.rx_lead, a.rx_sfdr)
    expect_f = -a.freq * (2.0e-6) - (a.freq - a.rx_offset) * (-1.5e-6)
    check("carrier frequency error recovered", abs(ex["f_err"] - expect_f) < 0.05, "(%.3f vs %.3f Hz)" % (ex["f_err"], expect_f))
    check("sample clock error recovered", abs(ex["clock_ppm"] - (-2.0 + 1.5)) < 0.25, "(%.3f ppm)" % ex["clock_ppm"])

    # Ideal transmitter (EER model with a linear modulator and no AM/PM) for the measurement accuracy checks.
    b2 = SimBench(a, "good")
    b2.truth.update(eer_gamma=1.0, eer_feed=0.0, eer_offset=0.0, eer_ph_deg=0.0, eer_delay=0.0, eer_lpf_hz=1e9,
                    rx_cubic=0.0, xo_ppm=0.0, rx_xo_ppm=0.0)
    ideal = default_profile("eer", a)
    ideal["settings"]["envelope_delay"] = 0.0
    for mode in ("fsk", "afsk", "mfsk", "bpsk", "qpsk", "8psk", "qam16", "nbfm", "cw"):
        b2.configure(ideal, mode)
        fr = build_frame(mode, a.rate, 0.8, 7)
        cap, _ = b2.burst(fr.iq, "ideal-" + mode)
        met, _ = measure_frame(cap, fr, a.rx_rate, a.rx_offset, b2.rx_lead, a.rx_sfdr)
        if mode in ("fsk", "afsk", "bpsk", "qpsk"):
            check("%s: no bit errors on an ideal transmitter" % mode, met["ber"][0] == 0)
        if mode in ("mfsk", "8psk", "qam16"):
            check("%s: no symbol errors on an ideal transmitter" % mode, met["ser"][0] == 0)
        if mode == "nbfm":
            check("nbfm: deviation within 0.5 %", met["dev_err_pct"][0] < 0.5, "(%.3f %%)" % met["dev_err_pct"][0])
        if mode == "qpsk":
            check("qpsk: ideal EVM near the measurement floor", met["evm_db"][0] < met["evm_db"][1] + 3,
                  "(%.1f dB, floor %.1f dB)" % met["evm_db"])
        if mode == "cw":
            check("cw: keying timing within 0.5 ms", met["key_timing_ms"][0] < 0.5, "(%.2f ms)" % met["key_timing_ms"][0])

    # Known memoryless compression: y = x (1 - c |x|^2); for two tones of amplitude A, IMD3 = cA^2 / (1 - 3cA^2).
    b3 = SimBench(a, "good")
    b3.truth.update(rx_cubic=0.0, xo_ppm=0.0, rx_xo_ppm=0.0)
    orig = b3._tx

    def compressed(iq, rng):
        x = iq * (1 - 0.1 * np.abs(iq) ** 2)
        return x, a.rate, dict(underflows=0, late=0, dma_errors=0, envelope_late=0, envelope_clip=0)

    b3._tx = compressed
    b3.configure(ideal, "usb")
    fr = build_frame("usb", a.rate, 0.8, 3)
    cap, _ = b3.burst(fr.iq, "imd")
    met, _ = measure_frame(cap, fr, a.rx_rate, a.rx_offset, b3.rx_lead, a.rx_sfdr)
    expect = 20 * math.log10(0.025 / (1 - 0.075))
    check("two-tone IMD3 of a known compression", abs(met["imd3_dbc"][0] - expect) < 1.0, "(%.2f vs %.2f dBc)" % (met["imd3_dbc"][0], expect))
    b3._tx = orig

    modes = "cw,nbfm,fsk,bpsk,qpsk,usb" if quick else "cw,nbfm,fsk,afsk,mfsk,bpsk,qpsk,8psk,am,usb,lsb,iq_raw"
    print("optimiser on simulated hardware")
    t0 = time.monotonic()
    good = cmd_run(_sim_args(["--modes", modes, "--out", os.path.join(tmp, "good")]), quiet=True)
    check("non-EER good hardware converges to PASS", good.final_status == PASS,
          "(%s, %d evaluations, %.0f s)" % (good.final_status, good.budget.trials, time.monotonic() - t0))
    check("AM/PM sign of the driver detected", any("AM/PM" in f for f in good.findings))
    if good.final_status != PASS:
        for path, (p, v) in good.results.items():
            for m, res in (v or {}).items():
                for k, e in res.items():
                    if k != "_extra" and e.get("state") != PASS:
                        print("      %s %s %s" % (m, k, e))
    t0 = time.monotonic()
    eer_modes = "nbfm,bpsk,qpsk,usb" if quick else "cw,nbfm,bpsk,qpsk,qam16,ofdm,am,usb"
    eer = cmd_run(_sim_args(["--path", "eer", "--modes", eer_modes, "--out", os.path.join(tmp, "eer")]), quiet=True)
    check("EER good hardware converges to PASS", eer.final_status == PASS,
          "(%s, %d evaluations, %.0f s)" % (eer.final_status, eer.budget.trials, time.monotonic() - t0))
    if eer.final_status != PASS:
        for path, (p, v) in eer.results.items():
            for m, res in (v or {}).items():
                for k, e in res.items():
                    if k != "_extra" and e.get("state") != PASS:
                        print("      %s %s %s" % (m, k, e))

    thr_file = os.path.join(tmp, "strict.json")
    with open(thr_file, "w") as f:
        json.dump({"non_eer": {"qam16": {"evm_db": -30}}}, f)
    a_bad = _sim_args(["--modes", "qam16", "--thresholds", thr_file, "--stages", "S1,S3,S4,S5,S6", "--max-trials", "2000",
                       "--required-modes", "qam16", "--allow-hardware-limit", "none", "--out", os.path.join(tmp, "bad")])
    a_bad.simulate = "bad"
    bad = cmd_run(a_bad, quiet=True)
    bv = bad.results["non_eer"][1] if bad.results.get("non_eer") else None
    st = bv["qam16"]["evm_db"]["state"] if bv else None
    check("unreachable threshold ends as HARDWARE_LIMIT", bad.final_status == HARDWARE_LIMIT and st == HARDWARE_LIMIT,
          "(%s, %d evaluations)" % (bad.final_status, bad.budget.trials))
    if bv:
        check("policy: an allowed HARDWARE_LIMIT counts as PASS", path_state(bv, dict(required=["qam16"], allow_hl=["qam16"])) == PASS)
        check("policy: an optional mode does not decide the outcome", path_state(bv, dict(required=[], allow_hl=[])) == PASS)
    pa = build_parser()
    dflt = finalize_args(pa, pa.parse_args(["run", "--freq", "7M"]))
    check("policy: default bare-pin required modes leave QPSK/8PSK/16QAM/AM optional",
          set(dflt.policy["non_eer"]["required"]) == {"cw", "nbfm", "fsk", "afsk", "mfsk", "bpsk", "usb", "lsb", "iq_raw"}
          and dflt.policy["non_eer"]["allow_hl"] == ["qam16", "ofdm"] and set(dflt.policy["eer"]["required"]) == set(DEFAULT_MODES))

    print("profiles, receiver placement and bands")
    gprof = good.results["non_eer"][0]
    text = driver_profile_text(gprof, good.a)
    pf = os.path.join(tmp, "roundtrip.rpitx")
    old_umask = os.umask(0o002)
    try:
        with open_mode_644(pf) as f:
            f.write(text)
        jf = os.path.join(tmp, "umask.json")
        write_json_atomic(jf, dict(a=1))
        modes_ok = (os.stat(pf).st_mode & 0o777) == 0o644 and (os.stat(jf).st_mode & 0o777) == 0o644
    finally:
        os.umask(old_umask)
    check("profile files are 0644 under umask 002", modes_ok)
    victim = os.path.join(tmp, "victim.txt")
    with open(victim, "w") as f:
        f.write("keep")
    link = os.path.join(tmp, "link.md")
    if os.path.lexists(link):
        os.unlink(link)
    os.symlink(victim, link)
    try:
        open_mode_644(link).close()
        followed = True
    except OSError:
        followed = False
    with open(victim) as f:
        kept = f.read() == "keep"
    open_dir = os.path.join(tmp, "open_dir")
    os.makedirs(open_dir, exist_ok=True)
    os.chmod(open_dir, 0o777)
    try:
        trusted_output_dir(open_dir)
        open_accepted = True
    except SystemExit:
        open_accepted = False
    link_dir = os.path.join(tmp, "link_dir")
    if os.path.lexists(link_dir):
        os.unlink(link_dir)
    os.symlink(open_dir, link_dir)
    try:
        trusted_output_dir(link_dir)
        link_accepted = True
    except SystemExit:
        link_accepted = False
    check("output files and directories are never reached through symbolic links or open directories",
          not followed and kept and not open_accepted and not link_accepted)
    leftovers = [n for n in os.listdir(os.path.dirname(jf)) if n.endswith(".tmp")]
    check("JSON files are written through a temporary file that is removed", not leftovers)
    back = profile_from_driver_file(pf, good.a)
    pa_, pb_ = profile_params(gprof), profile_params(back)
    same = all(same_across([pa_[k], pb_[k]], 1e-9) for k in pa_ if k not in ("gain_constant", "rx_ppm"))
    check("driver profile file round trip", same and back["path"] == "non_eer")
    try:
        parse_driver_profile("ppm=1\nnot a line\n")
        bad_line = False
    except ValueError as e:
        bad_line = "line 2" in str(e)
    check("driver profile parser reports the malformed line", bad_line)
    for ext in (".rpitx", ".json"):
        va = _sim_verify_args(["--modes", modes, "--profile", os.path.join(tmp, "good", "profile_non_eer" + ext), "--apply", "driver-profile",
                               "--out", os.path.join(tmp, "verify_dp" + ext[1:])])
        old = sys.stdout
        sys.stdout = open(os.devnull, "w")
        try:
            rc = cmd_verify(va)
        finally:
            sys.stdout.close()
            sys.stdout = old
        check("verify --apply driver-profile (%s) reproduces PASS" % ext, rc == 0)
    pr = build_parser()
    ra = finalize_args(pr, pr.parse_args(["run", "--freq", "7M", "--rx-host", "remote", "--rx-remote", "10.0.0.2:55132",
                                          "--rx-args", "driver=rtlsdr,direct_samp=2"]))
    check("remote receiver arguments", rx_device_args(ra) == {"driver": "remote", "remote": "tcp://10.0.0.2:55132",
                                                             "remote:driver": "rtlsdr", "direct_samp": "2"})
    check("slow-USB boards recognised", slow_usb_board("Raspberry Pi 3 Model B Rev 1.2") and slow_usb_board("Raspberry Pi Zero W Rev 1.1")
          and slow_usb_board("Raspberry Pi Model B Plus Rev 1.2") and not slow_usb_board("Raspberry Pi 4 Model B Rev 1.4"))
    t0 = time.monotonic()
    mb = cmd_run(_sim_args(["--freqs", "144.39M,432.1M", "--modes", "usb", "--stages", "S0,S1,S3,S5", "--band-reuse", "skip",
                            "--check-driver-profile", "--out", os.path.join(tmp, "bands")]), quiet=True)
    try:
        with open(os.path.join(tmp, "bands", "bands.json")) as f:
            bj = json.load(f)
        cls = {k: v["cls"] for k, v in bj["paths"]["non_eer"].items()}
    except (OSError, KeyError):
        cls = {}
    second = mb.bands[-1][1] if getattr(mb, "bands", None) else None
    skipped = second is not None and {r["stage"] for r in second.stage_reports if r["status"].startswith("SKIPPED")} == {"S0", "S5"}
    check("multi-band run with --band-reuse skip", mb.final_status == PASS and skipped, "(%s, %.0f s)" % (mb.final_status, time.monotonic() - t0))
    check("band table: AM/PM table band-dependent, amplitude table and stream independent",
          cls.get("ampm_lut") == "dependent" and cls.get("amplitude_lut") == "independent" and cls.get("stream.slots_per_sample") == "independent",
          "(%s)" % {k: cls.get(k) for k in ("ampm_lut", "amplitude_lut", "stream.slots_per_sample")})
    dpst = [path_state(c.results["non_eer"][2], c.a.policy["non_eer"]) for _, c in mb.bands if len(c.results["non_eer"]) > 2] if second else []
    check("--check-driver-profile re-verifies every band through profile=", dpst == [PASS, PASS], "(%s)" % dpst)

    thr2 = os.path.join(tmp, "floor.json")
    with open(thr2, "w") as f:
        json.dump({"non_eer": {"qpsk": {"evm_db": -60, "ber": 0, "acpr_db": -30}}}, f)
    a_n = _sim_args(["--modes", "qpsk", "--thresholds", thr2, "--stages", "S1", "--out", os.path.join(tmp, "floor")])
    a_n.simulate = "noisy"
    nz = cmd_run(a_n, quiet=True)
    st = nz.results["non_eer"][1]["qpsk"]["evm_db"]["state"]
    check("threshold below the receiver floor is UNVERIFIABLE", st == UNVERIFIABLE, "(%s)" % st)

    lim = cmd_run(_sim_args(["--modes", "nbfm", "--max-trials", "7", "--out", os.path.join(tmp, "limit")]), quiet=True)
    check("--max-trials stops the run exactly at the limit", lim.final_status == "BUDGET_EXHAUSTED" and lim.budget.trials == 7,
          "(%s after %d)" % (lim.final_status, lim.budget.trials))

    rmodes = "nbfm,usb"
    full = cmd_run(_sim_args(["--modes", rmodes, "--stages", "S1,S3,S4", "--out", os.path.join(tmp, "full")]), quiet=True)
    part_dir = os.path.join(tmp, "part")
    half = max(full.budget.trials // 2, 1)
    cmd_run(_sim_args(["--modes", rmodes, "--stages", "S1,S3,S4", "--max-trials", str(half), "--out", part_dir]), quiet=True)
    res = cmd_run(_sim_args(["--modes", rmodes, "--stages", "S1,S3,S4", "--resume", os.path.join(part_dir, "checkpoint.json"),
                             "--out", part_dir]), quiet=True)
    same = full.final_status == res.final_status and full.results["non_eer"][0] == res.results["non_eer"][0]
    check("--resume reproduces the uninterrupted run", same, "(%s / %s)" % (full.final_status, res.final_status))

    a_r = _sim_args(["--modes", "nbfm", "--stages", "S0", "--max-rounds", "0", "--out", os.path.join(tmp, "robust")])
    a_r.simulate = "robust-bad"
    rb = cmd_run(a_r, quiet=True)
    s0 = [s for s in rb.stage_reports if s["stage"] == "S0"]
    check("robustness ladder clears the simulated underflows", bool(s0) and s0[0]["status"] == PASS,
          "(%s)" % (s0[0]["changed"] if s0 else "-"))

    print("self test %s (outputs in %s)" % ("passed" if ok_all else "FAILED", tmp))
    return 0 if ok_all else 1


# ---------------------------------------------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------------------------------------------


def add_bench_options(p):
    p.add_argument("--freq", type=parse_frequency, help="transmit frequency (e.g. 144.39M); required")
    p.add_argument("--confirm-dummy-load", action="store_true", help="confirms that a dummy load and an attenuator are connected")
    p.add_argument("--simulate", choices=sorted(SIM_SCENARIOS), default=None, help="use the simulated bench instead of the radios")
    p.add_argument("--path", choices=("non-eer", "eer", "both"), default="non-eer", help="transmit path(s) to tune")
    p.add_argument("--envelope-output", choices=("pcm", "pwm"), default="pcm", help="envelope DAC of the EER path")
    p.add_argument("--pwm-gpio", type=int, default=18, help="pin of the PWM envelope output (12 or 18)")
    p.add_argument("--pwm-bits", type=int, default=8, help="resolution of the PWM envelope (6-12)")
    p.add_argument("--rf-dac-available", action="store_true", help="GPIO20 is wired as the second clock pin (rf_dac)")
    p.add_argument("--modes", default=",".join(DEFAULT_MODES), help="modes to verify: " + ",".join(USER_MODES))
    p.add_argument("--rate", type=float, default=48000, help="transmit sample rate")
    p.add_argument("--harmonic", type=int, default=1, help="harmonic setting of the driver")
    p.add_argument("--ppm", type=float, default=0.0, help="initial crystal correction of the transmitter")
    p.add_argument("--dev-args", default="", help="extra rpitx device arguments")
    p.add_argument("--stream-args", default="", help="initial rpitx stream arguments, e.g. 'dma_channel=7'")
    p.add_argument("--rx-args", default="driver=rtlsdr", help="SoapySDR receiver device arguments")
    p.add_argument("--rx-host", choices=("local", "remote"), default="local",
                   help="receiver on this host, or on another host through SoapyRemote (--rx-remote)")
    p.add_argument("--rx-remote", default=None, help="SoapyRemote server of the receiver, e.g. tcp://192.168.1.10:55132")
    p.add_argument("--rx-local-policy", choices=("warn", "refuse", "ignore"), default="warn",
                   help="what to do with a local USB receiver on a Pi Zero / 1 / 2 / 3 or CM1 / CM3")
    p.add_argument("--rx-rate", type=float, default=1.024e6, help="receiver sample rate")
    p.add_argument("--rx-offset", type=parse_frequency, default=150e3, help="receiver tuned this many Hz below the carrier")
    p.add_argument("--rx-gain", default="auto", help="receiver gain in dB, or 'auto'")
    p.add_argument("--rx-ppm", type=float, default=0.0, help="frequency correction of the receiver")
    p.add_argument("--freq-ref", choices=("rx", "tx", "external"), default="rx", help="trusted clock for the frequency stage")
    p.add_argument("--ref-freq", type=parse_frequency, default=None, help="external reference carrier (with --freq-ref external)")
    p.add_argument("--rx-sfdr", type=float, default=45.0, help="spurious-free dynamic range of the receiver in dB (measurement floor)")
    p.add_argument("--floor-margin", type=float, default=6.0, help="margin between a threshold and the floor for it to be verifiable, dB")
    p.add_argument("--thresholds", default=None, help="JSON file overriding the default thresholds")
    p.add_argument("--threshold-set", choices=("auto", "non-eer", "eer"), default="auto",
                   help="threshold table: the one of each path, or the same table for every path")
    p.add_argument("--required-modes", default="default",
                   help="bare-pin modes that decide the outcome: default (constant-envelope, BPSK, SSB, iq_raw), all, none or a list")
    p.add_argument("--required-modes-eer", default="default", help="EER modes that decide the outcome: default (all), all, none or a list")
    p.add_argument("--allow-hardware-limit", default=DEFAULT_ALLOW_HL["non_eer"],
                   help="bare-pin modes whose HARDWARE_LIMIT counts as passed: a list, all or none")
    p.add_argument("--allow-hardware-limit-eer", default=DEFAULT_ALLOW_HL["eer"],
                   help="EER modes whose HARDWARE_LIMIT counts as passed: a list, all or none")
    p.add_argument("--profile-format", choices=("args", "driver", "both"), default="both",
                   help="profile outputs: argument strings (.args), driver profile file (.rpitx, profile=<file>), or both")
    p.add_argument("--profile-gain", choices=("envelope", "constant"), default="envelope",
                   help="gain stored in the driver profile file: the one of the envelope or of the constant-envelope modes")
    p.add_argument("--profile-amplitude-mode", choices=("none", "envelope", "constant"), default="none",
                   help="amplitude_mode stored in the driver profile file (none: left to the application)")
    p.add_argument("--repeats", type=int, default=3, help="bursts per evaluation")
    p.add_argument("--verify-repeats", type=int, default=5, help="bursts per mode in the verification")
    p.add_argument("--payload-seconds", type=float, default=1.0, help="payload length of a test burst")
    p.add_argument("--robust-seconds", type=float, default=30.0, help="length of the robustness burst")
    p.add_argument("--duty", type=float, default=0.5, help="largest transmit duty cycle")
    p.add_argument("--max-trials", type=int, default=400, help="largest number of evaluations")
    p.add_argument("--max-minutes", type=float, default=90.0, help="largest wall-clock time")
    p.add_argument("--max-tx-seconds", type=float, default=1800.0, help="largest accumulated time on air")
    p.add_argument("--out", default="rpitx_autotune_results", help="output directory")
    p.add_argument("--save-captures", choices=("none", "fail", "all"), default="fail", help="captures saved for replay")
    p.add_argument("--seed", type=int, default=1, help="seed of the test payloads")
    p.add_argument("--allow-unverifiable", action="store_true", help="exit 0 when the only non-PASS states are UNVERIFIABLE")


def build_parser():
    p = argparse.ArgumentParser(description="Closed-loop over-the-air test and tuning of the SoapySDR rpitx driver.",
                                formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    sub = p.add_subparsers(dest="cmd")
    r = sub.add_parser("run", help="tune and verify", formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    add_bench_options(r)
    r.add_argument("--stages", default=",".join(STAGE_LIMITS), help="stages to run")
    r.add_argument("--stage-limit", default="", help="per-stage evaluation limits, e.g. 'S4=40,S5=20'")
    r.add_argument("--max-rounds", type=int, default=4, help="closed-loop verification rounds")
    r.add_argument("--min-delta", type=float, default=0.3, help="smallest score improvement accepted (dB)")
    r.add_argument("--sigma-k", type=float, default=2.0, help="improvement must exceed this many standard deviations")
    r.add_argument("--drive-margin", type=float, default=0.5, help="drive level within this many dB of the maximum output (S2)")
    r.add_argument("--dwell", type=float, default=0.2, help="seconds per state of the table measurements (S3)")
    r.add_argument("--eer-points", type=int, default=17, help="points of the EER tables (2-64)")
    r.add_argument("--delay-range", default=None, help="envelope_delay search range 'lo,hi' (default -2,2 or -8,8 for EER)")
    r.add_argument("--max-slots", type=int, default=4, help="largest slots_per_sample tried (S5)")
    r.add_argument("--resume", default=None, help="checkpoint to resume from, or 'auto' (the checkpoint of every output directory, if any)")
    r.add_argument("--tune-optional", choices=("yes", "no"), default="yes", help="also re-tune for failing optional modes")
    r.add_argument("--check-driver-profile", action="store_true",
                   help="after tuning, verify again through profile=<file> of the driver (needs the driver profile support)")
    r.add_argument("--freqs", default=None, help="list of frequencies (e.g. 7.1M,14.2M,144.39M): one tuning per band and a band table")
    r.add_argument("--band-reuse", choices=("none", "start", "skip"), default="none",
                   help="per band: start from the defaults, from the profile of the previous band, or from it skipping "
                        "the --band-independent-stages")
    r.add_argument("--band-independent-stages", default="S0,S5", help="stages skipped on later bands with --band-reuse skip")
    r.add_argument("--band-tolerance", type=float, default=0.05,
                   help="parameters agree across bands when they differ by at most this times max(1, |value|)")
    r.add_argument("--budget-scope", choices=("band", "run"), default="band", help="limits of the budget per band or for the whole run")
    v = sub.add_parser("verify", help="verify a saved profile", formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    add_bench_options(v)
    v.add_argument("--profile", required=True, help="profile_<path>.json or profile_<path>.rpitx written by run")
    v.add_argument("--apply", choices=("settings", "driver-profile"), default="settings",
                   help="apply the profile with writeSetting(), or open the driver with profile=<.rpitx file>")
    rp = sub.add_parser("replay", help="re-analyse saved captures (no radio)", formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    rp.add_argument("--out", default="rpitx_autotune_results", help="output directory of the run")
    st = sub.add_parser("self-test", help="checks on simulated hardware (no radio)", formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    st.add_argument("--quick", action="store_true", help="shorter test")
    st.add_argument("--keep", default=None, help="directory for the outputs of the test runs")
    return p


def parse_mode_set(p, text, selected, default, option, must_be_selected):
    t = text.strip().lower()
    if t == "default":
        return [m for m in selected if m in default]
    if t == "all":
        return list(selected) if must_be_selected else list(USER_MODES)
    if t == "none":
        return []
    out = [m.strip() for m in text.split(",") if m.strip()]
    for m in out:
        if m not in USER_MODES:
            p.error("%s: unknown mode '%s'" % (option, m))
        if must_be_selected and m not in selected:
            p.error("%s: mode '%s' is not in --modes" % (option, m))
    return out


def finalize_args(p, a):
    """Validates and completes the options of run / verify before any radio is opened."""
    if getattr(a, "freqs", None):
        try:
            a.freq_list = [parse_frequency(f) for f in a.freqs.split(",") if f.strip()]
        except argparse.ArgumentTypeError as e:
            p.error("--freqs: %s" % e)
        if not a.freq_list:
            p.error("--freqs is empty")
        if len(set(a.freq_list)) != len(a.freq_list):
            p.error("--freqs: repeated frequency")
        a.freq = a.freq_list[0]
    elif a.freq is None:
        p.error("--freq (or --freqs) is required")
    else:
        a.freq_list = [a.freq]
    if a.rx_host == "remote":
        if not a.rx_remote:
            p.error("--rx-host remote needs --rx-remote tcp://<host>:<port>")
        if "://" not in a.rx_remote:
            a.rx_remote = "tcp://" + a.rx_remote
    parse_kwargs(a.rx_args, "--rx-args")
    parse_kwargs(a.dev_args, "--dev-args")
    parse_kwargs(a.stream_args, "--stream-args")
    a.mode_list = [m.strip() for m in a.modes.split(",") if m.strip()]
    for m in a.mode_list:
        if m not in USER_MODES:
            p.error("unknown mode '%s' (choose from %s)" % (m, ",".join(USER_MODES)))
    a.path_list = {"non-eer": ["non_eer"], "eer": ["eer"], "both": ["non_eer", "eer"]}[a.path]
    if not 10000 <= a.rate <= MAX_SLOT_RATE:
        p.error("--rate must be 10 kHz - 200 kHz")
    if not 1 <= a.harmonic <= 15:
        p.error("--harmonic must be 1 - 15")
    if a.freq_ref == "external" and a.ref_freq is None:
        p.error("--freq-ref external needs --ref-freq")
    if a.rx_gain != "auto":
        try:
            float(a.rx_gain)
        except ValueError:
            p.error("--rx-gain must be a number or 'auto'")
    if not 0 < a.duty <= 1:
        p.error("--duty must be in (0, 1]")
    for name in ("repeats", "verify_repeats", "max_trials"):
        if getattr(a, name) < 1:
            p.error("--%s must be at least 1" % name.replace("_", "-"))
    a.thresholds_data = {}
    if a.thresholds:
        with open(a.thresholds) as f:
            a.thresholds_data = json.load(f)
    a.stage_list = [s.strip() for s in getattr(a, "stages", ",".join(STAGE_LIMITS)).split(",") if s.strip()]
    for s in a.stage_list:
        if s not in STAGE_LIMITS:
            p.error("unknown stage '%s'" % s)
    a.stage_limits = parse_stage_limits(getattr(a, "stage_limit", ""))
    for name, default in (("max_rounds", 0), ("min_delta", 0.3), ("sigma_k", 2.0), ("drive_margin", 0.5), ("dwell", 0.2),
                          ("eer_points", 17), ("delay_range", None), ("max_slots", 4), ("resume", None), ("tune_optional", "yes"),
                          ("check_driver_profile", False), ("band_reuse", "none"), ("band_independent_stages", "S0,S5"),
                          ("band_tolerance", 0.05), ("budget_scope", "band"), ("apply", "settings")):
        if not hasattr(a, name):
            setattr(a, name, default)
    if a.resume and a.resume != "auto" and len(a.freq_list) > 1:
        p.error("--resume with --freqs must be 'auto' (every band resumes from its own checkpoint)")
    a.band_stage_list = [s.strip() for s in a.band_independent_stages.split(",") if s.strip()]
    for s in a.band_stage_list:
        if s not in STAGE_LIMITS:
            p.error("--band-independent-stages: unknown stage '%s'" % s)
    if not a.band_tolerance >= 0:
        p.error("--band-tolerance must not be negative")
    a.policy = {}
    for path, rq, hl, sfx in (("non_eer", a.required_modes, a.allow_hardware_limit, ""),
                              ("eer", a.required_modes_eer, a.allow_hardware_limit_eer, "-eer")):
        a.policy[path] = dict(required=parse_mode_set(p, rq, a.mode_list, DEFAULT_REQUIRED[path], "--required-modes" + sfx, True),
                              allow_hl=parse_mode_set(p, hl, a.mode_list, [], "--allow-hardware-limit" + sfx, False))
    if not 2 <= a.eer_points <= 64:
        p.error("--eer-points must be 2 - 64")
    if isinstance(a.delay_range, str):
        try:
            lo, hi = (float(x) for x in a.delay_range.split(","))
        except ValueError:
            p.error("--delay-range must be 'lo,hi'")
        a.delay_range = (lo, hi)
    if a.envelope_output == "pwm" and (a.pwm_gpio not in (12, 18) or not 6 <= a.pwm_bits <= 12):
        p.error("--pwm-gpio must be 12 or 18 and --pwm-bits 6 - 12")
    return a


def main():
    p = build_parser()
    a = p.parse_args()
    if a.cmd == "self-test":
        return self_test(a.quick, a.keep)
    if a.cmd == "replay":
        return cmd_replay(a)
    if a.cmd not in ("run", "verify"):
        p.print_help()
        return 2
    finalize_args(p, a)
    if a.cmd == "verify":
        return cmd_verify(a)
    ctx = cmd_run(a)
    st = ctx.final_status
    return 0 if st == PASS or (st == UNVERIFIABLE and a.allow_unverifiable) else 1


if __name__ == "__main__":
    sys.exit(main())
