#!/usr/bin/env python3
# Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
# Project Site: https://github.com/hiperiondev/SoapySDR_rpitx
#
# This is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3, or (at your option)
# any later version.
#
# HPSDR <-> rpitx bridge.
#
# Emulates an openHPSDR Protocol 1 radio (Metis, Hermes, Hermes II, Angelia, Orion, Orion II or Hermes-Lite 2)
# so that SDR host programs (piHPSDR, Thetis, PowerSDR mRX, linHPSDR, Quisk, SparkSDR, ...) discover it and use
# it as a radio. The TX I/Q stream of the host is transmitted with the SoapySDR rpitx driver (GPIO4). With --rx
# (for example --rx rtlsdr) the driver opens the receiver as its RX channel and the bridge works as a transceiver:
# the receiver follows the host RX frequencies, its samples are down-converted and decimated to the host receive
# sample rate and sent as the receiver I/Q of EP6; with --relay-gpio the driver switches an antenna T/R relay and
# mutes the receiver while transmitting, and with --ptt-out-gpio it keys an amplifier or radio PTT line after the
# relay has settled (--relay-tx-delay-ms), before the first TX sample (--ptt-tx-delay-ms).
#
# Protocol coverage:
#   - discovery over UDP (broadcast and directed) and over TCP, with the board specific reply layout;
#   - start/stop of the receive I/Q stream (EP6) and of the wideband stream (EP4), over UDP or TCP;
#   - decoding of every command and control (C&C) address of the host stream (EP2);
#   - EP6 status words: PTT/dot/dash, IO inputs (all inactive), firmware version, forward/reverse power,
#     supply voltage, ADC overload (clipping of the receiver), Hermes-Lite 2 temperature, TX FIFO state and request
#     acknowledges;
#   - receiver I/Q (with --rx): every host receiver is a digital down-converter of the receiver capture, centred
#     on its own frequency (receivers outside the capture bandwidth send zeros); the capture is retuned only when
#     RX1 leaves it, and is offset from RX1 to keep the receiver DC spur out of the band; optional host control
#     of the receiver gain (attenuator / LNA gain);
#   - transmission of the host TX I/Q while the host asserts MOX, on the TX NCO frequency (or on the RX1
#     frequency when the host clears the duplex bit);
#   - radio side CW: internal iambic/straight keyer fed by optional GPIO paddles, Hermes-Lite 2 CWX keying
#     carried in the I sample LSB, and a PTT input;
#   - host watchdog: streaming stops and the transmitter is released when the host goes silent;
#   - access control: the bridge listens on loopback by default (--bind), only hosts of the --allow networks are
#     answered (default: loopback, private and link-local addresses), only the host that started the stream can key
#     the transmitter, and while a stream is active a start from another address is refused unless --takeover is
#     given. Protocol 1 has no authentication: access is decided by the source address only, which on a shared
#     network can be spoofed; bind to the address of a trusted interface and keep --allow narrow.
# Firmware programming, flash erase and IP address change requests are recognised and ignored.
#
# Protocol 1 hosts pace their TX stream on the receive stream of the radio (one microphone sample per receive
# sample at 48 kHz), so the bridge sends EP6 packets (silent microphone) at the receive sample rate requested by
# the host: paced by the receiver clock with --rx, otherwise paced by the system clock with noise or zeros as
# receiver I/Q.
#
# Run as root on the Raspberry Pi, into a dummy load or through a band filter, on a frequency you are licensed for.

import argparse
import collections
import ipaddress
import math
import os
import queue
import select
import signal
import socket
import struct
import sys
import threading
import time
import zlib

import numpy as np

HPSDR_PORT = 1024
PACKET_LEN = 1032
FRAME_LEN = 512
SYNC = b"\x7f\x7f\x7f"
TX_RATE = 48000  # Protocol 1 TX I/Q and microphone samples are always 48 kHz
SAMPLES_PER_EP2_FRAME = 63
# EP2 sequence tracking: packets that arrive up to this many sequence numbers late are reordered ones; EP2_RESYNC_LATE
# of them in a row mean that the host restarted its sequence.
EP2_REORDER_WINDOW = 256
EP2_RESYNC_LATE = 8
RX_RATES = (48000, 96000, 192000, 384000)
EP4_SAMPLES = 512  # 16-bit wideband samples per EP4 packet
EP4_PACKETS_PER_BLOCK = 32
MAX_TCP_CLIENTS = 4  # simultaneous Protocol 1 TCP connections


class Board:
    """Identity of an emulated board as reported in discovery replies and EP6 status words."""

    def __init__(self, name, code, firmware, max_rx, adc_count=1, hl2=False, metis=False):
        self.name = name
        self.code = code
        self.firmware = firmware
        self.max_rx = max_rx
        self.adc_count = adc_count
        self.hl2 = hl2
        self.metis = metis


BOARDS = {
    "metis": Board("Metis", 0x00, 26, 4, metis=True),
    "hermes": Board("Hermes", 0x01, 32, 4),
    "hermes2": Board("Hermes II", 0x02, 32, 2),
    "griffin": Board("Hermes II", 0x02, 32, 2),
    "angelia": Board("Angelia", 0x04, 21, 7, adc_count=2),
    "orion": Board("Orion", 0x05, 18, 7, adc_count=2),
    "orion2": Board("Orion II", 0x0A, 21, 7, adc_count=2),
    "hermeslite": Board("Hermes-Lite 2", 0x06, 73, 4, hl2=True),
}


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


def parse_kwargs(text, option):
    """Parses 'key=value,key=value' into a dict; exits with a message naming @option for a malformed item."""
    out = {}
    for item in filter(None, (s.strip() for s in text.split(","))):
        key, sep, value = item.partition("=")
        if not sep or not key.strip():
            raise SystemExit("%s: invalid item '%s' (expected key=value)" % (option, item))
        out[key.strip()] = value.strip()
    return out


def parse_allow(text):
    """Parses the --allow list: 'any', 'private', or comma-separated addresses / networks (CIDR).

    Returns None for 'any', otherwise a list whose items are ip_network objects or the string 'private'.
    """
    items = [t.strip() for t in text.split(",") if t.strip()]
    if not items:
        raise argparse.ArgumentTypeError("empty --allow list")
    if "any" in items:
        return None
    out = []
    for item in items:
        if item == "private":
            out.append(item)
            continue
        try:
            out.append(ipaddress.ip_network(item, strict=False))
        except ValueError:
            raise argparse.ArgumentTypeError("invalid address or network '%s' in --allow" % item)
    return out


def bind_is_loopback(host):
    """Tells whether a --bind value only accepts local connections."""
    if host == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def address_allowed(allow, host):
    """Tells whether @host (an IP address string) belongs to the parsed --allow list @allow."""
    if allow is None:
        return True
    try:
        ip = ipaddress.ip_address(host)
    except ValueError:
        return False
    for item in allow:
        if item == "private":
            if ip.is_private or ip.is_loopback or ip.is_link_local:
                return True
        elif ip.version == item.version and ip in item:
            return True
    return False


def parse_gain(text):
    """Parses --rx-gain: 'auto' or a gain in dB."""
    if text.strip().lower() == "auto":
        return "auto"
    try:
        return float(text)
    except ValueError:
        raise argparse.ArgumentTypeError("invalid gain '%s' (a value in dB or 'auto')" % text)


def parse_mac(text):
    """Parses 'aa:bb:cc:dd:ee:ff' (or '-' separated) into 6 bytes."""
    parts = text.replace("-", ":").split(":")
    try:
        mac = bytes(int(p, 16) for p in parts)
    except ValueError:
        mac = b""
    if len(mac) != 6:
        raise argparse.ArgumentTypeError("invalid MAC address '%s'" % text)
    return mac


def default_mac():
    """Returns a locally administered MAC derived from the host name (stable across runs)."""
    h = zlib.crc32(socket.gethostname().encode()) & 0xFFFFFFFF
    return bytes([0x02, 0x00]) + struct.pack(">I", h)


def cpu_temperature():
    """Returns the SoC temperature in degrees Celsius, or None when it cannot be read."""
    try:
        with open("/sys/class/thermal/thermal_zone0/temp") as f:
            return int(f.read().strip()) / 1000.0
    except (OSError, ValueError):
        return None


# ---------------------------------------------------------------------------------------------------------------
# Host command and control (EP2 C0..C4)
# ---------------------------------------------------------------------------------------------------------------


