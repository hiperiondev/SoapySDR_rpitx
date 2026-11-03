#!/usr/bin/env python3
# Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
# Project Site: https://github.com/hiperiondev/SoapySDR_rpitx
#
# This is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3, or (at your option)
# any later version.
#
# Signal test for the rpitx SoapySDR driver. Transmits either
# - an NBFM test tone (default, constant envelope), or
# - a WAV file (--wav), modulated as FM, AM, USB or LSB, or sent as I/Q when the file is a stereo
#   I/Q recording (--mod iq: left = I, right = Q).
# The driver counters are printed once per second. Receive it with a second receiver and look for dropouts.
# Run as root, into a dummy load or with a band filter, on a frequency you are licensed for.
#
# --seconds 0 only opens, activates and closes a stream: opening recovers a transmitter left running by a
# client that was killed while transmitting (the rpitx_reset tool does the same without SoapySDR).

import argparse
import signal
import sys
import time
import wave

import numpy as np

MODES = ("fm", "am", "usb", "lsb", "iq")


def parse_frequency(text):
    """Parses '144.8M', '7074k', '28.074e6' or '145500000' into Hz."""
    t = text.strip().lower().replace("hz", "")
    mult = 1.0
    if t and t[-1] in "kmg":
        mult = {"k": 1e3, "m": 1e6, "g": 1e9}[t[-1]]
        t = t[:-1]
    try:
        return float(t) * mult
    except ValueError:
        raise argparse.ArgumentTypeError("invalid frequency '%s'" % text)


def read_wav(path):
    """Reads a PCM WAV file (8/16/24/32-bit integer). Returns (samples[n, channels] in [-1, 1), rate)."""
    try:
        w = wave.open(path, "rb")
    except wave.Error as e:
        raise SystemExit("cannot read %s: %s (only integer PCM WAV is supported; convert float WAV with "
                         "'sox in.wav -b 16 out.wav')" % (path, e))
    with w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)

    if width == 1:
        x = (np.frombuffer(raw, np.uint8).astype(np.float32) - 128.0) / 128.0
    elif width == 2:
        x = np.frombuffer(raw, "<i2").astype(np.float32) / 32768.0
    elif width == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)
        x = v.astype(np.float32) / 8388608.0
    elif width == 4:
        x = np.frombuffer(raw, "<i4").astype(np.float32) / 2147483648.0
    else:
        raise SystemExit("unsupported WAV sample width: %d bytes" % width)
    return x.reshape(-1, ch), rate


def resample(x, rate_in, rate_out):
    """Linear-interpolation resampler (adequate for speech/test audio and for I/Q at similar rates)."""
    if rate_in == rate_out or len(x) == 0:
        return x
    n_out = int(round(len(x) * rate_out / rate_in))
    t_out = np.arange(n_out) * (rate_in / rate_out)
    t_in = np.arange(len(x))
    if np.iscomplexobj(x):
        return (np.interp(t_out, t_in, x.real) + 1j * np.interp(t_out, t_in, x.imag)).astype(np.complex64)
    return np.interp(t_out, t_in, x).astype(np.float32)


def band_limit(x, fs, lo, hi):
    """Zero-phase FFT band-pass of a real signal; returns the real result."""
    spec = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / fs)
    spec[(f < lo) | (f > hi)] = 0
    return np.fft.irfft(spec, len(x)).astype(np.float32)


