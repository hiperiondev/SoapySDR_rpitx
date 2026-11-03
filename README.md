# SoapySDR_rpitx

**English** | [Español](README.es.md) | [Italiano](README.it.md)

**A SoapySDR transmit driver that turns a Raspberry Pi into a narrowband I/Q transmitter — no extra hardware,
just the GPIO4 clock pin.**

SoapySDR_rpitx exposes the Raspberry Pi general-purpose clock output (GPCLK0 on GPIO4) as a standard
[SoapySDR](https://github.com/pothosware/SoapySDR) TX device. Any Soapy-aware application — GNU Radio, SDRangel,
Python scripts, the bundled HPSDR bridge for piHPSDR / Thetis / linHPSDR / Quisk / SparkSDR — can stream
complex baseband samples to it and get RF out of the header pin. It is built on a vendored, hardened copy of
[librpitx](https://github.com/F5OEO/librpitx) by Evariste Courjaud F5OEO.

> **Status: experimental.** The transmit path (DMA ring, real-time feeder, PLL modulation, recovery) is covered
> by host-side tests on simulated hardware, including AddressSanitizer, UndefinedBehaviorSanitizer and
> ThreadSanitizer runs. Reports from real boards are very welcome.

---

## Contents

- [Radio and legal notice](#radio-and-legal-notice)
- [Capabilities](#capabilities)
- [Supported hardware](#supported-hardware)
- [System requirements and constraints](#system-requirements-and-constraints)
- [Building on the Raspberry Pi](#building-on-the-raspberry-pi)
- [Cross-compiling](#cross-compiling)
- [Installing and verifying](#installing-and-verifying)
- [Usage](#usage)
- [Tools](#tools)
- [How it works](#how-it-works)
- [Troubleshooting](#troubleshooting)
- [Testing](#testing)
- [Project layout](#project-layout)
- [License and credits](#license-and-credits)

---

## Radio and legal notice

GPIO4 outputs a **square wave**. It is rich in harmonics (odd harmonics at roughly 1/n amplitude) and in
spurious products of the fractional PLL. **Always** put a band-pass or low-pass filter for the band in use between
the pin and any antenna, and transmit only on frequencies and with powers you are licensed for — or into a dummy
load. You are responsible for what the pin radiates.

The driver keeps the carrier off whenever no samples are being played, and switches it off while the PLL is
being reprogrammed, but the clock tree is modified as soon as a stream is created.

---

## Capabilities

| Feature | Details |
|---------|---------|
| Direction | Transmit only, one channel |
| Sample formats | `CF32` (native, full scale 1.0), `CS16` (full scale 32768), `CS8` (full scale 128) |
| Sample rate | 10 kHz – 200 kHz (any value; listed 24 k / 48 k / 96 k / 192 k; default 48 kHz) |
| Frequency | 50 kHz – 1500 MHz (BCM2711: 50 kHz – 1000 MHz) on the fundamental; `harmonic=n` multiplies the range |
| Modulation | Any I/Q signal: phase/frequency through the PLLC fractional multiplier, amplitude through the pad drive strength (8 levels) |
| Amplitude modes | `envelope` (drive follows \|I/Q\|) or `constant` (full drive whenever a sample is present: FM, FSK, CW, constant-envelope PSK) |
| Gain | `PAD` element, 0–7 = 2 mA … 16 mA pad drive |
| Frequency correction | Kernel NTP estimate (`ntp_adjtime`) plus manual ppm; `setFrequencyCorrection()` |
| Bursts | `SOAPY_SDR_END_BURST` honoured: deactivation waits until the burst has left the antenna |
| Streaming | Lock-free FIFO, real-time feeder thread, configurable pre-roll, back-pressure with timeouts, partial writes |
| Live changes | Frequency, sample rate, gain and all settings while streaming, without losing queued samples |
| Diagnostics | Sensors for PLL lock, DMA state, FIFO fill, underflows, DMA overruns, DMA restarts and the actual sample rate; `readStreamStatus()` reports `SOAPY_SDR_UNDERFLOW` |
| Safety | System-wide lock, crash record and automatic recovery, optional signal cleanup, `rpitx_reset` tool, refusal of conflicting GPIO / I2S / DMA users |
| Integrations | GNU Radio (Soapy sink), SDRangel (SoapySDR output), Python, HPSDR Protocol 1 hosts through the bridge |

What the hardware does well: **NBFM, FSK/AFSK, WSPR/FT8-style constant-envelope digital modes, CW and BPSK**.
AM and SSB work, but with only 8 amplitude steps (and the carrier gated off below the lowest one) their quality
is fundamentally limited.

---

## Supported hardware

| Board | SoC | Status |
|-------|-----|--------|
| Pi 1 A/A+/B/B+, Zero, Zero W/WH, CM1 | BCM2835 | supported |
| Pi 2 B | BCM2836 / BCM2837 | supported |
| Pi 3 B/B+/A+, CM3, Zero 2 W | BCM2837 / BCM2837B0 / RP3A0 | supported |
| Pi 4 B, Pi 400, CM4 | BCM2711 | supported |
| Pi 5, Pi 500, CM5 | BCM2712 + RP1 | **not supported** (GPIO, clocks and DMA live in the RP1 chip) |

- **RF output: GPIO4, header pin 7** (GPCLK0, alternate function 0). Ground on pin 9.
- 32-bit (armhf) and 64-bit (arm64) Raspberry Pi OS are both supported.
- The board is identified from `/proc/device-tree/model` and the peripheral base in `/proc/device-tree/soc/ranges`
  (`0x20000000`, `0x3F000000` or `0xFE000000`). `SoapySDRUtil --find` lists the device only on supported boards.
- The BCM2711 high-peripheral mode (`arm_peri_high=1`) is not supported.

---

## System requirements and constraints

- **Root privileges.** The driver maps peripheral registers through `/dev/mem` and allocates DMA memory through
  the VideoCore mailbox `/dev/vcio`. Without access it reports a clear error; it never terminates the host
  application.
- **Fixed core clock.** PLLC is modulated, so the VPU core clock is moved to PLLA while a stream exists. Like the
  rpitx installer, keep the core frequency fixed:

  ```ini
  # /boot/firmware/config.txt (or /boot/config.txt on older releases)
  gpu_freq=250
  ```

  The clock tree is saved when the stream is created and restored when it is closed. While the stream is open,
  peripherals clocked from the VPU clock (mini-UART, I2C, SPI, and the SD host of Pi Zero/1/3) run at a different
  rate than the kernel believes.
- **GPU memory.** Each DMA ring slot uses 140 bytes of VideoCore memory (4 KiB pages): 4096 slots ≈ 560 KiB,
  32768 slots ≈ 4.4 MiB. Keep `gpu_mem` at its default (≥ 64 MB) for large rings.
- **GPIO4 must be free.** A kernel driver claiming GPIO4 — notably the `w1-gpio` (1-Wire) overlay, whose default
  pin is GPIO4 — makes the driver refuse the stream.
- **The PCM/I2S block must be free.** It is used as the DMA pacing clock; with an I2S sound-card overlay or
  `dtparam=i2s=on` the stream is refused.
- **One transmitter per system**, enforced with an `flock()` on `/run/lock/soapy_rpitx.lock`.
- **GPIO 0–9 functions are frozen while transmitting.** The DMA rewrites the whole `GPFSEL0` register on every
  sample (it is the only way to gate the clock pin from DMA). The functions of GPIO 0–9 are sampled when the stream
  is activated; do not reconfigure them (I2C on 2/3, SPI on 7–9, …) during transmission.
- **GPIO 0–27 drive strength follows the envelope.** The pad register `PADS_GPIO_0` is shared by GPIO 0–27 and is
  rewritten per sample (input hysteresis is preserved). Idle samples and the end of the stream restore the
  original value.

---

## Building on the Raspberry Pi

### Dependencies

```sh
sudo apt update
sudo apt install git cmake g++ libsoapysdr-dev soapysdr-tools
# optional, for the Python tools
sudo apt install python3-numpy python3-soapysdr python3-libgpiod
```

### Build and install

```sh
git clone https://github.com/hiperiondev/SoapySDR_rpitx.git
cd SoapySDR_rpitx
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure   # optional: host-side tests on simulated hardware
sudo cmake --install build
```

On a Pi Zero / Pi 1 use `-j1` (512 MB of RAM). The build takes a few minutes there and well under a minute on a Pi 4.

If SoapySDR itself was built from source into `/usr/local`, configure with `-DCMAKE_INSTALL_PREFIX=/usr/local`.

### CMake options

| Option | Default | Description |
|--------|---------|-------------|
| `RPITX_BUILD_DRIVER` | `ON` | Build the SoapySDR module (needs SoapySDR) |
| `RPITX_BUILD_RESET_TOOL` | `ON` | Build and install `rpitx_reset` |
| `RPITX_INSTALL_TOOLS` | `ON` | Install `rpitx_tone_test.py` and `hpsdr_rpitx_bridge.py` |
| `BUILD_TESTING` | `ON` (`OFF` when cross-compiling) | Build the host-side tests |
| `RPITX_WERROR` | `OFF` | Treat warnings (`-Wall -Wextra -Wshadow`) as errors |

SoapySDR is located through its CMake package (`SoapySDRConfig.cmake`, which provides `SOAPY_SDR_MODULE_UTIL`).
When the package configuration is missing (typically an incomplete cross sysroot), the build falls back to
finding `SoapySDR/Device.hpp` and `libSoapySDR.so` directly and reads the module ABI version from
`SoapySDR/Version.h`.

---

## Cross-compiling

### With `rpi-cross.sh` (Fedora or Debian-family x86_64 host)

`rpi-cross.sh` knows every Pi model (CPU, ISA, float ABI, Debian multiarch triplet), installs the cross
toolchain, builds a sysroot from a running Pi, writes a CMake toolchain file and checks the result.

```sh
./rpi-cross.sh models                                   # list model ids and the compiler flags used
./rpi-cross.sh deps    -m pi4 -b 64                     # install the cross toolchain: dnf on Fedora, apt on Debian/Ubuntu/Mint (auto-detected)
./rpi-cross.sh sysroot -m pi4 -b 64 -H pi@raspberrypi.local --pi-packages "libsoapysdr-dev" --ask-pass
./rpi-cross.sh build   -m pi4 -b 64                     # -> build-rpi-pi4-64/
./rpi-cross.sh verify  -m pi4 -b 64 build-rpi-pi4-64/librpitxSupport.so
./rpi-cross.sh install -m pi4 -b 64 -H pi@raspberrypi.local --ask-pass   # build + install on the Pi over SSH
```

- `-m` selects the board (`pi1 pizero cm1 pi2 pi2v12 pi3 pi3plus cm3 pizero2w pi4 pi400 cm4 pi5 pi500 cm5`); `-b` must match the
  **userland** of the Pi (`dpkg --print-architecture`: `armhf` → 32, `arm64` → 64).
- `sysroot` rsyncs `/usr/include`, `/usr/lib` (without kernel modules and firmware), `/usr/share/cmake`,
  `/usr/share/pkgconfig`, `/usr/local` and `/lib` from the Pi into `~/rpi-sysroot/<arch>` and makes absolute
  symlinks relative. `--pi-packages` installs packages on the Pi first; `libsoapysdr-dev` is required.
  `--ask-pass` lets the script use `sudo` on the Pi to read root-only files (the helper holding the password is
  created on a tmpfs and removed afterwards).
- Pi 1 / Zero / CM1 are ARMv6: the armhf compilers of Fedora and Debian/Ubuntu target ARMv7. Use an ARMv6-capable toolchain (for example a
  Bootlin `armv6-eabihf` glibc toolchain) with `--toolchain-dir <dir> --prefix arm-linux-`.
- Fedora's `gcc-<arch>-linux-gnu` cross compilers are built for kernels and ship no target `libgcc_s`: the
  script detects this and links the Pi's `libgcc_s.so.1` from the sysroot (shown by `info`), otherwise C++
  programs fail with an undefined `_Unwind_Resume`.
- The host distribution is read from `/etc/os-release` (`ID` / `ID_LIKE`): Fedora, RHEL and derivatives use `dnf`;
  Debian, Ubuntu, Linux Mint, Pop!_OS and other derivatives use `apt` (`g++-aarch64-linux-gnu` or
  `g++-arm-linux-gnueabihf`). `--distro fedora|debian` (or `RPI_DISTRO`) forces the choice; `info` shows what was
  detected. On Fedora the sysroot is mandatory (its cross-gcc has no target libc); on Debian/Ubuntu it is still used so
  the build sees the Pi's exact headers and libraries.
- `install` builds the project (skip with `--no-build`), checks the result with `verify`, and installs it on the
  running Pi through one SSH connection: `librpitxSupport.so` goes to SoapySDR's module directory (the first
  `Search path` reported by `SoapySDRUtil --info` on the Pi, e.g. `/usr/lib/aarch64-linux-gnu/SoapySDR/modules0.8`;
  override with `--module-dir`), `rpitx_reset` and the Python tools go to `/usr/local/bin` (`--pi-prefix`,
  `--no-tools`). It refuses to continue if the Pi's userland or SoapySDR ABI does not match the build (`--force`
  overrides), installs `soapysdr-tools` on the Pi if `SoapySDRUtil` is missing, and installs `--pi-packages` there.
  `sudo` on the Pi works as in `sysroot` (`--ask-pass`, `--pi-password`, passwordless sudo, or a root login), and
  nothing needs a reboot.
- `cmake-toolchain -o file.cmake` only writes the toolchain file, for use in an IDE or another build.
- `verify` checks the architecture, the float ABI and that the required glibc / libstdc++ symbol versions exist in
  the sysroot.

When cross-compiling, `LIB_SUFFIX` is forced to empty: SoapySDR 0.7.x would otherwise derive `lib64` from the
build host and miss the library in the sysroot's `usr/lib/<multiarch>`.

### Manually (any Linux host)

1. Install a cross compiler matching the Pi userland, e.g. on Debian/Ubuntu
   `sudo apt install g++-aarch64-linux-gnu` (arm64) or `g++-arm-linux-gnueabihf` (armhf, ARMv7+).
2. Copy a sysroot from the Pi (after `sudo apt install libsoapysdr-dev` on it):

   ```sh
   mkdir -p ~/rpi-sysroot
   rsync -a --exclude=/modules/ --exclude=/firmware/ pi@raspberrypi.local:/usr/lib     ~/rpi-sysroot/usr/
   rsync -a pi@raspberrypi.local:/usr/include ~/rpi-sysroot/usr/
   rsync -a pi@raspberrypi.local:/usr/share/cmake ~/rpi-sysroot/usr/share/
   ln -sfn usr/lib ~/rpi-sysroot/lib
   ```

   Absolute symlinks inside the copied `usr/lib` (for example `libm.so`) must be made relative, or point the
   linker at the right files with `-Wl,-rpath-link`.
3. Write a toolchain file:

   ```cmake
   # rpi-arm64.cmake
   set(CMAKE_SYSTEM_NAME Linux)
   set(CMAKE_SYSTEM_PROCESSOR aarch64)
   set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
   set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
   set(CMAKE_SYSROOT $ENV{HOME}/rpi-sysroot)
   set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
   set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
   set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
   set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
   ```

4. Build:

   ```sh
   cmake -S . -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=rpi-arm64.cmake -DCMAKE_BUILD_TYPE=Release
   cmake --build build-arm64 -j
   ```

### Deploying a cross build

`rpi-cross.sh install` does all of this automatically. To do it by hand, copy the outputs to the Pi:

```sh
scp build-rpi-pi4-64/librpitxSupport.so build-rpi-pi4-64/rpitx_reset pi@raspberrypi.local:
ssh pi@raspberrypi.local
SoapySDRUtil --info | grep -A3 "Search path"       # shows the module directory
sudo cp librpitxSupport.so /usr/lib/aarch64-linux-gnu/SoapySDR/modules0.8/   # use the path printed above
sudo cp rpitx_reset /usr/local/bin/
```

---

## Installing and verifying

`cmake --install` puts:

| File | Destination |
|------|-------------|
| `librpitxSupport.so` | `<libdir>/SoapySDR/modules<ABI>/` (SoapySDR's own module directory, e.g. `/usr/lib/aarch64-linux-gnu/SoapySDR/modules0.8/`) |
| `rpitx_reset` | `<prefix>/bin` |
| `rpitx_tone_test.py`, `hpsdr_rpitx_bridge.py` | `<prefix>/bin` |

No reboot and no manual copy are needed. Check:

```sh
SoapySDRUtil --info                         # "rpitx" in "Available factories"
sudo SoapySDRUtil --find="driver=rpitx"     # the board model and serial number
sudo SoapySDRUtil --probe="driver=rpitx"    # channels, ranges, settings, sensors
sudo rpitx_tone_test.py --freq 144.800M     # 30 s NBFM test tone (dummy load!)
```

---

## Usage

Device string: **`driver=rpitx`**. Applications must run as root.

### Python

```python
import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_TX, SOAPY_SDR_CF32, SOAPY_SDR_END_BURST

sdr = SoapySDR.Device(dict(driver="rpitx", amplitude_mode="constant"))
sdr.setSampleRate(SOAPY_SDR_TX, 0, 48000)
sdr.setFrequency(SOAPY_SDR_TX, 0, 144.800e6)
sdr.setGain(SOAPY_SDR_TX, 0, 7)

st = sdr.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, [0], {"preroll_ms": "100"})
sdr.activateStream(st)

fs, dev, tone = 48000, 2500, 1000
n = np.arange(fs * 5)                                      # 5 s NBFM tone
iq = np.exp(1j * (dev / tone) * np.sin(2 * np.pi * tone * n / fs)).astype(np.complex64)
for i in range(0, len(iq), 4096):
    sr = sdr.writeStream(st, [iq[i:i + 4096]], len(iq[i:i + 4096]), timeoutUs=1000000)

sdr.writeStream(st, [np.zeros(0, np.complex64)], 0, SOAPY_SDR_END_BURST)
sdr.deactivateStream(st)                                    # waits for the burst to be sent
sdr.closeStream(st)
```

### GNU Radio and SDRangel

- **GNU Radio:** *Soapy Custom Sink* (or *Soapy Sink*), device `driver=rpitx`, type `fc32`, sample rate 48000, the
  centre frequency, and optional stream arguments such as `preroll_ms=150,fifo_ms=300`. Run `gnuradio-companion`
  or the generated script as root.
- **SDRangel:** add a *SoapySDR output* device and select the rpitx device; settings appear in the device panel.

### Stream semantics

- `writeStream()` blocks while the FIFO is full, honours `timeoutUs`, may accept fewer samples than offered (the
  return value is the number consumed) and returns `SOAPY_SDR_TIMEOUT` when nothing could be queued in time.
  `SOAPY_SDR_HAS_TIME` is not supported (`SOAPY_SDR_NOT_SUPPORTED`). The MTU is `min(4096, max(FIFO/4, 256))`.
- `SOAPY_SDR_END_BURST` on the last write (or on a zero-length write) marks the end of the burst: the DMA starts
  immediately even below the pre-roll, and after the last sample the carrier stays off.
- `deactivateStream()` turns the carrier off. If the last write carried `END_BURST`, it first waits (for the FIFO
  plus ring duration plus 200 ms at most) until every queued sample has been played; the device mutex is not held
  meanwhile, so the control API stays usable. Without `END_BURST`, queued samples are discarded.
- `readStreamStatus()` returns `SOAPY_SDR_UNDERFLOW` (channel mask 1) once for every new FIFO starvation inside a
  burst, DMA overrun or DMA restart, otherwise `SOAPY_SDR_TIMEOUT`.
- Frequency, sample rate, ppm and harmonic can change while streaming: the feeder stops the DMA, switches the
  carrier off, reprograms the PLL and the pacing clock and restarts with the samples that were already in the DMA
  ring (none lost, at most one sent twice), followed by the FIFO.

### Stream arguments

| Key | Default | Range | Description |
|-----|---------|-------|-------------|
| `ring_size` | 85 ms of samples, ≥ 4096, rounded to 512 | 512 – 32768, and ≥ 10 ms at the sample rate | DMA ring size in samples |
| `fifo_ms` | 250 | 20 – 5000 | Software FIFO capacity in ms (follows later sample-rate changes) |
| `preroll_ms` | 100 | 0 – 5000 | Signal queued before the DMA starts |
| `dma_channel` | -1 | -1 – 14 | DMA channel; -1 = first free channel not owned by the kernel |
| `dma_force` | false | bool | Reset an explicitly selected channel even when it is active |
| `rt_priority` | 50 | 0 – 99 | `SCHED_FIFO` priority of the feeder thread (0 = normal scheduling) |
| `cpu` | -1 | -1 – 63 | CPU the feeder thread is pinned to |
| `signal_cleanup` | false | bool | Turn the carrier off when the process is killed by a signal left at its default action |

Latency from `writeStream()` to the antenna ≈ FIFO fill + ¾ of the ring (≈ 64 ms for a 4096-slot ring at 48 kHz).
An application that writes faster than real time keeps the FIFO full: latency ≈ `fifo_ms` + ¾ ring.

### Settings (also accepted as device arguments)

| Key | Default | Description |
|-----|---------|-------------|
| `ppm` | 0 | Manual crystal correction, −200 … 200 ppm. **Positive when the crystal runs slow**, i.e. when the board transmits below the requested frequency (same sign as the kernel NTP estimate) |
| `ppm_ntp` | true | Use the kernel NTP frequency estimate as the base correction (added to `ppm`) |
| `harmonic` | 1 | Transmit on this harmonic of the square wave: the PLL runs at frequency / harmonic (1 – 15) |
| `amplitude_mode` | envelope | `envelope` or `constant` |
| `iq_scale` | 1.0 | Gain applied to \|I/Q\| before amplitude quantisation (envelope mode), 0 – 100 |
| `debug_level` | 0 | librpitx diagnostics on stderr: 0 errors, 1 configuration, 2 register dumps |

Example: `driver=rpitx,amplitude_mode=constant,ppm=1.5`. Unknown device arguments are reported with a warning.

### Sensors

| Key | Type | Description |
|-----|------|-------------|
| `pll_locked` | bool | PLLC lock bit (`CM_LOCK.FLOCKC`) |
| `dma_running` | bool | The feeder started the DMA and the channel `ACTIVE` bit is set |
| `fifo_fill` | float % | Software FIFO fill level |
| `underflows` | int | FIFO ran empty in the middle of a burst (application too slow) |
| `late` | int | DMA engine overtook the feeder (feeder thread stalled) |
| `dma_errors` | int | DMA engine stopped by itself and was restarted |
| `sample_rate` | float Hz | Rate actually produced by the PCM clock dividers |

---

## Tools

### `rpitx_tone_test.py`

Test transmitter: NBFM tone, or a WAV file modulated as FM, AM, USB, LSB or sent as I/Q (stereo WAV, L = I,
R = Q). Prints the sensors every second.

```sh
sudo rpitx_tone_test.py --freq 144.8M                         # 1 kHz NBFM tone for 30 s
sudo rpitx_tone_test.py --freq 7.074M --wav voice.wav --mod usb
sudo rpitx_tone_test.py --freq 144.8M --jitter-ms 20 --args "preroll_ms=200,fifo_ms=500"
sudo rpitx_tone_test.py --freq 144e6 --seconds 0              # open/close only: recovers a crashed session
```

### `rpitx_reset`

Returns the hardware to idle after a process using the driver died: recovers the session record (see
[Crash recovery](#crash-recovery)); without a record it switches a clock output left on GPIO4 off and stops GPCLK0.
`--dma-channel N` also resets DMA channel N (refused when the kernel owns it). Suitable for `ExecStopPost=` in a
systemd unit.

### `hpsdr_rpitx_bridge.py` — HPSDR Protocol 1 bridge

Makes the Pi look like an openHPSDR Protocol 1 radio on the network, so piHPSDR, Thetis, PowerSDR mRX, linHPSDR,
Quisk or SparkSDR discover it and transmit through rpitx.

```sh
sudo hpsdr_rpitx_bridge.py                          # follow the host TX frequency, Hermes emulation
sudo hpsdr_rpitx_bridge.py --freq 144.8M            # fixed TX frequency
sudo hpsdr_rpitx_bridge.py --freq-offset 116M       # transverter: host 28.8 MHz -> 144.8 MHz
sudo hpsdr_rpitx_bridge.py --amplitude-mode constant --follow-drive
sudo hpsdr_rpitx_bridge.py --key-dot-gpio 17 --key-dash-gpio 27 --ptt-gpio 22
hpsdr_rpitx_bridge.py --dry-run --verbose           # protocol only, no transmitter, no root
```

| Area | Implementation |
|------|----------------|
| Discovery | UDP broadcast/directed on port 1024 and TCP; MAC, firmware, board id, receivers; Hermes-Lite 2 extra bytes |
| Boards | `metis`, `hermes` (default), `hermes2`/`griffin`, `angelia`, `orion`, `orion2`, `hermeslite` |
| Start / stop | EP6 I/Q and EP4 wideband, over UDP or TCP; another host takes over with a start; a stop from another host is ignored |
| Host C&C (EP2) | Every address decoded (rate, receivers, duplex, TX/RX NCOs, drive, filters, attenuators, mic, PureSignal, CW keyer, HL2 extensions); `--verbose` logs changes |
| Status (EP6) | PTT/dot/dash, IO inputs inactive, firmware, forward power (`--fwd-counts`), supply voltage, HL2 temperature, TX FIFO level and acknowledges |
| Receive I/Q | Synthetic noise or zeros at 48/96/192/384 kHz, silent microphone; paced so the host TX stream runs at 48 kHz |
| Transmit | Host TX I/Q while MOX is set, on the TX NCO (or RX1 when duplex is off) |
| CW | Iambic A/B or straight keyer on GPIO paddles with host timing, HL2 CWX, GPIO PTT input |
| Watchdog | Streaming stops and TX is released after `--host-timeout` s without host packets |

Notes: in SSB with a silent microphone there is no carrier (in `envelope` mode magnitudes below 0.5 / (PAD + 1)
of full scale switch the carrier off; the bridge prints a hint). With piHPSDR's *CW handled in Radio* the host does
not assert MOX for CW: connect paddles to the GPIO inputs or disable that option. piHPSDR limits Protocol 1 radios
to 61.44 MHz; use `--freq-offset` or a transverter LO for VHF.

---

## How it works

### Overview

```
 application thread              feeder thread (SCHED_FIFO 50, 1 ms period)         DMA engine (hardware)
 ──────────────────              ───────────────────────────────────────────         ─────────────────────
 writeStream()                    poll DMA position (CONBLK_AD)                      per ring slot:
   │ CS16/CS8 → CF32               │ absolute position tracking (laps, overruns)      CB0  PAD      ← drive
   ▼                               │ scrub played slots with idle                     CB1  PLLC_FRAC← frequency
 SpscFifo<complex<float>> ───────► │ IQ → (amplitude, Δφ) → register words ─────►     CB2  GPFSEL0  ← carrier on/off
 (lock-free, back-pressure)        │ keep ring ¾ full, idle padding when starving     CB3  PCM FIFO ← paced by DREQ
                                   │ retune / DMA restart / underflow accounting       └─ next → closed loop
```

Each I/Q sample becomes one **ring slot**: three 32-bit words in VideoCore memory (fractional PLL multiplier,
pad drive word, `GPFSEL0` word) and four DMA control blocks that copy them to the peripherals. The fourth control
block writes a dummy word into the PCM transmit FIFO and is paced by the PCM DREQ line, so the engine advances
exactly one slot per sample period. The last control block points back to the first: the DMA plays the ring
forever without CPU intervention, and the feeder thread only has to stay ahead of it.

### Frequency synthesis: modulating PLLC

The carrier is GPCLK0 driven by **PLLC**. When a stream is created, the driver enables librpitx "advanced PLL
mode":

1. The clock-tree registers about to change are saved (`CORECLK_CNTL/DIV`, `EMMCCLK_CNTL/DIV`, `CM_GP0CTL/DIV`,
   `CM_PLLC`, `PLLC_CTRL/FRAC`, `PLLC_CORE0/1/2`, `PLLC_PER`, `A2W_PLLC_ANA0..3`) and recorded in the session file.
2. The VPU core clock is moved to PLLA (divider 4) and the EMMC clock to PLLD, so that nothing else depends on PLLC.
3. `PLLC_CORE0` is disabled, the PLLC peripheral channel is enabled with divider 1 (2 on BCM2711) and GPCLK0 is
   sourced from it, MASH 0 (integer divider, no noise shaping spurs).

For a carrier F (the PLL frequency F/harmonic), `ComputeBestLO()` chooses the GPCLK0 integer divider D: it scans
D from 4095 down to 2 for a VCO rate F·D between 200 MHz and 1.5 GHz such that the whole band F ± B (B = sample
rate) maps to the same **integer** PLL multiplier. Above a 750 MHz carrier (375 MHz on BCM2711) the PLL pre-divider is used
instead (GPCLK divider 2). The VCO multiplier is

```
M = (F + f) · K / (XOSC · (1 − ppm·10⁻⁶))          f = instantaneous offset from the carrier
K = D (BCM283x),  2·D (BCM2711, PLLC_PER = /2),  ½ or 2 with the pre-divider
XOSC = 19.2 MHz (BCM283x) or 54 MHz (BCM2711)
```

`M` is split into the integer part `PLLC_CTRL.NDIV` (programmed once per tune) and the 20-bit fractional part
`PLLC_FRAC`, the only register the DMA rewrites. The frequency resolution is `XOSC / 2²⁰ / K`: e.g. 1.83 Hz at
144 MHz on a Pi 3 (D = 10) and 5.15 Hz on a Pi 4 (D = 5, K = 10). If an offset would need another integer
multiplier, the fractional value saturates at 0 or 0xFFFFF instead of wrapping; when the chosen divider cannot
keep the full band inside one integer step, a warning is logged. After programming, PLLC lock (`CM_LOCK.FLOCKC`)
is awaited for up to 2 ms and a warning is logged if it does not lock.

The clock generators (GPCLK0, PCM, EMMC) are always reprogrammed with the hardware sequence: clear `ENAB`, wait for
`BUSY` to clear (kill after 1 ms), write divider and source, enable.

### From I/Q to register words

For every sample the feeder computes (`dsp::pushsample()`):

- **amplitude** a = |s|;
- **instantaneous frequency** f = wrap(arg(sₙ) − arg(sₙ₋₁)) · fs / 2π, with the phase difference wrapped to
  [−π, π), so f ∈ [−fs/2, fs/2).

Samples below a magnitude gate of 1e-3 have meaningless phase: they report f = 0 (carrier at the centre
frequency) and the last valid phase is kept, so the next valid sample applies the **whole** phase change across
the gap. A BPSK reversal through an exact zero therefore still turns the carrier by π.

The PLL realises a quantised frequency. The driver computes the frequency actually produced by the chosen
fractional word (`GetMasterFracFrequency()`), and the resulting phase error is **carried to the next sample**
(first-order error feedback, bounded to ±0.5 rad), so the carrier phase tracks the I/Q phase without drift over
long PSK transmissions.

The three words of a data slot:

| Word | Value |
|------|-------|
| `PLLC_FRAC` | `0x5A000000 \| frac(f / harmonic + carry)` |
| `PADS_GPIO_0` | `0x5A000000 \| HYST(original) \| SLEW \| level` — envelope mode: `level = round(min(a·iq_scale, 1)·(drive+1)) − 1`, carrier off when negative; constant mode: `level = drive` |
| `GPFSEL0` | GPIO 0–9 functions sampled at activation, with GPIO4 = ALT0 (carrier on) or input (carrier off) |

An **idle slot** holds the fractional word of the centre frequency, the original pad value and GPIO4 = input: the
GPCLK keeps running internally but nothing leaves the pin.

### Sample-rate pacing: the PCM block

The PCM/I2S controller is configured as a one-channel, 8-bit transmitter whose frame length (`PCM_MODE.FLEN`) is
the smallest value ≥ 10 that keeps the PCM clock divider between 2 and 4095. The PCM clock comes from PLLD with
MASH 1 and a 12-bit fractional divider, corrected by the same ppm as the carrier. The TX FIFO DREQ threshold is
64 words; every frame consumes one word, so each `CB3` write blocks until the FIFO has room — one slot per frame.
The produced rate (`sample_rate` sensor) differs from the nominal one by the divider quantisation, below 1 ppm.

### DMA engine and memory

- **Memory:** one contiguous VideoCore allocation (`mem_alloc` with `MEM_FLAG_DIRECT` on BCM2836+, the uncached
  `0xC` alias, or `MEM_FLAG_L1_NONALLOCATING` on BCM2835; `HINT_PERMALOCK`), locked to obtain its bus address and
  mapped into the process through `/dev/mem`. Control blocks first (32-byte aligned), then the slot words:
  `ring × (4·32 + 3·4)` = 140 bytes per slot. Every mailbox reply is checked (ioctl result, response code, tag
  response bit).
- **Control blocks:** `TI = NO_WIDE_BURSTS | WAIT_RESP`, plus `D_DREQ | PERMAP(2)` (PCM TX) for the pacing block;
  one 4-byte transfer each, destination the peripheral bus address (`0x7E102220` PLLC_FRAC, `0x7E10002C`
  PADS_GPIO_0, `0x7E200000` GPFSEL0, `0x7E203004` PCM FIFO).
- **Channel:** `CS = PRIORITY(7) | PANIC_PRIORITY(7) | DISDEBUG | ACTIVE`. Only legacy channels are used (0–14,
  0–10 on BCM2711 where 11–14 are DMA4 channels with another control-block format). The kernel's channels
  (`brcm,dma-channel-mask` of every `soc/dma@…` node) are never touched; the default is the first channel of
  7, 8, 9, 10, 6, 5 (BCM2711) or 14 … 5 that is neither the kernel's nor active. An explicit channel that is active
  is only reset with `dma_force=true`.

### The feeder thread

The feeder runs every 1 ms (`clock_nanosleep` on an absolute `CLOCK_MONOTONIC` deadline), with `SCHED_FIFO`
priority `rt_priority` and optional CPU affinity. Positions are tracked as **64-bit absolute slot counts**
(`DmaRingTracker`), because the engine only reports its position modulo the ring:

- `readPos` advances by the modular distance since the previous poll; when the elapsed time × rate exceeds half a
  ring, whole laps that a modular distance cannot show are added back
  (`laps = round((expected − delta) / ring)`).
- `fill = writePos − readPos`; `fill ≤ 0` means the engine overtook the writer: an **overrun** (`late`). The whole
  ring except the executing slot is then overwritten with idle samples and writing restarts a guard distance
  (`max(32, rate/500)`, ≤ ¼ ring) ahead of the engine.
- Each tick it **scrubs** the completed slots that have not been rewritten with newer data (overwrites them with
  idle), so that if the feeder ever stalls the engine plays silence, never a replay of the previous lap.
- It moves samples from the FIFO into the ring up to a **target fill of ¾ ring**, never closer than 2 slots to the
  executing one.
- When the FIFO is empty and the fill drops below `low = max(ring/8, rate/200)`, it pads with idle slots; if a
  burst was in progress without `END_BURST`, an **underflow** is counted.
- If the DMA position is invalid or the channel is no longer `ACTIVE` for 5 consecutive polls, the engine is
  considered stopped: the event is logged with `DMA_CS` / `DMA_DEBUG`, `dma_errors` is incremented and the stream
  restarts with the unplayed samples.

**Start-up and pre-roll:** after activation the feeder fills the ring while the DMA is stopped. The DMA starts at
slot 0 when `preroll_ms` of signal is queued, when the burst end is signalled, when the FIFO is full, or when the
first queued sample has waited `preroll_ms` (so short bursts still go out).

**Retune:** the control thread posts the request with a sequence number and waits up to 3 s for the feeder to
take it (then until it completes). The feeder records the exact engine position, stops the DMA, **switches GPIO4
to input and restores the pads** before touching the PLL, copies the unplayed slots of the ring (the original I/Q
samples are kept in a shadow array) into a replay buffer, reprograms PLLC and the PCM clock, resets the ring and
resumes: the replay buffer is sent before the FIFO. A request that the PLL cannot satisfy leaves the previous
configuration in effect.

**Drain detection:** `WaitDrained()` compares the FIFO's pushed counter with a consumed counter that the feeder
publishes only *after* the samples are in the ring and the end-of-data position is updated, so a burst is never
reported as sent while its last samples are in flight between the FIFO and the ring.

### The FIFO

`SpscFifo` is a bounded lock-free single-producer/single-consumer queue with 64-bit head and tail counters
(acquire/release), so the fill level is always `head − tail`. Its storage is allocated for `fifo_ms` at the
maximum sample rate; the usable capacity is `max(rate·fifo_ms/1000, 1024)` and is updated on every retune. A
blocked producer waits on a condition variable in 2 ms slices, woken by the feeder when it frees space.

### Shutdown, crash recovery and safety

- **Normal close** (`deactivateStream` / `closeStream` / device destruction): the feeder is joined, the DMA reset,
  GPIO4 returned to its original function (input if it was the clock output), the pads restored, the PCM block
  disabled and its clock restored, the clock tree restored (PLLC first, waiting for lock, then its channels, the
  VPU and EMMC clocks and GPCLK0), the VideoCore memory freed and the lock file cleared.
- **`exit()`:** an `atexit` handler performs an emergency stop (DMA reset, carrier off, GPCLK0 stopped) without
  invalidating the objects, so a later destructor still completes the shutdown.
- **Signals** (`signal_cleanup=true`): handlers for SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGSEGV, SIGBUS, SIGFPE, SIGILL
  and SIGABRT. When the previous disposition was the default action (the process is about to die) they perform
  async-signal-safe register writes (DMA reset, GPIO4 input, pads, GPCLK0 off), restore the previous action and
  re-raise; ignored signals and application handlers are passed through untouched.

#### Crash recovery

While a transmitter exists, `/run/lock/soapy_rpitx.lock` (tmpfs, never survives a reboot) is held with an
exclusive `flock()` and contains a `key=value` record of everything the process owns: PID, DMA channel,
VideoCore memory handle, original pad value and GPIO4 function, original PCM clock and the saved clock tree. The
kernel releases the lock when the process dies, even with `kill -9`. The next process that acquires the lock and
finds a non-empty record knows the owner died without cleaning up and recovers: DMA channel reset, GPIO4 and pads
restored, GPCLK0 stopped, PCM disabled and its clock restored, clock tree restored, and the dead process's GPU
memory released (VideoCore handles are global to the firmware). `rpitx_reset` does the same on demand.

#### Conflict checks at open

- another process holding the lock → "the transmitter is in use by process N";
- GPIO4 claimed by any consumer on the SoC GPIO chip (`pinctrl-bcm2…`, via the GPIO character-device line info);
- `soc/i2s@7e203000` enabled in the device tree;
- the requested DMA channel owned by the kernel, or active without `dma_force`;
- a warning when GPCLK0 is already running (another program may use it).

### Module hygiene

The SoapySDR module is linked with hidden visibility and `--no-undefined`: the vendored librpitx symbols are not
exported, so it cannot clash with another librpitx in the same process. librpitx never calls `exit()`; every
hardware failure becomes a `std::runtime_error` that the application receives from SoapySDR.

---

## Troubleshooting

| Symptom | Cause and remedy |
|---------|------------------|
| `No devices found` | `SoapySDRUtil --info` must list `rpitx` (module directory); run as root; Pi 5 family is not supported |
| "/dev/mem and the VideoCore mailbox are not accessible" | Run as root (or give the binary access to both devices) |
| "the transmitter is in use by process N" | Another program owns the transmitter; close it |
| "DMA channel N is reserved for the Linux kernel" / "is active" | Pick another `dma_channel` or leave -1; `dma_force=true` only for a channel known to be free |
| "GPIO4 is claimed by …" | Remove the overlay that uses the pin (e.g. `dtoverlay=w1-gpio`) and reboot |
| "the PCM/I2S controller is enabled" | Remove the sound-card overlay / `dtparam=i2s=on` and reboot |
| "VideoCore memory allocation failed" | Lower `ring_size`, raise `gpu_mem`, or run `sudo rpitx_reset` to free memory left by a crash |
| `underflows` grows | The application is too slow: raise `preroll_ms` and `fifo_ms`, write larger blocks |
| `late` grows | The feeder was delayed: keep `rt_priority` > 0, pin it (`cpu=3`), consider `isolcpus=3`, the `performance` governor |
| `dma_errors` grows | Something else reset or used the DMA channel; choose another channel |
| "crosses a PLL integer step" | Deviations beyond the step are clipped: move the frequency slightly or lower the sample rate |
| "PLLC did not report lock" | Very high frequencies may not lock on every chip; lower the frequency or use `harmonic` |
| Serial console / I2C / SPI garbled while transmitting | Expected: the VPU clock is on PLLA while the stream is open (`gpu_freq=250` keeps it predictable) |
| Carrier stuck after a crash | `sudo rpitx_reset` (or open any stream): the session record is recovered |
| No carrier in SSB with the HPSDR bridge | Silent microphone; see the bridge notes above |

---

## Testing

```sh
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

The tests run on any Linux host; `tests/mock_hw.cpp` replaces the mailbox, `/dev/mem`, the device-tree queries and
the DMA engine (a thread that executes the control-block ring at the sample rate and applies its `GPFSEL0` words).

| Test | Covers |
|------|--------|
| `rpitx_unit_tests` | FIFO ordering under concurrency and capacity limit; ring accounting against a simulated engine (steady state, short stalls, lost laps, double stall, host starvation); DSP frequency estimation, gating and phase continuity through zero |
| `rpitx_transmitter_tests` | Feeder, pre-roll, drain, retune with the carrier off and ring replay, emergency stop then destruction, DMA restart, pad hysteresis, FIFO capacity vs rate, constructor failures (unreachable frequency, refused allocation, short ring), DMA channel selection, session lock and crash recovery, signal handlers |
| `rpitx_driver_tests` | SoapySDR device: settings and ranges, frequency correction, stream arguments, CF32/CS16/CS8 scaling, `END_BURST` drain, stream status, sensors |

CI (`.github/workflows/ci.yml`) runs the tests in a plain build, with AddressSanitizer + UndefinedBehaviorSanitizer
and with ThreadSanitizer (`tests/tsan.supp` suppresses only the simulated-register accesses), and checks
`clang-format`, `shellcheck` on `rpi-cross.sh` and `pyflakes` on the Python tools.

---

## Project layout

```
src/
  SoapyRPITX.hpp                 SoapySDR device class and stream wrapper
  SoapyRPITX_Registration.cpp    board detection, device enumeration, registry entry
  SoapyRPITX_Settings.cpp        identification, settings, antenna, gain, frequency, sample rate
  SoapyRPITX_Streaming.cpp       stream setup, format conversion, write / status / burst handling
  SoapySDR_Sensors.cpp           sensors
  SoapyRPITX_IQ_dmaSync.*        transmitter: DMA ring, feeder thread, modulation, retune, shutdown
  SoapyRPITX_Buffers.hpp         SpscFifo and DmaRingTracker (hardware independent)
  SoapyRPITX_Session.*           lock file, crash record and recovery, conflict checks
librpitx/src/                    vendored librpitx (dma, gpio/clock/PCM/pads, mailbox, dsp, device tree)
tools/
  rpitx_reset.cpp                recovery tool
  rpitx_tone_test.py             test transmitter (tone / WAV)
  hpsdr_rpitx_bridge.py          openHPSDR Protocol 1 radio emulation
tests/                           unit, transmitter and driver tests, hardware mock, TSan suppressions
rpi-cross.sh                     cross-compilation helper (Fedora / Debian-family host)
```

---

## License and credits

GPL-3.0 — see [LICENSE](LICENSE).

- Copyright © 2023 Emiliano Augusto Gonzalez (hiperion), <https://github.com/hiperiondev/SoapySDR_rpitx>.
- Based on [librpitx](https://github.com/F5OEO/librpitx), Copyright © 2018 Evariste Courjaud F5OEO; the vendored
  copy and its differences from upstream are described in [librpitx/README.md](librpitx/README.md).
- Device structure modelled on [SoapyPlutoSDR](https://github.com/pothosware/SoapyPlutoSDR).
- PLL divider search adapted from [PiFmRds](https://github.com/SaucySoliton/PiFmRds) (SaucySoliton's fork);
  angle helper from F4GKR's [RadiantBee](https://github.com/f4gkr/RadiantBee).