class ControlState:
    """Decoded host command and control state.

    Every C&C word is stored raw in `words` (address -> (C1, C2, C3, C4)); the fields used by the bridge and
    the fields shown by --verbose are decoded into attributes. `apply()` returns the names of the decoded fields
    whose value changed.
    """

    def __init__(self, board):
        self.board = board
        self.lock = threading.Lock()
        self.words = {}
        self.mox = False
        # Address 0x00.
        self.rx_rate = 48000
        self.ref_10mhz = 0
        self.clock_122mhz = 0
        self.config = 0
        self.mic_source = 0
        self.class_e = 0
        self.oc_outputs = 0
        self.alex_attenuator = 0
        self.preamp = 0
        self.dither = 0
        self.random = 0
        self.alex_rx_antenna = 0
        self.alex_rx_out = 0
        self.alex_tx_relay = 0
        self.duplex = 1
        self.num_rx = 1
        self.timestamp = 0
        self.common_frequency = 0
        # Addresses 0x01..0x08 (and 0x12..0x16 on the Hermes-Lite 2).
        self.tx_freq = None
        self.rx_freq = [None] * 12
        # Address 0x09.
        self.drive = 255
        self.mic_boost = 0
        self.line_in = 0
        self.apollo = 0
        self.alex_manual = 0
        self.vna = 0
        self.alex_hpf = 0
        self.alex_lpf = 0
        self.alex_tr_disable = 0
        self.pa_enable = 0
        self.tune_request = 0
        # Address 0x0A.
        self.preamps = 0
        self.mic_ptt_tip = 0
        self.mic_bias = 0
        self.mic_ptt_disable = 0
        self.line_in_gain = 0
        self.puresignal = 0
        self.user_outputs = 0
        self.adc1_attenuator = 0
        self.lna_gain = 0
        # Address 0x0B.
        self.adc2_attenuator = 0
        self.adc3_attenuator = 0
        self.cw_reversed = 0
        self.cw_speed = 20
        self.cw_mode = 0
        self.cw_weight = 50
        self.cw_spacing = 0
        # Address 0x0E.
        self.adc_assign = 0
        self.tx_attenuator = 0
        # Address 0x0F.
        self.cw_internal = 0
        self.sidetone_volume = 0
        self.cw_rf_delay = 0
        # Address 0x10.
        self.cw_hang = 0
        self.sidetone_freq = 600
        # Address 0x11.
        self.pwm_min = 0
        self.pwm_max = 0
        # Address 0x12 on Orion II (on the Hermes-Lite 2 it is the RX8 frequency).
        self.alex2_filters = 0
        self.rx2_ground_tx = 0
        self.xvtr_enable = 0
        # Hermes-Lite 2 extended addresses.
        self.ptt_hang = 12
        self.tx_latency = 20
        self.watchdog = 1
        self.reset_on_disconnect = 0

    def _set(self, changed, name, value):
        if getattr(self, name) != value:
            setattr(self, name, value)
            changed.append(name)

    def apply(self, c0, c1, c2, c3, c4):
        """Decodes one C&C word; returns (address, request flag, changed field names)."""
        changed = []
        hl2 = self.board.hl2
        request = hl2 and bool(c0 & 0x80)
        address = (c0 >> 1) & (0x3F if hl2 else 0x7F)
        with self.lock:
            self._set(changed, "mox", bool(c0 & 0x01))
            self.words[address] = (c1, c2, c3, c4)
            word = (c1 << 24) | (c2 << 16) | (c3 << 8) | c4
            if address == 0x00:
                self._set(changed, "rx_rate", RX_RATES[c1 & 0x03])
                self._set(changed, "ref_10mhz", (c1 >> 2) & 0x03)
                self._set(changed, "clock_122mhz", (c1 >> 4) & 0x01)
                self._set(changed, "config", (c1 >> 5) & 0x03)
                self._set(changed, "mic_source", (c1 >> 7) & 0x01)
                self._set(changed, "class_e", c2 & 0x01)
                self._set(changed, "oc_outputs", c2 >> 1)
                self._set(changed, "alex_attenuator", c3 & 0x03)
                self._set(changed, "preamp", (c3 >> 2) & 0x01)
                self._set(changed, "dither", (c3 >> 3) & 0x01)
                self._set(changed, "random", (c3 >> 4) & 0x01)
                self._set(changed, "alex_rx_antenna", (c3 >> 5) & 0x03)
                self._set(changed, "alex_rx_out", (c3 >> 7) & 0x01)
                self._set(changed, "alex_tx_relay", c4 & 0x03)
                self._set(changed, "duplex", (c4 >> 2) & 0x01)
                if hl2:
                    self._set(changed, "num_rx", ((c4 >> 3) & 0x0F) + 1)
                else:
                    self._set(changed, "num_rx", ((c4 >> 3) & 0x07) + 1)
                    self._set(changed, "timestamp", (c4 >> 6) & 0x01)
                self._set(changed, "common_frequency", (c4 >> 7) & 0x01)
            elif address == 0x01:
                self._set(changed, "tx_freq", word)
            elif 0x02 <= address <= 0x08:
                self._set_rx_freq(changed, address - 0x02, word)
            elif address == 0x09:
                self._set(changed, "drive", c1)
                if hl2:
                    self._set(changed, "vna", (c2 >> 7) & 0x01)
                    self._set(changed, "tune_request", (c2 >> 4) & 0x01)
                    self._set(changed, "pa_enable", (c2 >> 3) & 0x01)
                    self._set(changed, "alex_tr_disable", (c2 >> 2) & 0x01)
                else:
                    self._set(changed, "mic_boost", c2 & 0x01)
                    self._set(changed, "line_in", (c2 >> 1) & 0x01)
                    self._set(changed, "apollo", (c2 >> 2) & 0x0F)
                    self._set(changed, "alex_manual", (c2 >> 6) & 0x01)
                    self._set(changed, "vna", (c2 >> 7) & 0x01)
                    self._set(changed, "alex_tr_disable", (c3 >> 7) & 0x01)
                self._set(changed, "alex_hpf", c3 & 0x7F)
                self._set(changed, "alex_lpf", c4 & 0x7F)
            elif address == 0x0A:
                self._set(changed, "preamps", c1 & 0x0F)
                self._set(changed, "mic_ptt_tip", (c1 >> 4) & 0x01)
                self._set(changed, "mic_bias", (c1 >> 5) & 0x01)
                self._set(changed, "mic_ptt_disable", (c1 >> 6) & 0x01)
                self._set(changed, "line_in_gain", c2 & 0x1F)
                self._set(changed, "puresignal", (c2 >> 6) & 0x01)
                self._set(changed, "user_outputs", c3 & 0x0F)
                if hl2:
                    self._set(changed, "lna_gain", c4 & 0x3F)
                else:
                    self._set(changed, "adc1_attenuator", (c4 & 0x1F) if c4 & 0x20 else 0)
            elif address == 0x0B:
                self._set(changed, "adc2_attenuator", (c1 & 0x1F) if c1 & 0x20 else 0)
                self._set(changed, "adc3_attenuator", (c2 & 0x1F) if c2 & 0x20 else 0)
                self._set(changed, "cw_reversed", (c2 >> 6) & 0x01)
                self._set(changed, "cw_speed", max(1, c3 & 0x3F))
                self._set(changed, "cw_mode", (c3 >> 6) & 0x03)
                self._set(changed, "cw_weight", min(100, c4 & 0x7F))
                self._set(changed, "cw_spacing", (c4 >> 7) & 0x01)
            elif address == 0x0E:
                self._set(changed, "adc_assign", (c1 << 8) | c2)
                self._set(changed, "tx_attenuator", c3 & 0x1F)
            elif address == 0x0F:
                self._set(changed, "cw_internal", c1 & 0x01)
                self._set(changed, "sidetone_volume", c2)
                self._set(changed, "cw_rf_delay", c3)
            elif address == 0x10:
                self._set(changed, "cw_hang", (c1 << 2) | (c2 & 0x03))
                self._set(changed, "sidetone_freq", (c3 << 4) | (c4 & 0x0F))
            elif address == 0x11:
                self._set(changed, "pwm_min", (c1 << 2) | (c2 & 0x03))
                self._set(changed, "pwm_max", (c3 << 2) | (c4 & 0x03))
            elif address == 0x12 and not hl2:
                self._set(changed, "alex2_filters", c1 & 0x7F)
                self._set(changed, "rx2_ground_tx", (c1 >> 7) & 0x01)
                self._set(changed, "xvtr_enable", (c2 >> 1) & 0x01)
            elif hl2 and 0x12 <= address <= 0x16:
                self._set_rx_freq(changed, address - 0x12 + 7, word)
            elif hl2 and address == 0x17:
                self._set(changed, "ptt_hang", c3 & 0x1F)
                self._set(changed, "tx_latency", c4 & 0x7F)
            elif hl2 and address == 0x39:
                misc = c1 & 0x0F
                if misc == 0x08:
                    self._set(changed, "watchdog", 1)
                elif misc == 0x09:
                    self._set(changed, "watchdog", 0)
            elif hl2 and address == 0x3A:
                self._set(changed, "reset_on_disconnect", c4 & 0x01)
        return address, request, changed

    def _set_rx_freq(self, changed, index, value):
        if self.rx_freq[index] != value:
            self.rx_freq[index] = value
            changed.append("rx%d_freq" % (index + 1))

    def snapshot(self, names):
        with self.lock:
            return {n: getattr(self, n) for n in names}


# ---------------------------------------------------------------------------------------------------------------
# Radio device (rpitx transmitter, optionally with its receiver)
# ---------------------------------------------------------------------------------------------------------------


class Radio:
    """Opens the SoapySDR device shared by the transmitter and the receiver.

    Normally the rpitx driver is opened, with the receiver of --rx attached as its RX channel ("rx=<driver>",
    "rx.<key>=<value>" for --rx-args), the antenna relay and the PTT output settings. With --dry-run nothing is transmitted: only
    the receiver of --rx, if any, is opened on its own. `sdr` is None when no device is needed.
    """

    def __init__(self, args):
        self.sdr = None
        self.SoapySDR = None
        self.has_tx = False
        self.has_rx = False
        rx_args = parse_kwargs(args.rx_args, "--rx-args")
        if args.dry_run and not args.rx:
            return

        import SoapySDR

        self.SoapySDR = SoapySDR
        if args.dry_run:
            dev_args = dict(rx_args)
            dev_args["driver"] = args.rx
        else:
            dev_args = dict(driver="rpitx", amplitude_mode=args.amplitude_mode)
            if args.ppm is not None:
                dev_args["ppm"] = str(args.ppm)
            if args.relay_gpio is not None:
                dev_args["relay_gpio"] = str(args.relay_gpio)
                dev_args["relay_active_low"] = "true" if args.relay_active_low else "false"
                dev_args["relay_tx_delay_ms"] = str(args.relay_tx_delay_ms)
                dev_args["relay_hang_ms"] = str(args.relay_hang_ms)
            if args.ptt_out_gpio is not None:
                dev_args["ptt_gpio"] = str(args.ptt_out_gpio)
                dev_args["ptt_active_low"] = "true" if args.ptt_out_active_low else "false"
                dev_args["ptt_tx_delay_ms"] = str(args.ptt_tx_delay_ms)
                dev_args["ptt_rx_delay_ms"] = str(args.ptt_rx_delay_ms)
                if args.relay_gpio is None:
                    dev_args["relay_hang_ms"] = str(args.relay_hang_ms)
            if args.rx:
                dev_args["rx_mute_tx"] = "false" if args.rx_during_tx else "true"
                dev_args["rx"] = args.rx
                for key, value in rx_args.items():
                    dev_args["rx." + key] = value
        self.sdr = SoapySDR.Device(dev_args)
        self.has_tx = not args.dry_run
        self.has_rx = bool(args.rx) and self.sdr.getNumChannels(SoapySDR.SOAPY_SDR_RX) > 0
        if args.rx and not self.has_rx:
            raise SystemExit("the receiver '%s' has no RX channel" % args.rx)
        if self.has_rx:
            info = self.sdr.getHardwareInfo()
            print("receiver %s (%s)%s" % (args.rx, info.get("rx_hardware", self.sdr.getHardwareKey()),
                                          ", full duplex" if self.sdr.getFullDuplex(SoapySDR.SOAPY_SDR_RX, 0) else ""))

    def close(self):
        """Drops the device (the streams must be closed first)."""
        self.sdr = None


# ---------------------------------------------------------------------------------------------------------------
# Receiver: capture, down-conversion and decimation to the host receivers
# ---------------------------------------------------------------------------------------------------------------


def lowpass_taps(num_taps, cutoff):
    """Blackman-windowed sinc low-pass; @cutoff is the -6 dB frequency relative to the input rate (0 < cutoff < 0.5)."""
    n = np.arange(num_taps, dtype=np.float64) - (num_taps - 1) / 2.0
    h = 2.0 * cutoff * np.sinc(2.0 * cutoff * n) * np.blackman(num_taps)
    return (h / np.sum(h)).astype(np.float32)


def decimation_stages(factor):
    """Splits a decimation factor into stages of at most 10, largest first (for example 50 -> [10, 5])."""
    stages = []
    rest = factor
    while rest > 1:
        for d in range(min(10, rest), 1, -1):
            if rest % d == 0:
                stages.append(d)
                rest //= d
                break
        else:
            stages.append(rest)  # a prime above 10
            rest = 1
    return stages


class Decimator:
    """FIR low-pass filter and decimation by an integer factor, with state carried across blocks."""

    def __init__(self, factor, final):
        self.factor = factor
        # The last stage defines the band edge (transition from 0.4 to 0.6 of the output rate); the earlier stages
        # only have to keep their aliases away from that band, so they get wider transitions and fewer taps.
        num_taps = (28 if final else 12) * factor + 1
        self.taps = lowpass_taps(num_taps, 0.5 / factor)[::-1].copy()
        self.history = np.zeros(num_taps - 1, dtype=np.complex64)

    def process(self, x):
        buf = np.concatenate((self.history, x))
        n = len(self.taps)
        if len(buf) < n:
            self.history = buf
            return np.zeros(0, dtype=np.complex64)
        count = (len(buf) - n) // self.factor + 1
        windows = np.lib.stride_tricks.sliding_window_view(buf, n)[: count * self.factor : self.factor]
        out = (windows @ self.taps).astype(np.complex64)
        self.history = buf[count * self.factor :]
        return out