def modulate(audio, fs, a):
    """Turns mono audio at fs into complex baseband for the selected mode."""
    m = band_limit(audio, fs, a.audio_low if a.mod in ("usb", "lsb") else 0.0, a.audio_high)
    peak = np.max(np.abs(m)) if len(m) else 0.0
    if a.normalize and peak > 0:
        m = m / peak
    m = np.clip(m * a.audio_gain, -1.0, 1.0)

    if a.mod == "fm":
        phase = 2 * np.pi * a.dev * np.cumsum(m, dtype=np.float64) / fs
        return np.exp(1j * phase).astype(np.complex64)
    if a.mod == "am":
        return ((1.0 + a.am_depth * m) / (1.0 + a.am_depth)).astype(np.complex64)

    # SSB: analytic signal (positive frequencies only = USB); its conjugate is LSB.
    n = len(m)
    spec = np.fft.fft(m)
    h = np.zeros(n)
    h[0] = 1
    if n % 2 == 0:
        h[1 : n // 2] = 2
        h[n // 2] = 1
    else:
        h[1 : (n + 1) // 2] = 2
    s = np.fft.ifft(spec * h).astype(np.complex64)
    if a.mod == "lsb":
        s = np.conj(s)
    peak = np.max(np.abs(s)) if n else 0.0
    return (s / peak).astype(np.complex64) if peak > 0 else s


def load_baseband(a):
    """Builds the complex baseband to transmit from the WAV file."""
    x, rate = read_wav(a.wav)
    print("%s: %d Hz, %d channel(s), %.1f s" % (a.wav, rate, x.shape[1], len(x) / rate))

    if a.mod == "iq":
        if x.shape[1] < 2:
            raise SystemExit("--mod iq needs a stereo WAV (left = I, right = Q)")
        iq = (x[:, 0] + 1j * x[:, 1]).astype(np.complex64)
        if a.swap_iq:
            iq = (x[:, 1] + 1j * x[:, 0]).astype(np.complex64)
        iq = resample(iq, rate, a.rate)
        if a.audio_gain != 1.0:
            iq = iq * np.float32(a.audio_gain)
        return iq

    audio = resample(x.mean(axis=1), rate, a.rate)
    return modulate(audio, a.rate, a)


def tone_block(n0, count, a):
    """NBFM test tone: phase(t) = -(dev / tone) * cos(2 pi tone t), |s| = 1."""
    k = n0 + np.arange(count)
    return np.exp(1j * (-(a.dev / a.tone) * np.cos(2 * np.pi * a.tone * k / a.rate))).astype(np.complex64)


def main():
    p = argparse.ArgumentParser(description="rpitx test transmitter: NBFM tone or WAV file",
                                formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    p.add_argument("--freq", type=parse_frequency, required=True, help="output frequency (e.g. 144.8M, 7074k, 145500000)")
    p.add_argument("--rate", type=parse_frequency, default=48000, help="transmit sample rate (e.g. 48000, 96k)")
    p.add_argument("--wav", default=None, help="WAV file to transmit instead of the test tone")
    p.add_argument("--mod", choices=MODES, default="fm",
                   help="WAV modulation: fm, am, usb, lsb, or iq (stereo WAV holding I/Q)")
    p.add_argument("--loop", action="store_true", help="repeat the WAV file until --seconds or Ctrl-C")
    p.add_argument("--tone", type=float, default=1000, help="test tone frequency in Hz (no --wav)")
    p.add_argument("--dev", type=float, default=2500, help="FM peak deviation in Hz (tone and --mod fm)")
    p.add_argument("--am-depth", type=float, default=0.8, help="AM modulation depth 0-1")
    p.add_argument("--audio-low", type=float, default=300, help="SSB audio low cut in Hz")
    p.add_argument("--audio-high", type=float, default=3000, help="audio high cut in Hz (fm, am, usb, lsb)")
    p.add_argument("--audio-gain", type=float, default=1.0, help="gain applied to the audio (or I/Q) after normalisation")
    p.add_argument("--no-normalize", dest="normalize", action="store_false", help="do not normalise the audio peak to full scale")
    p.add_argument("--swap-iq", action="store_true", help="swap I and Q of a --mod iq WAV")
    p.add_argument("--amplitude-mode", choices=("auto", "envelope", "constant"), default="auto",
                   help="rpitx amplitude mode; auto = constant for tone/fm, envelope for am/usb/lsb/iq")
    p.add_argument("--block", type=int, default=512, help="samples per writeStream() call")
    p.add_argument("--seconds", type=float, default=None,
                   help="duration (default: 30 s for the tone, one pass for a WAV, endless with --loop; 0 = reset only)")
    p.add_argument("--gain", type=float, default=7, help="pad drive level 0-7")
    p.add_argument("--jitter-ms", type=float, default=0, help="mean random extra delay per block, emulates a busy host")
    p.add_argument("--args", default="", help="extra stream arguments, e.g. 'preroll_ms=200,fifo_ms=500'")
    a = p.parse_args()

    baseband = None
    if a.wav and (a.seconds is None or a.seconds > 0):
        baseband = load_baseband(a)
        if len(baseband) == 0:
            raise SystemExit("the WAV file is empty")

    if a.seconds is None:
        a.seconds = float("inf") if (baseband is not None and a.loop) else (
            len(baseband) / a.rate if baseband is not None else 30.0)

    mode = a.amplitude_mode
    if mode == "auto":
        mode = "constant" if (baseband is None or a.mod == "fm") else "envelope"

    import SoapySDR
    from SoapySDR import SOAPY_SDR_CF32, SOAPY_SDR_END_BURST, SOAPY_SDR_TIMEOUT, SOAPY_SDR_TX

    sdr = SoapySDR.Device(dict(driver="rpitx", amplitude_mode=mode))
    sdr.setSampleRate(SOAPY_SDR_TX, 0, a.rate)
    sdr.setFrequency(SOAPY_SDR_TX, 0, a.freq)
    sdr.setGain(SOAPY_SDR_TX, 0, a.gain)

    stream_args = dict(kv.split("=", 1) for kv in a.args.split(",") if kv)
    st = sdr.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, [0], stream_args)
    sdr.activateStream(st)

    if a.seconds <= 0:
        sdr.deactivateStream(st)
        sdr.closeStream(st)
        print("transmitter reset")
        return 0

    def terminate(signum, frame):
        raise KeyboardInterrupt

    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, terminate)

    if baseband is not None:
        print("transmitting %s (%s, amplitude mode %s) on %.6f MHz" % (a.wav, a.mod, mode, a.freq / 1e6))
    else:
        print("transmitting a %.0f Hz NBFM tone on %.6f MHz" % (a.tone, a.freq / 1e6))

    total = int(a.seconds * a.rate) if np.isfinite(a.seconds) else None
    n = 0
    pos = 0
    t0 = time.time()
    next_report = t0 + 1
    rng = np.random.default_rng()
    failed = False
    try:
        while not failed and (total is None or n < total):
            count = a.block if total is None else min(a.block, total - n)
            if baseband is None:
                x = tone_block(n, count, a)
            else:
                if pos >= len(baseband):
                    if not a.loop:
                        break
                    pos = 0
                x = baseband[pos : pos + count]
                pos += len(x)
                count = len(x)
            sent = 0
            while sent < count:
                sr = sdr.writeStream(st, [x[sent:]], count - sent, timeoutUs=1000000)
                if sr.ret == SOAPY_SDR_TIMEOUT:
                    continue
                if sr.ret < 0:
                    print("writeStream error:", sr.ret, file=sys.stderr)
                    failed = True
                    break
                sent += sr.ret
            n += sent
            if a.jitter_ms > 0:
                time.sleep(rng.exponential(a.jitter_ms / 1000.0))
            if time.time() >= next_report:
                next_report += 1
                print("fifo %5s%%  underflows %s  late %s  pll_locked %s" % (
                    sdr.readSensor("fifo_fill"), sdr.readSensor("underflows"), sdr.readSensor("late"),
                    sdr.readSensor("pll_locked")))
    except KeyboardInterrupt:
        pass

    # END_BURST lets the queued samples finish before the carrier is turned off.
    sdr.writeStream(st, [np.zeros(0, np.complex64)], 0, SOAPY_SDR_END_BURST)
    sdr.deactivateStream(st)
    sdr.closeStream(st)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