class DownConverter:
    """One host receiver: mixes its frequency down to 0 Hz and decimates the capture to the host rate."""

    def __init__(self, capture_rate, factor):
        self.capture_rate = capture_rate
        self.shift = 0.0
        self.phase = 0.0
        self.enabled = True
        stages = decimation_stages(factor)
        self.stages = [Decimator(d, i == len(stages) - 1) for i, d in enumerate(stages)]

    def process(self, x):
        if self.shift != 0.0:
            step = -2.0 * math.pi * self.shift / self.capture_rate
            ph = self.phase + step * np.arange(len(x), dtype=np.float64)
            x = x * np.exp(1j * ph).astype(np.complex64)
            self.phase = float((self.phase + step * len(x)) % (2.0 * math.pi))
        for stage in self.stages:
            x = stage.process(x)
        return x


def ep6_payload(iq):
    """Packs receiver samples (spf x num_rx complex, full scale 1.0) and a silent microphone into one EP6 frame payload."""
    spf, num_rx = iq.shape
    bps = 6 * num_rx + 2
    vals = np.empty((spf, num_rx, 2), dtype=np.float64)
    vals[:, :, 0] = iq.real
    vals[:, :, 1] = iq.imag
    vals = np.clip(np.rint(vals * 8388607.0), -8388608, 8388607).astype(">i4")
    out = np.zeros((spf, bps), dtype=np.uint8)
    out[:, : num_rx * 6] = vals.view(np.uint8).reshape(spf, num_rx * 2, 4)[:, :, 1:].reshape(spf, num_rx * 6)
    payload = out.tobytes()
    return payload + bytes(FRAME_LEN - 8 - len(payload))


class Receiver:
    """Owns the RX stream of the radio and turns the capture into host receiver samples.

    One reader thread reads the capture (CF32), follows the host configuration (receive rate, number of
    receivers, their frequencies and, with --follow-rx-gain, the attenuator / LNA gain), and queues blocks of
    decimated samples (n x num_rx) that the EP6 sender takes with take(). The capture runs at an integer multiple
    of the host rate accepted by the receiver; its centre is RX1 plus an offset that keeps the receiver DC spur out
    of the RX1 band, and it is retuned only when RX1 leaves the usable bandwidth (90 % of the capture).
    """

    READ_TIMEOUT_US = 100000
    MAX_BUFFER_S = 0.25
    OVERLOAD_HOLD_S = 0.2

    def __init__(self, args, radio, bridge):
        self.args = args
        self.bridge = bridge
        self.control = bridge.control
        self.sdr = radio.sdr
        S = radio.SoapySDR
        self.S = S
        self.RX = S.SOAPY_SDR_RX
        self.cond = threading.Condition()
        self.blocks = collections.deque()
        self.available = 0
        self.layout = None  # (host rate, num_rx) of the queued samples
        self.running = True
        self.overflows = 0
        self.errors = 0
        self.retunes = 0
        self.dropped = 0
        self.overload_until = 0.0
        self.rate = None
        self.capture_rate = None
        self.num_rx = 0
        self.center = None
        self.converters = []
        self.outside = set()
        self.gain_state = None
        self.wideband = np.zeros(EP4_SAMPLES, dtype=np.float32)
        self.freq_offset = args.rx_freq_offset if args.rx_freq_offset is not None else args.freq_offset

        if args.rx_antenna:
            self.sdr.setAntenna(self.RX, 0, args.rx_antenna)
        if args.rx_ppm:
            self.sdr.setFrequencyCorrection(self.RX, 0, args.rx_ppm)
        self._set_gain(args.rx_gain)
        self._configure(self.control.rx_rate, 1)
        self._retune(self._host_freqs(1)[0])
        self.stream = self.sdr.setupStream(self.RX, S.SOAPY_SDR_CF32, [0], parse_kwargs(args.rx_stream_args, "--rx-stream-args"))
        try:
            self.mtu = max(1024, int(self.sdr.getStreamMTU(self.stream)))
            self.sdr.activateStream(self.stream)
        except BaseException:
            self.sdr.closeStream(self.stream)
            raise
        self.thread = threading.Thread(target=self._reader, name="rx-reader", daemon=True)
        self.thread.start()

    # ---- configuration (reader thread, and the constructor) ----

    def _set_gain(self, gain):
        if gain == "auto":
            if self.sdr.hasGainMode(self.RX, 0):
                self.sdr.setGainMode(self.RX, 0, True)
            else:
                print("the receiver has no automatic gain; set --rx-gain", file=sys.stderr)
        else:
            if self.sdr.hasGainMode(self.RX, 0):
                self.sdr.setGainMode(self.RX, 0, False)
            r = self.sdr.getGainRange(self.RX, 0)
            self.sdr.setGain(self.RX, 0, max(r.minimum(), min(r.maximum(), float(gain))))

    def _follow_gain(self):
        """Maps the host attenuator (Hermes family) or LNA gain (Hermes-Lite 2) to the receiver gain."""
        s = self.control.snapshot(("lna_gain", "adc1_attenuator"))
        state = (s["lna_gain"], s["adc1_attenuator"])
        if state == self.gain_state:
            return
        self.gain_state = state
        r = self.sdr.getGainRange(self.RX, 0)
        if self.bridge.board.hl2:
            gain = s["lna_gain"] - 12.0  # HL2 LNA gain code 0..60 is -12..+48 dB
        else:
            gain = r.maximum() - s["adc1_attenuator"]
        self._set_gain(max(r.minimum(), min(r.maximum(), gain)))
        print("RX gain %.1f dB (host %s)" % (self.sdr.getGain(self.RX, 0), "LNA" if self.bridge.board.hl2 else "attenuator"))

    def _capture_rate(self, rate):
        """Smallest integer multiple of @rate the receiver accepts (or --rx-rate when it is a multiple)."""
        ranges = self.sdr.getSampleRateRange(self.RX, 0)

        def accepted(fs):
            return any(r.minimum() - 0.5 <= fs <= r.maximum() + 0.5 for r in ranges)

        if self.args.rx_rate:
            fs = self.args.rx_rate
            if fs % rate == 0 and accepted(fs):
                return fs
            print("--rx-rate %.0f is not a multiple of %d accepted by the receiver: choosing the capture rate" % (fs, rate),
                  file=sys.stderr)
        for factor in range(1, 257):
            if accepted(rate * factor):
                return rate * factor
        raise RuntimeError("the receiver accepts no integer multiple of %d S/s" % rate)

    def _configure(self, rate, num_rx):
        """Sets the capture rate for a host rate and builds the down-converters; queued samples are dropped."""
        if rate != self.rate:
            fs = self._capture_rate(rate)
            self.sdr.setSampleRate(self.RX, 0, fs)
            self.capture_rate = fs
            self.rate = rate
            self.center = None  # the usable bandwidth changed: recentre on RX1
            print("RX capture %.0f S/s, decimation %d to %d S/s" % (fs, fs // rate, rate))
        factor = self.capture_rate // self.rate
        self.converters = [DownConverter(self.capture_rate, factor) for _ in range(num_rx)]
        self.num_rx = num_rx
        self.outside.clear()
        with self.cond:
            self.blocks.clear()
            self.available = 0
            self.layout = (self.rate, num_rx)

    def _limit(self):
        """Highest |shift| + half the host band that stays in the usable part of the capture."""
        return 0.45 * self.capture_rate

    def _offset(self):
        """Distance between the capture centre and RX1 (keeps the DC spur of the receiver out of the RX1 band)."""
        if self.args.rx_offset is not None:
            return self.args.rx_offset
        offset = self._limit() / 2.0
        return offset if offset > 0.55 * self.rate else 0.0

    def _retune(self, rx1):
        self.center = rx1 + self._offset()
        self.sdr.setFrequency(self.RX, 0, self.center)
        self.retunes += 1

    def _host_freqs(self, num_rx):
        s = self.control.snapshot(("common_frequency",))
        default = self.args.initial_freq
        freqs = []
        for i in range(num_rx):
            f = self.control.rx_freq[0 if s["common_frequency"] else i] or (self.control.rx_freq[0] or default)
            freqs.append(float(f) + self.freq_offset)
        return freqs

    def _follow_host(self):
        s = self.control.snapshot(("rx_rate", "num_rx"))
        num_rx = max(1, min(s["num_rx"], 12))
        if s["rx_rate"] != self.rate or num_rx != self.num_rx:
            self._configure(s["rx_rate"], num_rx)
        if self.args.follow_rx_gain:
            self._follow_gain()
        freqs = self._host_freqs(num_rx)
        half = self.rate / 2.0
        shift = freqs[0] - self.center if self.center is not None else None
        offset_mode = self._offset() > 0
        if shift is None or abs(shift) + half > self._limit() or (offset_mode and abs(shift) < half):
            try:
                self._retune(freqs[0])
            except Exception as e:
                print("cannot tune the receiver to %.0f Hz: %s" % (freqs[0], e), file=sys.stderr)
                return
        for i, (conv, f) in enumerate(zip(self.converters, freqs)):
            conv.shift = f - self.center
            conv.enabled = abs(conv.shift) + half <= self._limit()
            if not conv.enabled and i not in self.outside:
                self.outside.add(i)
                print("RX%d %.6f MHz is outside the receiver capture (%.6f MHz +/- %.0f kHz): sending zeros"
                      % (i + 1, f / 1e6, self.center / 1e6, self._limit() / 1e3))
            elif conv.enabled:
                self.outside.discard(i)

    # ---- reader thread ----

    def _reader(self):
        buf = np.empty(self.mtu, dtype=np.complex64)
        was_streaming = False
        while self.running:
            try:
                self._follow_host()
            except Exception as e:
                self.errors += 1
                print("receiver configuration failed: %s" % e, file=sys.stderr)
                time.sleep(0.1)
                continue
            sr = self.sdr.readStream(self.stream, [buf], len(buf), timeoutUs=self.READ_TIMEOUT_US)
            if sr.ret == self.S.SOAPY_SDR_TIMEOUT:
                continue
            if sr.ret == self.S.SOAPY_SDR_OVERFLOW:
                self.overflows += 1
                continue
            if sr.ret < 0:
                self.errors += 1
                time.sleep(0.01)
                continue
            x = buf[: sr.ret]
            peak = float(np.max(np.maximum(np.abs(x.real), np.abs(x.imag)))) if sr.ret else 0.0
            if peak >= 0.99:
                self.overload_until = time.monotonic() + self.OVERLOAD_HOLD_S
            streaming = self.bridge.streaming
            if streaming and not was_streaming:
                # A new host stream starts with fresh samples.
                with self.cond:
                    self.blocks.clear()
                    self.available = 0
            was_streaming = streaming
            if not streaming:
                continue
            if len(x) >= EP4_SAMPLES:
                self.wideband = (x[-EP4_SAMPLES:].real * 0.5).astype(np.float32)
            outs = []
            for conv in self.converters:
                y = conv.process(x)
                outs.append(y if conv.enabled else np.zeros_like(y))
            if not outs or len(outs[0]) == 0:
                continue
            block = np.stack(outs, axis=1)
            with self.cond:
                self.blocks.append(block)
                self.available += len(block)
                limit = int(self.rate * self.MAX_BUFFER_S)
                while self.available > limit and len(self.blocks) > 1:
                    old = self.blocks.popleft()
                    self.available -= len(old)
                    self.dropped += len(old)
                self.cond.notify_all()

    # ---- EP6 sender ----

    def take(self, count, rate, num_rx, timeout):
        """Returns @count samples (count x num_rx complex64) of the given layout, or None after @timeout seconds."""
        deadline = time.monotonic() + timeout
        with self.cond:
            while self.running:
                if self.layout == (rate, num_rx) and self.available >= count:
                    parts = []
                    need = count
                    while need > 0:
                        b = self.blocks[0]
                        if len(b) <= need:
                            parts.append(self.blocks.popleft())
                            need -= len(b)
                        else:
                            parts.append(b[:need])
                            self.blocks[0] = b[need:]
                            need = 0
                    self.available -= count
                    return np.concatenate(parts) if len(parts) > 1 else parts[0]
                left = deadline - time.monotonic()
                if left <= 0:
                    return None
                self.cond.wait(left)
        return None

    def overload(self):
        return time.monotonic() < self.overload_until

    def close(self):
        self.running = False
        with self.cond:
            self.cond.notify_all()
        self.thread.join()
        try:
            self.sdr.deactivateStream(self.stream)
            self.sdr.closeStream(self.stream)
        except Exception as e:
            print("closing RX stream: %s" % e, file=sys.stderr)


# ---------------------------------------------------------------------------------------------------------------
# Transmitter
# ---------------------------------------------------------------------------------------------------------------


class Transmitter:
    """Owns the rpitx SoapySDR TX stream. Samples, frequency and drive changes are applied by one writer thread.

    The request fields (pending_freq, pending_gain) and the statistics are plain attributes shared between
    threads; single attribute reads and writes are atomic under the CPython GIL.
    """

    WRITE_TIMEOUT_US = 20000

    def __init__(self, args, radio):
        self.args = args
        self.queue = queue.Queue()
        self.max_queued = max(1, int(TX_RATE * args.queue_ms / 1000))
        self.queued = 0
        self.queued_lock = threading.Lock()
        self.dropped = 0
        self.sent = 0
        self.current_freq = None
        self.failed_freq = None
        self.current_gain = None
        self.pending_freq = None
        self.pending_gain = None
        self.last_retune = -1e9
        self.running = True
        self.sdr = None
        self.stream = None
        self.fifo_fill = 0.0
        self.underflows = 0
        self.late = 0
        self.envelope = 0.0
        self.ptt = False
        stream_args = parse_kwargs(args.stream_args, "--stream-args")

        if not radio.has_tx:
            print("dry run: nothing is transmitted")
            self.current_gain = args.gain
        else:
            SoapySDR = radio.SoapySDR
            SOAPY_SDR_CF32 = SoapySDR.SOAPY_SDR_CF32
            SOAPY_SDR_TX = SoapySDR.SOAPY_SDR_TX

            self.SoapySDR = SoapySDR
            self.SOAPY_SDR_TX = SOAPY_SDR_TX
            self.sdr = radio.sdr
            try:
                self.sdr.setSampleRate(SOAPY_SDR_TX, 0, TX_RATE)
                self.sdr.setGain(SOAPY_SDR_TX, 0, args.gain)
                self.current_gain = args.gain
                start_freq = args.freq if args.freq is not None else args.initial_freq
                self._apply_frequency(start_freq)
                self.stream = self.sdr.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, [0], stream_args)
                self.sdr.activateStream(self.stream)
            except BaseException:
                # Release the TX stream before the error propagates.
                if self.stream is not None:
                    try:
                        self.sdr.closeStream(self.stream)
                    except Exception:
                        pass
                self.stream = None
                self.sdr = None
                raise

        self.thread = threading.Thread(target=self._writer, name="rpitx-writer", daemon=True)
        self.thread.start()

    # ---- control requests (any thread) ----

    def request_frequency(self, freq):
        """Asks for a retune; the writer applies the latest request, at most once per --retune-interval."""
        if freq is not None and freq > 0:
            self.pending_freq = float(freq)

    def request_drive(self, drive):
        """Maps the host drive level (0-255) to the rpitx pad drive (0-7)."""
        self.pending_gain = int(round(max(0, min(255, drive)) * 7 / 255))

    def submit(self, iq):
        """Queues a block of complex64 samples, or None to end the current burst."""
        if iq is not None:
            n = len(iq)
            if n:
                self.envelope = float(np.sqrt(np.mean(np.abs(iq) ** 2)))
            with self.queued_lock:
                self.queued += n
                overflow = self.queued > self.max_queued
            if overflow:
                # The DMA clock and the host clock drift apart: drop the oldest block rather than adding latency.
                self._drop_oldest()
        self.queue.put(iq)

    def _drop_oldest(self):
        """Drops the oldest block of samples; burst ends (None) keep their place in the queue."""
        with self.queue.mutex:
            items = self.queue.queue
            for idx, item in enumerate(items):
                if item is not None:
                    del items[idx]
                    break
            else:
                return
        with self.queued_lock:
            self.queued -= len(item)
        self.dropped += 1

    # ---- writer thread ----

    def _apply_frequency(self, freq):
        self.last_retune = time.monotonic()
        try:
            if self.sdr is not None:
                self.sdr.setFrequency(self.SOAPY_SDR_TX, 0, freq)
            self.current_freq = freq
            self.failed_freq = None
            print("TX frequency %.6f MHz" % (freq / 1e6))
        except Exception as e:  # out of range or PLL error: the previous frequency stays in effect
            self.failed_freq = freq
            print("cannot set TX frequency %.0f Hz: %s" % (freq, e), file=sys.stderr)

    def _apply_controls(self):
        freq = self.pending_freq
        if freq is not None and self.failed_freq is not None and abs(freq - self.failed_freq) < 0.5:
            self.pending_freq = None  # already refused by the driver: wait for another frequency
        elif freq is not None and (self.current_freq is None or abs(freq - self.current_freq) >= 0.5):
            if time.monotonic() - self.last_retune >= self.args.retune_interval:
                self.pending_freq = None
                self._apply_frequency(freq)
        elif freq is not None:
            self.pending_freq = None
        gain = self.pending_gain
        if gain is not None:
            self.pending_gain = None
            if gain != self.current_gain:
                self.current_gain = gain
                if self.sdr is not None:
                    self.sdr.setGain(self.SOAPY_SDR_TX, 0, gain)
                print("TX drive -> PAD %d" % gain)

    def _update_stats(self):
        if self.sdr is None:
            return
        try:
            self.fifo_fill = float(self.sdr.readSensor("fifo_fill"))
            self.underflows = int(float(self.sdr.readSensor("underflows")))
            self.late = int(float(self.sdr.readSensor("late")))
            self.ptt = self.sdr.readSensor("ptt") == "true"
        except Exception:
            pass

    def _write(self, item):
        done = 0
        while done < len(item) and self.running:
            sr = self.sdr.writeStream(self.stream, [item[done:]], len(item) - done, 0, 0, self.WRITE_TIMEOUT_US)
            if sr.ret == self.SoapySDR.SOAPY_SDR_TIMEOUT:
                continue
            if sr.ret < 0:
                print("writeStream error %d" % sr.ret, file=sys.stderr)
                break
            done += sr.ret
        return done

    def _writer(self):
        next_stats = 0.0
        while self.running:
            self._apply_controls()
            now = time.monotonic()
            if now >= next_stats:
                next_stats = now + 0.1
                self._update_stats()
            try:
                item = self.queue.get(timeout=0.02)
            except queue.Empty:
                continue
            if item is None:
                self.envelope = 0.0
                if self.sdr is not None:
                    end = np.zeros(0, np.complex64)
                    self.sdr.writeStream(self.stream, [end], 0, self.SoapySDR.SOAPY_SDR_END_BURST)
                continue
            with self.queued_lock:
                self.queued -= len(item)
            if self.sdr is None:
                self.sent += len(item)
                continue
            self.sent += self._write(item)

    def close(self):
        self.running = False
        # The writer wakes up at least every 20 ms (queue and writeStream timeouts); the stream must not be
        # closed while it is still writing.
        self.thread.join()
        if self.sdr is not None and self.stream is not None:
            try:
                self.sdr.deactivateStream(self.stream)
                self.sdr.closeStream(self.stream)
            except Exception as e:
                print("closing stream: %s" % e, file=sys.stderr)


# ---------------------------------------------------------------------------------------------------------------
# CW: key inputs, envelope shaping, keyer
# ---------------------------------------------------------------------------------------------------------------


class KeyInputs:
    """Optional GPIO inputs (active low, internal pull-up): dot paddle or straight key, dash paddle, PTT.

    Uses the python3 libgpiod bindings (v2 API, or the v1 API as a fallback).
    """

    def __init__(self, chip, dot, dash, ptt):
        self.pins = {"dot": dot, "dash": dash, "ptt": ptt}
        used = [p for p in self.pins.values() if p is not None]
        self.enabled = bool(used)
        if not self.enabled:
            return
        if 4 in used:
            raise SystemExit("GPIO4 is the rpitx RF output and cannot be used as a key or PTT input")
        try:
            import gpiod
        except ImportError:
            raise SystemExit("key/PTT GPIO inputs need the python3 libgpiod bindings (apt install python3-libgpiod)")
        self.gpiod = gpiod
        path = chip if chip.startswith("/") else "/dev/" + chip
        if hasattr(gpiod, "request_lines"):
            from gpiod.line import Bias, Direction, Value

            self.active = Value.ACTIVE
            settings = gpiod.LineSettings(direction=Direction.INPUT, bias=Bias.PULL_UP, active_low=True)
            self.request = gpiod.request_lines(path, consumer="hpsdr_rpitx_bridge", config={tuple(used): settings})
            self.read_pin = lambda pin: self.request.get_value(pin) == self.active
        else:
            c = gpiod.Chip(path)
            self.lines = {}
            for pin in used:
                line = c.get_line(pin)
                flags = getattr(gpiod, "LINE_REQ_FLAG_BIAS_PULL_UP", 0) | getattr(gpiod, "LINE_REQ_FLAG_ACTIVE_LOW", 0)
                line.request(consumer="hpsdr_rpitx_bridge", type=gpiod.LINE_REQ_DIR_IN, flags=flags)
                self.lines[pin] = line
            self.read_pin = lambda pin: bool(self.lines[pin].get_value())

    def read(self):
        """Returns (dot, dash, ptt) as booleans (True = pressed)."""
        if not self.enabled:
            return False, False, False
        return tuple(self.pins[k] is not None and self.read_pin(self.pins[k]) for k in ("dot", "dash", "ptt"))


class CwShaper:
    """Turns a per-sample key-down signal into a raised-cosine shaped carrier envelope (I = envelope, Q = 0)."""

    def __init__(self, ramp_ms):
        self.ramp = max(1, int(TX_RATE * ramp_ms / 1000))
        self.pos = 0
        n = np.arange(self.ramp + 1, dtype=np.float64)
        self.table = (0.5 - 0.5 * np.cos(np.pi * n / self.ramp)).astype(np.float32)

    def idle(self):
        return self.pos == 0

    def process(self, keydown):
        """keydown: boolean array, one entry per 48 kHz sample. Returns complex64 samples."""
        steps = np.where(keydown, 1, -1).astype(np.int64)
        if np.all(steps == steps[0]):
            pos = np.clip(self.pos + steps[0] * np.arange(1, len(steps) + 1), 0, self.ramp)
        else:
            pos = np.empty(len(steps), dtype=np.int64)
            p = self.pos
            for i, s in enumerate(steps):
                p = min(self.ramp, max(0, p + s))
                pos[i] = p
        self.pos = int(pos[-1]) if len(pos) else self.pos
        return self.table[pos].astype(np.complex64)


class Keyer:
    """Radio side CW keyer (straight, iambic A, iambic B) driven by GPIO paddles.

    Active while the host enables the radio internal keyer (C&C 0x0F C1 bit 0). Timing, weight, spacing, key
    reversal, RF delay and hang time follow the host C&C. The keyer runs on a 1 ms tick and produces 48 samples
    per tick; it reports PTT to the host while keyed or hanging.
    """

    TICK = 0.001
    SAMPLES_PER_TICK = TX_RATE // 1000

    def __init__(self, bridge, inputs, ramp_ms):
        self.bridge = bridge
        self.inputs = inputs
        self.shaper = CwShaper(ramp_ms)
        self.dot = False
        self.dash = False
        self.ptt_in = False
        self.ptt = False
        self.running = True
        # Iambic state.
        self.element = None  # "dot" / "dash" while an element (mark + space) is being sent
        self.mark_left = 0
        self.space_left = 0
        self.last_element = None
        self.memory = None
        self.squeezed = False
        self.prev_paddles = (False, False)
        self.gap_left = 0
        self.delay_line = collections.deque()
        self.hang_left = 0
        self.thread = threading.Thread(target=self._run, name="cw-keyer", daemon=True)
        self.thread.start()

    def _timing(self, cfg):
        dit = 1200.0 / max(1, cfg["cw_speed"])
        weight = cfg["cw_weight"] / 50.0 if cfg["cw_weight"] else 1.0
        mark_extra = dit * (weight - 1.0)
        return {
            "dot": (max(1, int(round(dit + mark_extra))), max(1, int(round(dit - mark_extra)))),
            "dash": (max(1, int(round(3 * dit + mark_extra))), max(1, int(round(dit - mark_extra)))),
            "letter": int(round(2 * dit)),
        }

    def _start(self, element, timing):
        self.element = element
        self.mark_left, self.space_left = timing[element]
        self.last_element = element

    def _iambic(self, dot, dash, cfg, timing):
        """Advances the keyer by 1 ms; returns the key-down state of this millisecond."""
        mode = cfg["cw_mode"]
        if mode == 0:
            self.element = None
            return dot or dash
        prev_dot, prev_dash = self.prev_paddles
        self.prev_paddles = (dot, dash)
        if self.element is not None:
            # Paddle memory: an opposite paddle newly pressed during an element is sent next.
            if self.element == "dot" and dash and not prev_dash:
                self.memory = "dash"
            elif self.element == "dash" and dot and not prev_dot:
                self.memory = "dot"
            if dot and dash:
                self.squeezed = True
            if self.mark_left > 0:
                self.mark_left -= 1
                return True
            if self.space_left > 0:
                self.space_left -= 1
                if self.space_left > 0:
                    return False
            # Element complete.
            finished = self.element
            self.element = None
            nxt = None
            if dot and dash:
                nxt = "dash" if finished == "dot" else "dot"
            elif self.memory is not None:
                nxt = self.memory
            elif mode == 2 and self.squeezed:
                # Iambic B: a squeeze released during the element adds one opposite element.
                nxt = "dash" if finished == "dot" else "dot"
            elif dot:
                nxt = "dot"
            elif dash:
                nxt = "dash"
            self.memory = None
            self.squeezed = False
            if nxt is not None:
                self._start(nxt, timing)
            elif cfg["cw_spacing"]:
                self.gap_left = timing["letter"]
            return False
        if self.gap_left > 0:
            self.gap_left -= 1
            return False
        if dot and dash:
            self._start("dash" if self.last_element == "dot" else "dot", timing)
            self.squeezed = True
        elif dot:
            self._start("dot", timing)
        elif dash:
            self._start("dash", timing)
        else:
            return False
        self.mark_left -= 1
        return True

    def _run(self):
        cfg_names = ("cw_internal", "cw_speed", "cw_mode", "cw_weight", "cw_spacing", "cw_reversed", "cw_rf_delay", "cw_hang")
        next_tick = time.perf_counter()
        while self.running:
            next_tick += self.TICK
            delay = next_tick - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
            elif delay < -0.05:
                next_tick = time.perf_counter()
            dot, dash, ptt_in = self.inputs.read()
            cfg = self.bridge.control.snapshot(cfg_names)
            if cfg["cw_reversed"]:
                dot, dash = dash, dot
            self.dot, self.dash, self.ptt_in = dot, dash, ptt_in
            if not cfg["cw_internal"] or not self.bridge.streaming:
                if self.ptt:
                    self._release()
                continue
            key = self._iambic(dot, dash, cfg, self._timing(cfg))
            # RF delay: PTT rises immediately, the carrier follows cw_rf_delay ms later.
            self.delay_line.append(key)
            while len(self.delay_line) > cfg["cw_rf_delay"] + 1:
                self.delay_line.popleft()
            rf_key = self.delay_line[0] if len(self.delay_line) > cfg["cw_rf_delay"] else False
            if key or rf_key or not self.shaper.idle():
                self.hang_left = cfg["cw_hang"]
                self.ptt = True
            elif self.ptt:
                if self.hang_left > 0:
                    self.hang_left -= 1
                else:
                    self._release()
                    continue
            if self.ptt:
                iq = self.shaper.process(np.full(self.SAMPLES_PER_TICK, rf_key))
                self.bridge.tx_push("keyer", iq)

    def _release(self):
        self.ptt = False
        self.delay_line.clear()
        self.element = None
        self.bridge.tx_release("keyer")

    def stop(self):
        self.running = False
        self.thread.join(timeout=1)


class PttInput:
    """GPIO PTT input without a keyer (reported to the host, which then asserts MOX)."""

    def __init__(self, inputs):
        self.inputs = inputs
        self.dot = self.dash = self.ptt = self.ptt_in = False
        self.running = True
        self.thread = threading.Thread(target=self._run, name="key-inputs", daemon=True)
        self.thread.start()

    def _run(self):
        while self.running:
            self.dot, self.dash, self.ptt_in = self.inputs.read()
            time.sleep(0.002)

    def stop(self):
        self.running = False
        self.thread.join(timeout=1)


# ---------------------------------------------------------------------------------------------------------------
# Host link (UDP peer or TCP connection)
# ---------------------------------------------------------------------------------------------------------------


class HostLink:
    """Destination of the radio -> host stream: a UDP address or a connected TCP socket."""

    def __init__(self, udp_sock=None, addr=None, tcp_sock=None):
        self.udp_sock = udp_sock
        self.addr = addr
        self.tcp_sock = tcp_sock
        self.lock = threading.Lock()
        self.closed = False

    @property
    def is_tcp(self):
        return self.tcp_sock is not None

    def same(self, other):
        if other is None:
            return False
        if self.is_tcp or other.is_tcp:
            return self.tcp_sock is other.tcp_sock
        return self.addr == other.addr

    def describe(self):
        return "%s:%d%s" % (self.addr[0], self.addr[1], " (TCP)" if self.is_tcp else "")

    def send(self, data):
        if self.closed:
            return False
        try:
            if self.is_tcp:
                with self.lock:
                    self.tcp_sock.sendall(data)
            else:
                self.udp_sock.sendto(data, self.addr)
            return True
        except OSError as e:
            if self.is_tcp:
                self.closed = True
            print("send to %s failed: %s" % (self.describe(), e), file=sys.stderr)
            return False


# ---------------------------------------------------------------------------------------------------------------
# Bridge
# ---------------------------------------------------------------------------------------------------------------


class Bridge:
    VERBOSE_SKIP = ("mox", "tx_freq", "rx1_freq")

    def __init__(self, args):
        self.args = args
        self.board = BOARDS[args.board]
        self.firmware = args.fw_version if args.fw_version is not None else self.board.firmware
        self.control = ControlState(self.board)
        self.mac = args.mac if args.mac is not None else default_mac()

        # Everything that can be refused (GPIO inputs, sockets) is set up before the transmitter is opened, and
        # whatever was opened is released when the construction fails or is interrupted.
        self.udp = None
        self.tcp_listen = None
        self.radio = None
        self.tx = None
        self.rx = None
        self.keys = None
        self.streaming = False
        used = [p for p in (args.key_dot_gpio, args.key_dash_gpio, args.ptt_gpio) if p is not None]
        if args.relay_gpio is not None and args.relay_gpio in used:
            raise SystemExit("GPIO%d cannot be both the antenna relay and a key or PTT input" % args.relay_gpio)
        if args.ptt_out_gpio is not None and args.ptt_out_gpio in used:
            raise SystemExit("GPIO%d cannot be both the PTT output and a key or PTT input" % args.ptt_out_gpio)
        inputs = KeyInputs(args.gpio_chip, args.key_dot_gpio, args.key_dash_gpio, args.ptt_gpio)
        try:
            self._open(args, inputs)
        except BaseException:
            self._release()
            raise

    def _open(self, args, inputs):
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self.udp.bind((args.bind, args.port))
        if args.tcp:
            self.tcp_listen = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.tcp_listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.tcp_listen.bind((args.bind, args.port))
            self.tcp_listen.listen(2)
        self.tcp_clients = {}  # socket -> (address, receive buffer, HostLink), oldest first
        self.refused_hosts = set()  # addresses already reported as refused by --allow
        self.udp_errors = set()  # errno values of UDP receive errors already reported
        self.takeover_refused = set()  # addresses already reported as refused a start while another host streams

        self.link = None
        self.streaming = False
        self.wideband = False
        self.stream_gen = 0
        self.last_host_packet = 0.0
        self.ep2_seq = None
        self.ep2_packets = 0
        self.ep2_lost = 0
        self.ep2_late = 0
        self.ep2_late_run = 0

        self.tx_lock = threading.Lock()
        self.tx_source = None
        self.tx_priority = {"host": 0, "cwx": 1, "keyer": 2}
        self.mox_seen = False
        self.mox_since = 0.0
        self.mox_peak = 0.0
        self.mox_hint_given = False
        self.cw_hint_given = False
        self.cwx_shaper = CwShaper(args.cw_ramp_ms)
        self.cwx_hang = 0
        self.cwx_ptt = False

        self.rx_stalls = 0
        self.requests = collections.deque(maxlen=16)  # Hermes-Lite 2 acknowledges to send
        self.stop = threading.Event()
        self.sender = None
        self._payload_cache = {}

        self.radio = Radio(args)
        self.tx = Transmitter(args, self.radio)
        if self.radio.has_rx:
            self.rx = Receiver(args, self.radio, self)
        if inputs.enabled:
            if args.key_dot_gpio is not None or args.key_dash_gpio is not None:
                self.keys = Keyer(self, inputs, args.cw_ramp_ms)
            else:
                self.keys = PttInput(inputs)

    def _release(self):
        """Releases what a failed construction had opened."""
        if self.keys is not None:
            self.keys.stop()
        if self.tx is not None:
            self.tx.close()
        if self.rx is not None:
            self.rx.close()
        if self.radio is not None:
            self.radio.close()
        if self.tcp_listen is not None:
            self.tcp_listen.close()
        if self.udp is not None:
            self.udp.close()

    # ---- transmitter arbitration (host I/Q, Hermes-Lite 2 CWX, radio keyer) ----

    def tx_push(self, source, iq):
        with self.tx_lock:
            current = self.tx_source
            if current is not None and current != source:
                if self.tx_priority[source] < self.tx_priority[current]:
                    return
                # A higher priority source takes over: close the burst of the previous one.
                self.tx.submit(None)
            self.tx_source = source
            self.tx.submit(iq)

    def tx_release(self, source):
        with self.tx_lock:
            if self.tx_source == source:
                self.tx_source = None
                self.tx.submit(None)

    def _update_frequency(self):
        if self.args.freq is not None:
            self.tx.request_frequency(self.args.freq)
            return
        s = self.control.snapshot(("duplex", "tx_freq"))
        freq = s["tx_freq"] if s["duplex"] else self.control.rx_freq[0]
        if freq:
            self.tx.request_frequency(freq + self.args.freq_offset)

    # ---- discovery, start/stop and the other Metis commands ----

    def discovery_reply(self, length):
        reply = bytearray(max(length, 60))
        reply[0:3] = bytes([0xEF, 0xFE, 0x03 if self.streaming else 0x02])
        reply[3:9] = self.mac
        reply[9] = self.firmware & 0xFF
        reply[10] = self.board.code
        reply[19] = self.board.max_rx
        if self.board.hl2:
            reply[20] = 0x40 | 0x05  # 16-bit wideband samples, build 5
            reply[21] = self.args.fw_minor & 0xFF
            key = self.keys
            reply[27] = (0x80 if key is not None and (key.dot or key.dash) else 0) | (0x40 if self._ptt() else 0)
        return bytes(reply)

    def _start(self, link, flags):
        iq = bool(flags & 0x01)
        wideband = bool(flags & 0x02)
        restart = not self.streaming or not link.same(self.link)
        if self.link is not None and not link.same(self.link):
            print("host changed from %s to %s" % (self.link.describe(), link.describe()))
        self.link = link
        self.last_host_packet = time.monotonic()
        self.wideband = wideband
        if not iq:
            self._stop_stream(release_link=False)
            self.wideband = wideband
            if wideband:
                self._launch_sender()
            return
        if restart or self.sender is None or not self.sender.is_alive():
            self.ep2_seq = None
            self.streaming = True
            self._launch_sender()
        print("start from %s (I/Q%s)" % (link.describe(), " + wideband" if wideband else ""))

    def _launch_sender(self):
        self.stream_gen += 1
        self.sender = threading.Thread(target=self._sender, args=(self.stream_gen, self.link), name="ep6", daemon=True)
        self.sender.start()

    def _stop_stream(self, release_link=True):
        self.streaming = False
        self.wideband = False
        self.stream_gen += 1
        self._host_mox(False)
        self.tx_release("cwx")
        self.cwx_ptt = False
        if release_link and self.link is not None and self.link.is_tcp:
            self._close_tcp(self.link.tcp_sock)
        if release_link:
            self.link = None

    def _takeover_refused(self, link):
        """A host at another address cannot take the transmitter over while a stream is active (unless --takeover).

        The same address may restart its stream (the program restarted with another source port); a stream whose
        host went silent is released by the watchdog, after which anyone allowed can start."""
        if self.args.takeover or not self.streaming or self.link is None or link.addr[0] == self.link.addr[0]:
            return False
        if link.addr[0] not in self.takeover_refused:
            self.takeover_refused.add(link.addr[0])
            print("start from %s refused: %s is streaming (use --takeover to let another host take over)" % (link.describe(), self.link.describe()))
        return True

    def handle_command(self, data, link):
        cmd = data[2]
        if cmd == 0x02:
            link.send(self.discovery_reply(PACKET_LEN if link.is_tcp else 60))
            print("discovery from %s" % link.describe())
        elif cmd == 0x04 and len(data) >= 4:
            if data[3] & 0x03:
                if self._takeover_refused(link):
                    return
                self._start(link, data[3])
            elif self.link is None or link.same(self.link):
                self._stop_stream()
                print("stop from %s" % link.describe())
            if link.is_tcp and not data[3] & 0x03:
                self._close_tcp(link.tcp_sock)
        elif cmd == 0x03:
            if len(data) >= 13 and data[3:9] == self.mac:
                print("IP address change request from %s ignored" % link.describe())
            elif len(data) >= 4 and data[3] in (0x01, 0x02):
                print("firmware %s request from %s ignored" % ("programming" if data[3] == 0x01 else "erase", link.describe()))
        elif cmd == 0x01 and len(data) == PACKET_LEN and data[3] == 0x02:
            # Only the host that started the stream drives the transmitter.
            if self.link is not None and link.same(self.link):
                self.last_host_packet = time.monotonic()
                self.handle_ep2(data)

    # ---- host -> radio: endpoint 2 ----

    def _host_mox(self, mox):
        was = self.mox_seen
        self.mox_seen = mox
        if mox and not was:
            self.mox_since = time.monotonic()
            self.mox_peak = 0.0
            self.mox_hint_given = False
            print("MOX on")
        elif was and not mox:
            print("MOX off")
            self.tx_release("host")

    def _apply_cc(self, c):
        address, request, changed = self.control.apply(*c)
        if request:
            self._queue_ack(address, c)
        if not changed:
            return
        if "tx_freq" in changed or "rx1_freq" in changed or "duplex" in changed:
            self._update_frequency()
        if "drive" in changed and self.args.follow_drive:
            self.tx.request_drive(self.control.drive)
        if "cw_internal" in changed and self.control.cw_internal and self.keys is None and not self.board.hl2:
            if not self.cw_hint_given:
                self.cw_hint_given = True
                print("host enabled the radio CW keyer: in CW the host does not assert MOX and expects the radio to "
                      "key from its paddles; use --key-dot-gpio/--key-dash-gpio, or let the host generate CW "
                      "(piHPSDR: disable 'CW handled in Radio')")
        if "rx2_ground_tx" in changed and self.control.rx2_ground_tx and not self.control.mox and self.control.cw_internal:
            # piHPSDR grounds RX2 whenever it transmits: here it is in TX with the MOX bit left to the radio keyer.
            print("host switched to TX in CW without asserting MOX: the radio keyer is in charge (key the GPIO "
                  "paddles, use TUNE, or disable 'CW handled in Radio' in the host)")
        if self.args.verbose:
            shown = [n for n in changed if n not in self.VERBOSE_SKIP]
            if shown:
                freqs = [n for n in shown if n[:2] == "rx" and n[2:-5].isdigit() and n.endswith("_freq")]
                s = self.control.snapshot([n for n in shown if n not in freqs])
                for n in freqs:
                    s[n] = self.control.rx_freq[int(n[2:-5]) - 1]
                print("C&C 0x%02X: %s" % (address, ", ".join("%s=%s" % (n, s[n]) for n in shown)))

    def _queue_ack(self, address, c):
        c0, c1, c2, c3, c4 = c
        if address in (0x3C, 0x3D) and c1 == 0x07:
            # I2C read: no I2C device behind the bridge, answer with an error response.
            self.requests.append((0x3F, c1, c2, c3, c4))
        else:
            self.requests.append((address, c1, c2, c3, c4))

    def _ep2_accept(self, seq):
        """Tracks the EP2 sequence number; returns False for a late or duplicate packet (to be dropped).

        A forward jump counts the missing packets as lost. A packet that arrives late (reordered) or twice is
        counted in ep2_late and dropped: its samples are out of order or already sent, its C&C is stale, and the
        sequence number is not moved back, so it is not counted as lost either. Sequence 0, a jump back of more
        than EP2_REORDER_WINDOW packets, or EP2_RESYNC_LATE late packets in a row are a restarted stream."""
        last = self.ep2_seq
        if last is None or seq == 0:
            self.ep2_seq = seq
            self.ep2_late_run = 0
            return True
        d = (seq - last) & 0xFFFFFFFF
        if 0 < d < 0x80000000:
            self.ep2_lost += d - 1
            self.ep2_seq = seq
            self.ep2_late_run = 0
            return True
        back = (-d) & 0xFFFFFFFF  # 0 for a duplicate
        if back <= EP2_REORDER_WINDOW:
            self.ep2_late_run += 1
            if self.ep2_late_run < EP2_RESYNC_LATE:
                self.ep2_late += 1
                return False
        self.ep2_seq = seq
        self.ep2_late_run = 0
        return True

    def handle_ep2(self, data):
        seq = struct.unpack(">I", data[4:8])[0]
        if not self._ep2_accept(seq):
            return
        self.ep2_packets += 1

        for base in (8, 8 + FRAME_LEN):
            if data[base : base + 3] != SYNC:
                continue
            self._apply_cc(tuple(data[base + 3 : base + 8]))
            s = np.frombuffer(data, dtype=">i2", count=SAMPLES_PER_EP2_FRAME * 4, offset=base + 8).reshape(-1, 4)
            mox = self.control.mox
            if self.board.hl2 and self.control.cw_internal and self._cwx(s[:, 2]):
                continue
            self._host_mox(mox)
            if mox or self.args.ignore_ptt:
                i = s[:, 2].astype(np.float32) / 32768.0
                q = s[:, 3].astype(np.float32) / 32768.0
                if self.args.swap_iq:
                    i, q = q, i
                iq = (i + 1j * q).astype(np.complex64)
                if self.args.iq_gain != 1.0:
                    iq *= np.float32(self.args.iq_gain)
                if mox:
                    self._check_silence(iq)
                self.tx_push("host", iq)

    def _cwx(self, i_samples):
        """Hermes-Lite 2 CWX: the LSB of every I sample is the key; returns True while CWX owns the transmitter."""
        keydown = (i_samples & 1).astype(bool)
        if not keydown.any() and not self.cwx_ptt:
            return False
        hang_ms = max(self.control.cw_hang, self.control.ptt_hang)
        if keydown.any() or not self.cwx_shaper.idle():
            self.cwx_hang = int(TX_RATE * hang_ms / 1000)
        else:
            self.cwx_hang -= len(keydown)
            if self.cwx_hang <= 0:
                self.cwx_ptt = False
                self.tx_release("cwx")
                return False
        self.cwx_ptt = True
        self.tx_push("cwx", self.cwx_shaper.process(keydown))
        return True

    def _check_silence(self, iq):
        """Explains why a MOX without modulation leaves the carrier off (envelope mode gates small amplitudes)."""
        peak = float(np.max(np.abs(iq))) if len(iq) else 0.0
        self.mox_peak = max(self.mox_peak, peak)
        if self.mox_hint_given or time.monotonic() - self.mox_since < 1.5:
            return
        self.mox_hint_given = True
        # In envelope mode the carrier is off below magnitude 0.5 / (PAD + 1) (see the driver amplitude mapping).
        pad = self.tx.current_gain if self.tx.current_gain is not None else 7
        threshold = 0.5 / (pad + 1)
        if self.mox_peak < threshold and self.args.amplitude_mode == "envelope":
            print("MOX on but the host I/Q stays below %.1f dBFS (peak %.4f): no carrier in envelope mode at PAD %d. Use TUNE, "
                  "speak into the microphone, select AM/FM/CW, or raise the drive/--iq-gain."
                  % (20 * math.log10(threshold), self.mox_peak, pad))

    # ---- radio -> host: endpoints 6 and 4 ----

    def _ptt(self):
        k = self.keys
        return bool((k is not None and (k.ptt or k.ptt_in)) or self.cwx_ptt)

    def _payloads(self, num_rx):
        """16 pre-built EP6 frame payloads: num_rx * (I24, Q24) + silent mic16 per sample."""
        cached = self._payload_cache.get(num_rx)
        if cached is not None:
            return cached
        bps = 6 * num_rx + 2
        spf = (FRAME_LEN - 8) // bps
        level = self.args.rx_noise
        payloads = []
        for seed in range(16):
            out = np.zeros((spf, bps), dtype=np.uint8)
            if level > 0:
                rng = np.random.default_rng(seed)
                vals = np.clip(rng.normal(0, level * 8388607, spf * num_rx * 2), -8388608, 8388607).astype(">i4")
                b = vals.view(np.uint8).reshape(spf, num_rx * 2, 4)[:, :, 1:]  # 24-bit big endian
                out[:, : num_rx * 6] = b.reshape(spf, num_rx * 6)
            payload = out.tobytes()
            payloads.append(payload + bytes(FRAME_LEN - 8 - len(payload)))
        self._payload_cache[num_rx] = (payloads, spf)
        return payloads, spf

    def _power_counts(self):
        if self.args.fwd_counts <= 0 or self.tx_source is None:
            return 0
        pad = (self.tx.current_gain if self.tx.current_gain is not None else 7) + 1
        return int(min(4095, self.args.fwd_counts * min(1.0, self.tx.envelope) * pad / 8))

    def _status(self, k):
        """C0..C4 of the k-th EP6 frame."""
        ptt = 1 if self._ptt() else 0
        if self.board.hl2 and k % 2 == 1 and self.requests:
            raddr, c1, c2, c3, c4 = self.requests.popleft()
            return bytes([0x80 | (raddr << 1) | ptt, c1, c2, c3, c4])
        keys = self.keys
        dot = 1 if keys is not None and keys.dot else 0
        dash = 1 if keys is not None and keys.dash else 0
        flags = (dot << 2) | (dash << 1) | ptt
        rotation = 3 if self.board.hl2 else (5 if self.board.adc_count > 1 else 4)
        addr = k % rotation
        c1 = c2 = c3 = c4 = 0
        if addr == 0:
            # IO1..IO4 inputs are active low: all inactive (no TX inhibit, no auto tune request); bit 0 is the
            # ADC overload, reported while the receiver clips.
            c1 = 0x1E | (1 if self.rx is not None and self.rx.overload() else 0)
            c4 = self.firmware & 0xFF
            if self.board.metis:
                c2 = self.args.mercury_version & 0xFF
                c3 = self.args.penelope_version & 0xFF
            if self.board.hl2:
                fifo = int(self.tx.fifo_fill * TX_RATE * self.args.fifo_ms_hint / 100000 / 32)
                c3 = min(0x7F, max(0, fifo))
        elif addr == 1:
            fwd = self._power_counts()
            if self.board.hl2:
                t = cpu_temperature()
                temp = int((t * 0.01 + 0.5) / 3.26 * 4096) if t is not None else 0
                c1, c2 = (temp >> 8) & 0xFF, temp & 0xFF
            else:
                c1, c2 = (fwd >> 8) & 0xFF, fwd & 0xFF
            c3, c4 = (fwd >> 8) & 0xFF, fwd & 0xFF
        elif addr == 3:
            supply = int(self.args.supply_voltage / ((4.7 + 0.82) / 0.82) / 3.3 * 4095) & 0xFFF
            c3, c4 = (supply >> 8) & 0xFF, supply & 0xFF
        return bytes([(addr << 3) | flags, c1, c2, c3, c4])

    def _sender(self, gen, link):
        k = 0
        seq6 = 0
        seq4 = 0
        t0 = time.perf_counter()
        sent_samples = 0
        last_rate = self.control.rx_rate
        wb_period = 1.0 / self.args.wideband_rate if self.args.wideband_rate > 0 else 0
        next_wb = time.perf_counter()
        rng = np.random.default_rng(99)
        # With a receiver the packets are paced by its samples; while it delivers nothing (reconfiguration, stall)
        # the system clock paces packets of noise, so that the host keeps sending its TX stream.
        rx_paced = self.rx is not None
        reconfig_since = None
        while gen == self.stream_gen and not self.stop.is_set() and not link.closed:
            now = time.perf_counter()
            if self.wideband and wb_period and now >= next_wb:
                next_wb = now + wb_period
                for _ in range(EP4_PACKETS_PER_BLOCK):
                    if self.rx is not None:
                        wb = np.clip(self.rx.wideband * 32767, -32768, 32767)
                    else:
                        wb = np.clip(rng.normal(0, max(self.args.rx_noise, 1e-6) * 32767, EP4_SAMPLES), -32768, 32767)
                    link.send(bytes([0xEF, 0xFE, 0x01, 0x04]) + struct.pack(">I", seq4) + wb.astype("<i2").tobytes())
                    seq4 = (seq4 + 1) & 0xFFFFFFFF
            if not self.streaming:
                time.sleep(0.01)
                continue
            rate = self.control.rx_rate
            if rate != last_rate:
                # New sample rate: restart the pacing clock instead of bursting to the new rate.
                last_rate = rate
                t0 = time.perf_counter()
                sent_samples = 0
            num_rx = max(1, min(self.control.num_rx, 12))
            payloads, spf = self._payloads(num_rx)
            data = None
            if self.rx is not None:
                data = self.rx.take(2 * spf, rate, num_rx, 0.2 if rx_paced else 0.0)
                if data is not None:
                    reconfig_since = None
                    if not rx_paced:
                        rx_paced = True
                        print("receiver samples flowing")
                elif rx_paced and self.streaming and gen == self.stream_gen:
                    # A new rate or number of receivers takes the reader one capture block to apply; only a
                    # receiver that stays silent with the requested layout (or fails to apply it) is a stall.
                    now = time.perf_counter()
                    if self.rx.layout != (rate, num_rx) and (reconfig_since is None or now - reconfig_since < 1.0):
                        reconfig_since = reconfig_since or now
                        continue
                    reconfig_since = None
                    rx_paced = False
                    self.rx_stalls += 1
                    print("receiver delivers no samples: sending noise", file=sys.stderr)
                    t0 = time.perf_counter()
                    sent_samples = 0
            if data is None:
                due = (time.perf_counter() - t0) * rate
                if sent_samples > due:
                    time.sleep(min(0.002, (sent_samples - due) / rate))
                    continue
            frames = []
            for j in range(2):
                body = ep6_payload(data[j * spf : (j + 1) * spf]) if data is not None else payloads[k % len(payloads)]
                frames.append(SYNC + self._status(k) + body)
                k += 1
            if not link.send(bytes([0xEF, 0xFE, 0x01, 0x06]) + struct.pack(">I", seq6) + b"".join(frames)):
                time.sleep(0.01)
            seq6 = (seq6 + 1) & 0xFFFFFFFF
            if data is not None:
                continue
            sent_samples += 2 * spf
            # After a long stall do not burst to catch up: restart the pacing clock.
            if due - sent_samples > rate * 0.2:
                t0 = time.perf_counter()
                sent_samples = 0

    # ---- TCP ----

    def _allowed(self, addr):
        if address_allowed(self.args.allow, addr[0]):
            return True
        if addr[0] not in self.refused_hosts:
            self.refused_hosts.add(addr[0])
            print("ignoring %s: not in the --allow networks" % addr[0])
        return False

    def _accept_tcp(self):
        try:
            sock, addr = self.tcp_listen.accept()
        except OSError as e:  # for example EMFILE, or a connection reset before it was accepted
            print("TCP accept failed: %s" % e, file=sys.stderr)
            return
        if not self._allowed(addr):
            sock.close()
            return
        if len(self.tcp_clients) >= MAX_TCP_CLIENTS:
            # The oldest connection that is not streaming makes room for the new one.
            for old in list(self.tcp_clients):
                if self.link is None or self.link.tcp_sock is not old:
                    print("TCP connection from %s:%d closed: too many connections" % self.tcp_clients[old][0])
                    self._close_tcp(old)
                    break
        if len(self.tcp_clients) >= MAX_TCP_CLIENTS:
            sock.close()
            return
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.tcp_clients[sock] = (addr, bytearray(), HostLink(tcp_sock=sock, addr=addr))
        print("TCP connection from %s:%d" % addr)

    def _close_tcp(self, sock):
        entry = self.tcp_clients.pop(sock, None)
        if entry is None:
            return
        entry[2].closed = True
        if self.link is not None and self.link.tcp_sock is sock:
            self.streaming = False
            self.wideband = False
            self.stream_gen += 1
            self._host_mox(False)
            self.link = None
        try:
            sock.close()
        except OSError:
            pass

    def _read_tcp(self, sock):
        addr, buf, link = self.tcp_clients[sock]
        try:
            chunk = sock.recv(65536)
        except OSError:
            chunk = b""
        if not chunk:
            print("TCP connection from %s:%d closed" % addr)
            if self.link is not None and self.link.tcp_sock is sock:
                self.tx_release("cwx")
            self._close_tcp(sock)
            return
        buf.extend(chunk)
        while len(buf) >= PACKET_LEN and sock in self.tcp_clients:
            if buf[0] != 0xEF or buf[1] != 0xFE:
                # Lost framing: resynchronise on the next header.
                idx = buf.find(b"\xef\xfe", 1)
                del buf[: idx if idx > 0 else len(buf)]
                continue
            packet = bytes(buf[:PACKET_LEN])
            del buf[:PACKET_LEN]
            self.handle_command(packet, link)

    # ---- main loop ----

    def report(self):
        s = self.control.snapshot(("rx_rate", "num_rx", "tx_freq", "mox", "duplex"))
        host_freq = "%.6f MHz" % (s["tx_freq"] / 1e6) if s["tx_freq"] else "-"
        sensors = ""
        if self.tx.sdr is not None:
            sensors = " | fifo %.0f%% underflows %d late %d%s" % (self.tx.fifo_fill, self.tx.underflows, self.tx.late,
                                                                 " | PTT/relay TX" if self.tx.ptt else "")
        if self.rx is not None:
            sensors += " | rx %s MHz capture %.0f kS/s overflows %d dropped %d stalls %d%s" % (
                "%.6f" % (self.rx.center / 1e6) if self.rx.center else "-", (self.rx.capture_rate or 0) / 1e3,
                self.rx.overflows, self.rx.dropped, self.rx_stalls, " OVERLOAD" if self.rx.overload() else "")
        print("%s | rx %d kHz x%d | host TX %s%s | MOX %s | PTT %s | TX %s | ep2 %d lost %d reordered %d | tx %d samples dropped %d%s" % (
            self.link.describe() if self.link else "no host",
            s["rx_rate"] // 1000,
            s["num_rx"],
            host_freq,
            "" if s["duplex"] else " (simplex: RX1)",
            "on " if s["mox"] else "off",
            "on " if self._ptt() else "off",
            self.tx_source or "idle",
            self.ep2_packets,
            self.ep2_lost,
            self.ep2_late,
            self.tx.sent,
            self.tx.dropped,
            sensors,
        ))

    def serve(self):
        print("HPSDR Protocol 1 %s emulation on %s:%d UDP%s (MAC %s, firmware %d), %s" % (
            self.board.name, self.args.bind, self.args.port, "/TCP" if self.tcp_listen else "", self.mac.hex(":"),
            self.firmware, "transceiver (receiver %s)" % self.args.rx if self.rx is not None else "transmitter only"))
        if self.args.freq is not None:
            print("fixed TX frequency %.6f MHz (host frequency ignored)" % (self.args.freq / 1e6))
        else:
            print("following the host TX frequency%s" % (" + %.0f Hz" % self.args.freq_offset if self.args.freq_offset else ""))
        next_report = time.monotonic() + self.args.report
        try:
            while not self.stop.is_set():
                rlist = [self.udp] + list(self.tcp_clients)
                if self.tcp_listen is not None:
                    rlist.append(self.tcp_listen)
                ready, _, _ = select.select(rlist, [], [], 0.1)
                for sock in ready:
                    if sock is self.udp:
                        try:
                            data, addr = self.udp.recvfrom(2048)
                        except OSError as e:
                            # A transient socket error (ENOBUFS, an ICMP error reported on the socket, ...) must not
                            # stop the radio: reported once per error number, then the loop goes on.
                            if e.errno not in self.udp_errors:
                                self.udp_errors.add(e.errno)
                                print("UDP receive error: %s (continuing)" % e, file=sys.stderr)
                            continue
                        if len(data) >= 3 and data[0] == 0xEF and data[1] == 0xFE and self._allowed(addr):
                            link = self.link if self.link is not None and not self.link.is_tcp and self.link.addr == addr else None
                            self.handle_command(data, link or HostLink(udp_sock=self.udp, addr=addr))
                    elif sock is self.tcp_listen:
                        self._accept_tcp()
                    elif sock in self.tcp_clients:
                        self._read_tcp(sock)
                now = time.monotonic()
                if self.streaming and self.args.host_timeout > 0 and self.control.watchdog:
                    if now - self.last_host_packet > self.args.host_timeout:
                        print("host %s silent for %.1f s: stopping" % (self.link.describe(), self.args.host_timeout))
                        self._stop_stream()
                if self.args.report > 0 and now >= next_report:
                    next_report = now + self.args.report
                    self.report()
        except KeyboardInterrupt:
            pass
        finally:
            self.stop.set()
            self.streaming = False
            self.stream_gen += 1
            if self.keys is not None:
                self.keys.stop()
            self.tx.close()
            if self.rx is not None:
                self.rx.close()
            self.radio.close()
            for sock in list(self.tcp_clients):
                self._close_tcp(sock)
            if self.tcp_listen is not None:
                self.tcp_listen.close()
            self.udp.close()


def main():
    p = argparse.ArgumentParser(
        description="Emulate an openHPSDR Protocol 1 radio and transmit the host TX I/Q with SoapySDR rpitx.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("-f", "--freq", type=parse_frequency, default=None,
                   help="transmit frequency (e.g. 144.8M, 7074k, 28074000); when omitted the host TX frequency is followed")
    p.add_argument("--freq-offset", type=parse_frequency, default=0.0,
                   help="offset added to the host TX frequency when following it (transverter operation)")
    p.add_argument("--initial-freq", type=parse_frequency, default=parse_frequency("7.074M"),
                   help="frequency programmed before the host sends one (only without --freq)")
    p.add_argument("--retune-interval", type=float, default=0.1, help="minimum seconds between retunes when following the host")
    p.add_argument("--gain", type=int, default=7, choices=range(8), help="rpitx pad drive level 0-7")
    p.add_argument("--follow-drive", action="store_true", help="map the host drive level (0-255) to the pad drive level")
    p.add_argument("--amplitude-mode", choices=("envelope", "constant"), default="envelope",
                   help="envelope for SSB/AM, constant for FM, CW and constant-envelope digital modes")
    p.add_argument("--iq-gain", type=float, default=1.0, help="linear gain applied to the host I/Q")
    p.add_argument("--swap-iq", action="store_true", help="swap I and Q (inverts the sideband)")
    p.add_argument("--ignore-ptt", action="store_true", help="transmit the I/Q stream even when the host does not assert MOX")
    p.add_argument("--ppm", type=float, default=None, help="rpitx crystal correction in ppm")
    p.add_argument("--stream-args", default="", help="rpitx stream arguments, e.g. 'preroll_ms=150,fifo_ms=300'")
    p.add_argument("--queue-ms", type=float, default=100.0, help="maximum TX samples buffered between the network and rpitx, in ms")
    p.add_argument("--fifo-ms-hint", type=float, default=250.0,
                   help="rpitx FIFO length in ms (match fifo_ms of --stream-args); scales the Hermes-Lite 2 TX FIFO report")
    p.add_argument("--board", choices=sorted(BOARDS), default="hermes", help="board type reported to the host")
    p.add_argument("--fw-version", type=int, default=None, help="firmware version reported to the host (default: per board)")
    p.add_argument("--fw-minor", type=int, default=0, help="Hermes-Lite 2 gateware minor version")
    p.add_argument("--mercury-version", type=int, default=34, help="Mercury version reported by a Metis board")
    p.add_argument("--penelope-version", type=int, default=17, help="Penelope version reported by a Metis board")
    p.add_argument("--mac", type=parse_mac, default=None, help="MAC address reported to the host (default: derived from the host name)")
    p.add_argument("--rx-noise", type=float, default=1e-5, help="RMS level of the noise sent as receiver I/Q (0 = zeros)")
    p.add_argument("--wideband-rate", type=float, default=4.0, help="wideband (EP4) blocks per second when the host enables it")
    p.add_argument("--fwd-counts", type=int, default=0,
                   help="forward power ADC counts reported at full envelope and PAD 7 (0 = report no power)")
    p.add_argument("--supply-voltage", type=float, default=13.8, help="supply voltage reported to the host")
    p.add_argument("--key-dot-gpio", type=int, default=None, help="GPIO of the dot paddle or straight key (active low)")
    p.add_argument("--key-dash-gpio", type=int, default=None, help="GPIO of the dash paddle (active low)")
    p.add_argument("--ptt-gpio", type=int, default=None, help="GPIO of a PTT input reported to the host (active low)")
    p.add_argument("--gpio-chip", default="gpiochip0", help="GPIO chip of the key and PTT inputs")
    p.add_argument("--cw-ramp-ms", type=float, default=5.0, help="rise/fall time of the radio generated CW envelope")
    p.add_argument("--host-timeout", type=float, default=3.0, help="seconds without host packets before streaming stops (0 = never)")
    p.add_argument("--no-tcp", dest="tcp", action="store_false", help="do not accept Protocol 1 over TCP")
    p.add_argument("--bind", default="127.0.0.1",
                   help="local address to listen on (default: loopback only). Hosts on the network need the address of the "
                        "interface they reach, or 0.0.0.0; Protocol 1 has no authentication, so keep --allow narrow. A "
                        "socket bound to a unicast address does not receive broadcast discovery: enter the address in the host")
    p.add_argument("--allow", type=parse_allow, default="private",
                   help="hosts answered: 'any', 'private' (loopback, private and link-local addresses) and/or "
                        "comma-separated addresses or CIDR networks, e.g. '192.168.1.0/24,10.0.0.5'")
    p.add_argument("--takeover", action="store_true",
                   help="let a start from another address take the stream (and the transmitter) over while a host is streaming")
    p.add_argument("--port", type=int, default=HPSDR_PORT, help="UDP and TCP port")
    p.add_argument("--report", type=float, default=2.0, help="status report period in seconds (0 = off)")
    p.add_argument("--verbose", action="store_true", help="print every change of the host C&C settings")
    p.add_argument("--dry-run", action="store_true",
                   help="speak the protocol but do not open the transmitter (the receiver of --rx is still opened, on its own)")
    rx = p.add_argument_group("receiver (transceiver operation)")
    rx.add_argument("--rx", default=None, metavar="DRIVER",
                    help="SoapySDR driver of the receiver attached to rpitx, e.g. 'rtlsdr'; without it the receiver I/Q is noise")
    rx.add_argument("--rx-args", default="", help="receiver device arguments, e.g. 'serial=00000001,direct_samp=2'")
    rx.add_argument("--rx-stream-args", default="", help="receiver stream arguments, e.g. 'buffers=8,bufflen=16384'")
    rx.add_argument("--rx-gain", type=parse_gain, default="auto", help="receiver gain in dB, or 'auto' for its AGC")
    rx.add_argument("--follow-rx-gain", action="store_true",
                    help="set the receiver gain from the host attenuator (Hermes family) or LNA gain (Hermes-Lite 2)")
    rx.add_argument("--rx-antenna", default=None, help="receiver antenna (default: the receiver default)")
    rx.add_argument("--rx-ppm", type=float, default=0.0, help="receiver crystal correction in ppm")
    rx.add_argument("--rx-rate", type=parse_frequency, default=None,
                    help="receiver capture rate; must be a multiple of the host rate (default: smallest accepted multiple)")
    rx.add_argument("--rx-offset", type=parse_frequency, default=None,
                    help="capture centre minus RX1, keeps the receiver DC spur out of the band (default: a quarter of the "
                         "usable capture when it fits, else 0)")
    rx.add_argument("--rx-freq-offset", type=parse_frequency, default=None,
                    help="offset added to the host RX frequencies (up/down-converter; default: --freq-offset)")
    rx.add_argument("--rx-during-tx", action="store_true",
                    help="keep the receiver samples while transmitting (default: the driver mutes them while keyed)")
    relay = p.add_argument_group("antenna relay (T/R switching by the driver)")
    relay.add_argument("--relay-gpio", type=int, default=None,
                       help="GPIO of the antenna relay: TX position while the transmitter is keyed")
    relay.add_argument("--relay-active-low", action="store_true", help="the relay is in the TX position with the pin low")
    relay.add_argument("--relay-tx-delay-ms", type=int, default=20,
                       help="relay settling time before the PTT output is asserted (before the first TX sample without "
                            "--ptt-out-gpio)")
    relay.add_argument("--relay-hang-ms", type=int, default=100,
                       help="time the relay stays in TX (and the receiver muted) after the last TX sample")
    relay.add_argument("--ptt-out-gpio", type=int, default=None,
                       help="GPIO of the PTT output (amplifier or radio), asserted --relay-tx-delay-ms after the relay switched "
                            "to TX and released before the relay returns to RX")
    relay.add_argument("--ptt-out-active-low", action="store_true", help="the PTT output is asserted with the pin low")
    relay.add_argument("--ptt-tx-delay-ms", type=int, default=10,
                       help="time from the PTT output asserted to the first TX sample")
    relay.add_argument("--ptt-rx-delay-ms", type=int, default=10,
                       help="time from the PTT output released to the relay returning to RX")
    args = p.parse_args()

    if args.relay_gpio is not None and args.relay_gpio == 4:
        p.error("--relay-gpio cannot be GPIO4, the rpitx RF output")
    if args.relay_gpio is not None and args.dry_run:
        print("warning: --relay-gpio has no effect with --dry-run (the driver is not opened)", file=sys.stderr)
    if args.ptt_out_gpio is not None and args.ptt_out_gpio == 4:
        p.error("--ptt-out-gpio cannot be GPIO4, the rpitx RF output")
    if args.ptt_out_gpio is not None and args.ptt_out_gpio == args.relay_gpio:
        p.error("--ptt-out-gpio and --relay-gpio cannot be the same GPIO")
    for name in ("ptt_tx_delay_ms", "ptt_rx_delay_ms"):
        if not 0 <= getattr(args, name) <= 1000:
            p.error("--%s must be between 0 and 1000" % name.replace("_", "-"))
    if args.ptt_out_gpio is not None and args.dry_run:
        print("warning: --ptt-out-gpio has no effect with --dry-run (the driver is not opened)", file=sys.stderr)

    if not bind_is_loopback(args.bind):
        print("warning: listening on %s: Protocol 1 has no authentication, any host allowed by --allow (%s) that can reach "
              "this address can start the stream and key the transmitter; bind to loopback or narrow --allow" % (
                  args.bind, "any address" if args.allow is None else "see --allow"), file=sys.stderr)

    if not args.dry_run and hasattr(os, "geteuid") and os.geteuid() != 0:
        print("warning: rpitx needs root; run with sudo", file=sys.stderr)

    def terminate(signum, frame):
        raise KeyboardInterrupt

    # SIGTERM / SIGHUP (kill, systemd) and SIGINT (also when started in the background) stop the
    # transmitter cleanly.
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, terminate)

    try:
        bridge = Bridge(args)  # releases what it opened when it fails or is interrupted
    except KeyboardInterrupt:
        return 1
    bridge.serve()
    return 0


if __name__ == "__main__":
    sys.exit(main())
