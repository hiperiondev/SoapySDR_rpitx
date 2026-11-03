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
- [Precise amplitude and EER](#precise-amplitude-and-eer)
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
| Modulation | Any I/Q signal: phase/frequency through the PLLC fractional multiplier, amplitude through the pad drive strength (8 levels, calibrated tables, optional sigma-delta noise shaping and a second clock pin) or an external envelope DAC (EER) |
| Amplitude modes | `envelope` (drive follows \|I/Q\|) or `constant` (full drive whenever a sample is present: FM, FSK, CW, constant-envelope PSK); envelope output `none` (pad drive), `pcm` (16-bit I2S DAC) or `pwm` |
| Gain | `PAD` element, 0–7 = 2 mA … 16 mA pad drive |
| Frequency correction | Kernel NTP estimate (`ntp_adjtime`) plus manual ppm; `setFrequencyCorrection()` |
| Bursts | `SOAPY_SDR_END_BURST` honoured: deactivation waits until the burst has left the antenna |
| Streaming | Lock-free FIFO, real-time feeder thread, configurable pre-roll, back-pressure with timeouts, partial writes |
| Live changes | Frequency, sample rate, gain and all settings while streaming, without losing queued samples |
| Diagnostics | Sensors for PLL lock, DMA state, FIFO fill, underflows, DMA overruns, DMA restarts, the actual sample rate and envelope clipping; `readStreamStatus()` reports `SOAPY_SDR_UNDERFLOW` |
| Safety | System-wide lock, crash record and automatic recovery, optional signal cleanup, `rpitx_reset` tool, refusal of conflicting GPIO / I2S / DMA users |
| Integrations | GNU Radio (Soapy sink), SDRangel (SoapySDR output), Python, HPSDR Protocol 1 hosts through the bridge |

What the hardware does well: **NBFM, FSK/AFSK, WSPR/FT8-style constant-envelope digital modes, CW and BPSK**.
AM and SSB work; with the bare pin their quality is limited by the 8 amplitude steps (see
[Precise amplitude and EER](#precise-amplitude-and-eer) for calibration, noise shaping and an external envelope
modulator).

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
- **Fixed core clock.** PLLC is modulated, so the VPU core clock is moved to PLLA while a stream exists (the stream
  is refused when the PLLA peripheral channel is not running). Like the rpitx installer, keep the core frequency
  fixed:

  ```ini
  # /boot/firmware/config.txt (or /boot/config.txt on older releases)
  gpu_freq=250
  ```

  The clock tree is saved when the stream is created and restored when it is closed. While the stream is open,
  peripherals clocked from the VPU clock (mini-UART, I2C, SPI, and the SD host of Pi Zero/1/3) run at a different
  rate than the kernel believes.
- **GPU memory.** Each DMA ring slot uses 140 bytes of VideoCore memory (4 KiB pages; 144 bytes with an envelope
  DAC, 180 bytes with the RF-DAC and an envelope DAC): 4096 slots ≈ 560 KiB, 32768 slots ≈ 4.4 MiB. Keep `gpu_mem`
  at its default (≥ 64 MB) for large rings.
- **GPIO4 must be free.** A kernel driver claiming GPIO4 — notably the `w1-gpio` (1-Wire) overlay, whose default
  pin is GPIO4 — makes the driver refuse the stream.
- **The PCM/I2S block must be free.** It is used as the DMA pacing clock (and as the envelope DAC with
  `envelope_output=pcm`); with an I2S sound-card overlay or `dtparam=i2s=on` the stream is refused. With
  `envelope_output=pwm` the PWM block paces the DMA instead and must be free: no `pwm` overlay and
  `dtparam=audio=off`.
- **The kernel DMA channel mask must be readable** from the device tree (`soc/dma@…` or `soc/dma-controller@…`,
  property `dma-channel-mask` or `brcm,dma-channel-mask`) for the automatic channel choice; otherwise set
  `dma_channel` explicitly. The crash record notes that the mask was unreadable, so the recovery still resets that
  channel.
- **One transmitter per system**, enforced with an `flock()` on `/run/soapy_rpitx/soapy_rpitx.lock`. The directory
  is created with mode 0700 and must belong to the user running the driver; a lock file that is not a regular file
  owned by that user (for example a symbolic link) is refused.
- **GPIO 0–9 functions are frozen while transmitting.** The DMA rewrites the whole `GPFSEL0` register on every
  sample (it is the only way to gate the clock pin from DMA). The functions of GPIO 0–9 are sampled when the stream
  is activated; do not reconfigure them (I2C on 2/3, SPI on 7–9, …) during transmission. With `rf_dac=true` the same
  applies to GPIO 20–29 (`GPFSEL2`).
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
| `RPITX_INSTALL_TOOLS` | `ON` | Install `rpitx_tone_test.py`, `hpsdr_rpitx_bridge.py`, `rpitx_amp_calibrate.py` and `rpitx_autotune.py` |
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
| `rpitx_tone_test.py`, `hpsdr_rpitx_bridge.py`, `rpitx_amp_calibrate.py`, `rpitx_autotune.py` | `<prefix>/bin` |

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

- Timeouts (`timeoutUs` of `writeStream()` and `readStreamStatus()`) are limited to one day, so `LONG_MAX` can be
  used for "wait forever".
- `writeStream()` blocks while the FIFO is full, honours `timeoutUs`, may accept fewer samples than offered (the
  return value is the number consumed) and returns `SOAPY_SDR_TIMEOUT` when nothing could be queued in time.
  `SOAPY_SDR_HAS_TIME` is not supported (`SOAPY_SDR_NOT_SUPPORTED`). The MTU is `min(4096, max(FIFO/4, 256))`.
- `SOAPY_SDR_END_BURST` on the last write (or on a zero-length write) marks the end of the burst: the DMA starts
  immediately even below the pre-roll, and after the last sample the carrier stays off.
- `activateStream()` on an active stream has no effect (the counters keep running, no event is reported).
- `deactivateStream()` turns the carrier off. If the last write carried `END_BURST`, it first waits (at most the
  FIFO duration at the sample rate, plus two rings at the slot rate, plus 200 ms) until every queued sample has been
  played; the device mutex is not held
  meanwhile, so the control API stays usable. Without `END_BURST`, queued samples are discarded.
- `closeStream()` (and destroying the device) turns the carrier off at once and discards queued samples, also while
  another thread waits in `deactivateStream()`; the hardware is free for a new stream right after. A `writeStream()` or
  `readStreamStatus()` running in another thread is woken up and returns before the stream is destroyed (calls in
  flight are counted and `closeStream()` waits for them), so closing during a shutdown is safe.
- `readStreamStatus()` returns `SOAPY_SDR_UNDERFLOW` (channel mask 1) once for every new FIFO starvation inside a
  burst, DMA overrun or DMA restart, otherwise `SOAPY_SDR_TIMEOUT`.
- Frequency, sample rate, ppm and harmonic can change while streaming: the feeder stops the DMA, switches the
  carrier off, reprograms the PLL and the pacing clock and restarts with the samples that were already in the DMA
  ring (none lost, at most one sent twice), followed by the FIFO. Idle slots before the first queued sample are not
  replayed; an idle gap inside a burst (FIFO starvation) is replayed as idle.

### Stream arguments

| Key | Default | Range | Description |
|-----|---------|-------|-------------|
| `ring_size` | 85 ms of slots, ≥ 4096, rounded to 512 | 512 – 32768, and ≥ 10 ms at the slot rate | DMA ring size in slots (one slot per sample, or `slots_per_sample` per sample) |
| `fifo_ms` | 250 | 20 – 5000 | Software FIFO capacity in ms (follows later sample-rate changes) |
| `preroll_ms` | 100 | 0 – 5000 | Signal queued before the DMA starts |
| `dma_channel` | -1 | -1 – 14 | DMA channel; -1 = first free channel not owned by the kernel |
| `dma_force` | false | bool | Reset an explicitly selected channel even when it is active |
| `rt_priority` | 50 | 0 – 99 | `SCHED_FIFO` priority of the feeder thread (0 = normal scheduling) |
| `cpu` | -1 | -1 – 63 | CPU the feeder thread is pinned to |
| `signal_cleanup` | false | bool | Turn the carrier off when the process is killed by a signal left at its default action |
| `slots_per_sample` | 1 | 1 – 4 | DMA slots per I/Q sample; the I/Q signal is interpolated and the amplitude quantised at the slot rate (sample rate × slots ≤ 200 kHz) |
| `envelope_output` | none | `none`, `pcm`, `pwm` | Envelope output for EER: `pcm` = 16-bit I2S on GPIO18/19/21, `pwm` = PWM duty on `pwm_gpio` |
| `pwm_gpio` | 18 | 12 or 18 | Pin of the PWM envelope output (GPIO18 ALT5, GPIO12 ALT0) |
| `pwm_bits` | 8 | 6 – 12 | Resolution of the PWM envelope (period of 2^bits PWM clocks) |
| `rf_dac` | false | bool | GPIO20 as a second clock output, summed with GPIO4 by an external weighted network |

Latency from `writeStream()` to the antenna ≈ FIFO fill + ¾ of the ring (≈ 64 ms for a 4096-slot ring at 48 kHz).
An application that writes faster than real time keeps the FIFO full: latency ≈ `fifo_ms` + ¾ ring.

### Settings (also accepted as device arguments)

| Key | Default | Description |
|-----|---------|-------------|
| `ppm` | 0 | Manual crystal correction, −200 … 200 ppm. **Positive when the crystal runs slow**, i.e. when the board transmits below the requested frequency (same sign as the kernel NTP estimate) |
| `ppm_ntp` | true | Use the kernel NTP frequency estimate as the base correction (added to `ppm`); the estimate is used in every clock state except an unsynchronised clock (it is kept around leap seconds) |
| `harmonic` | 1 | Transmit on this harmonic of the square wave: the PLL runs at frequency / harmonic (1 – 15) |
| `amplitude_mode` | envelope | `envelope` or `constant` |
| `iq_scale` | 1.0 | Gain applied to \|I/Q\| before amplitude quantisation (envelope mode), 0 – 100 |
| `debug_level` | 0 | librpitx diagnostics on stderr: 0 errors, 1 configuration, 2 register dumps |
| `amplitude_lut` | empty | Measured relative RF amplitude of pad drive levels 0–7 (8 increasing values, comma separated); empty = `(level + 1) / 8` |
| `ampm_lut` | empty | Measured phase of pad drive levels 0–7 in degrees (8 values); compensated on the carrier |
| `amplitude_quantizer` | nearest | `nearest` or `sigma_delta` (noise-shaped amplitude selection) |
| `sigma_delta_order` | 2 | Order of the error feedback, 1 or 2 |
| `envelope_delay` | 0.5 | Envelope delay relative to the phase path in samples, −32 … 32 (0.5 aligns the two paths of the bare pin) |
| `envelope_floor` | 0 | Envelope below which the carrier is switched off after 2 ms (0 = never) |
| `rfdac_lut` | empty | Relative amplitude with GPIO4 only, GPIO20 only and both clock pins (3 values); empty = 2/3, 1/3, 1 |
| `eer_lut` | empty | RF amplitude measured at equally spaced envelope codes from 0 to full scale (2–64 increasing values); empty = linear |
| `eer_ampm_lut` | empty | Phase measured at the same codes, in degrees (2–64 values) |
| `eer_gain`, `eer_offset` | 1, 0 | Gain and offset applied to the envelope code |
| `eer_idle` | 0 | Envelope code while nothing is transmitted (fraction of full scale) |
| `eer_drive` | 7 | Pad drive of the clock pins while the envelope DAC is used |
| `profile` | empty | Profile file: its settings and `gain` are applied at once, its `stream.*` lines become the defaults of the next stream (see [Profile files](#profile-files)); empty forgets the stream defaults |

Example: `driver=rpitx,amplitude_mode=constant,ppm=1.5`. Unknown device arguments are reported with a warning,
except the identification keys (`driver`, `label`, `product`, `serial`, `manufacturer`, `tuner`, `type`) and keys
containing `:`.
Integers are decimal (a leading zero does not mean octal) and numbers must not carry trailing characters.

### Profile files

`profile=<file>` (a device argument, or `writeSetting("profile", file)` at run time) loads a text file of
`key=value` lines, such as the `profile_<path>.rpitx` written by [`rpitx_autotune.py`](#rpitx_autotunepy):

```ini
# comment
ppm=1.37
amplitude_lut=0.12,0.27,0.42,0.56,0.69,0.8,0.9,1
envelope_delay=0.62
stream.slots_per_sample=2
stream.fifo_ms=300
gain=7
```

- Setting keys are applied in file order; a setting also given as a device argument takes precedence over the file.
  A refused value stops the loading with an error that names the file (the load is all-or-nothing: nothing is changed, and with an open stream there is at most one retune).
- `stream.<key>` lines become the defaults of `setupStream()`; the arguments passed by the application take
  precedence.
- `gain` sets the PAD gain (0–7).
- Blank lines and lines starting with `#` are ignored, unknown keys are reported with a warning and ignored, a line
  without `=` is an error, and a later line replaces an earlier one with the same key.
- `readSetting("profile")` returns the file loaded last; `writeSetting("profile", "")` forgets its stream defaults
  (the settings stay).
- The file must be a regular file of at most 64 KiB that group and others cannot write (mode 0644 or stricter);
  symbolic links are not followed, and devices, pipes and sockets are refused without being read. The driver runs
  as root and the setting can arrive from a remote client (SoapyRemote), so nothing else is opened.

### Sensors

| Key | Type | Description |
|-----|------|-------------|
| `pll_locked` | bool | PLLC lock bit (`CM_LOCK.FLOCKC`) |
| `dma_running` | bool | The feeder started the DMA and the channel `ACTIVE` bit is set |
| `fifo_fill` | float % | Software FIFO fill level |
| `underflows` | int | FIFO ran empty in the middle of a burst (application too slow) |
| `late` | int | DMA engine overtook the feeder (feeder thread stalled) |
| `dma_errors` | int | DMA engine stopped by itself and was restarted |
| `sample_rate` | float Hz | I/Q sample rate actually produced by the pacing clock dividers |
| `envelope_clip` | int | Slots whose envelope (\|I/Q\| × `iq_scale`, or the EER code) exceeded full scale |
| `envelope_late` | int | Envelope words that could not be written ahead of their sample (ring nearly empty) |

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

The `key=value` lists of the tools (`--args`, `--stream-args`, `--rx-args`) are checked before any radio is
opened: an item without `=` stops the tool with a message naming it.

### `rpitx_amp_calibrate.py`

Measures the amplitude path with a SoapySDR receiver (an RTL-SDR next to a dummy load is enough) and prints the
settings to use. Each state is alternated with a full-scale reference so the phase drift between the two radios
cancels; the receiver is tuned `--offset` Hz below the carrier.

```sh
sudo rpitx_amp_calibrate.py pad   --freq 7.1M --rx-args driver=rtlsdr         # amplitude_lut, ampm_lut
sudo rpitx_amp_calibrate.py rfdac --freq 7.1M                                 # rfdac_lut (rf_dac=true)
sudo rpitx_amp_calibrate.py eer   --freq 7.1M --envelope-output pcm --points 17   # eer_lut, eer_ampm_lut
sudo rpitx_amp_calibrate.py delay --freq 7.1M --envelope-output pcm           # envelope_delay (two-tone IMD3)
rpitx_amp_calibrate.py self-test                                              # analysis check, no radio
```

The printed tables are already in the form the driver accepts: amplitude tables strictly increasing (a value
that measurement noise left out of order is raised minimally, with a warning), phases within ±180°, and the
phase of the EER code 0 (no carrier) taken from code 1.

### `rpitx_autotune.py`

Closed-loop over-the-air test and tuning. The tool transmits known test bursts in every modulation family,
receives them with a SoapySDR receiver (an RTL-SDR next to a dummy load and an attenuator is enough), measures the
impairments against the transmitted waveform and adjusts the driver settings and stream arguments until every
selected mode meets its thresholds or a budget is exhausted. The bare-pin path and the EER path are tuned
separately (`--path non-eer|eer|both`).

```sh
sudo rpitx_autotune.py run --freq 144.39M --confirm-dummy-load                     # bare pin, default modes
sudo rpitx_autotune.py run --freq 7.1M --path eer --envelope-output pcm \
     --rx-args driver=rtlsdr,direct_samp=2 --confirm-dummy-load                    # EER path on HF
sudo rpitx_autotune.py run --freq 144.39M --path both --modes nbfm,fsk,bpsk,qpsk,usb --max-minutes 30 --confirm-dummy-load
sudo rpitx_autotune.py run --freq 144.39M --resume rpitx_autotune_results/checkpoint.json --confirm-dummy-load
sudo rpitx_autotune.py verify --freq 144.39M --profile rpitx_autotune_results/profile_non_eer.json --confirm-dummy-load
rpitx_autotune.py run --simulate good --freq 144.39M                               # simulated bench, no radio
rpitx_autotune.py replay --out rpitx_autotune_results                              # re-analyse saved captures
rpitx_autotune.py self-test [--quick]                                              # analysis and optimiser checks, no radio
```

Every burst is a lead-in carrier, a gap, an MSK sync word, the payload, a second sync word and a tail carrier. From
each capture the tool measures the carrier frequency error and the sample clock error (from the delay between the
two sync words), aligns the capture sample by sample with the transmitted signal and fits a short linear filter that
absorbs the passband of the receiver; the remaining error and demodulators of each mode give the metrics.

| Modes | Metrics |
|-------|---------|
| `cw` | keying edge timing, leakage with the key up, key clicks |
| `nbfm`, `wbfm` (sample rate ≥ 180 kHz) | deviation error, SINAD |
| `fsk` (300 Bd), `afsk` (Bell 202), `mfsk` (8-FSK) | bit / symbol errors, tone frequency error |
| `bpsk` (PSK31 shaping), `qpsk`, `8psk`, `qam16`, `ofdm` | EVM, bit / symbol errors, phase wander, ACPR |
| `am` | distortion of the demodulated tone |
| `usb`, `lsb` | two-tone IMD3, opposite sideband suppression, residual carrier |
| `iq_raw` | long wideband burst: robustness counters and waveform error |

Every mode also checks the frequency error, the sample clock error, the `underflows`, `late`, `dma_errors` and
`envelope_late` counters and, in envelope mode, `envelope_clip`. The default modes are all except `wbfm` and `ofdm`.

The tuning stages run in this order (`--stages` selects them, `--stage-limit S4=40,...` limits their evaluations):

| Stage | Parameters | Method |
|-------|------------|--------|
| S0 | `rt_priority`, `cpu`, `fifo_ms`, `preroll_ms`, `ring_size` | escalated until two long `iq_raw` bursts run with all counters at zero |
| S1 | `ppm` | carrier error of a CW burst; with `--freq-ref tx` the receiver correction is tuned instead, with `--freq-ref external` the receiver is first calibrated on the carrier at `--ref-freq` |
| S2 | PAD gain of the constant-envelope modes, or `eer_drive` | lowest drive within `--drive-margin` dB of the largest output |
| S3 | `amplitude_lut`, `ampm_lut` (`rfdac_lut` with `--rf-dac-available`), or `eer_lut`, `eer_ampm_lut` | reference / state measurement of `rpitx_amp_calibrate.py`, then refined in a closed loop; the sign with which the driver applies the AM/PM table is measured, not assumed |
| S4 | `envelope_delay` | lowest two-tone IMD3 (grid, then golden section) |
| S5 | `amplitude_quantizer`, `sigma_delta_order`, `slots_per_sample` (and `rf_dac`) | every combination, scored on QPSK EVM and IMD3 without giving up ACPR (bare pin only) |
| S6 | `iq_scale` (EER: also `eer_gain`, `eer_offset`) | best OFDM EVM without `envelope_clip` |
| S7 | `envelope_floor` (EER: also `eer_idle`) | no noise in the pauses without cutting the signal |

All selected modes are then verified (`--verify-repeats` bursts, the worst value counts). The stages that can
change a failing metric are run again, up to `--max-rounds` rounds. A new value is accepted only when it beats the
current one by more than `--min-delta` and `--sigma-k` times the measured spread.

Every metric ends as `PASS`, `FAIL`, `UNVERIFIABLE` (the threshold is closer than `--floor-margin` dB to what the
bench can resolve: the receiver noise, and `--rx-sfdr` for products next to a strong signal), `HARDWARE_LIMIT`
(every enabled stage that could improve it was tuned again without success), `INFEASIBLE` or `INVALID`. The run stops when
everything passes, at `--max-trials`, `--max-minutes` or `--max-tx-seconds` (`BUDGET_EXHAUSTED`), or on Ctrl-C
(`ABORTED`: the burst ends with `END_BURST`, the stream is closed and the checkpoint kept). `--duty` limits the
transmit duty cycle. The exit code is 0 only when everything passes (`--allow-unverifiable` also accepts
`UNVERIFIABLE`).

The output directory (`--out`) holds `profile_<path>.json` (input of `verify`), `profile_<path>.rpitx` (driver
profile file, see [Profile files](#profile-files)), `profile_<path>.args` (device
arguments, the table settings to apply with `writeSetting()` — they contain commas and cannot be part of a device
argument string —, stream arguments and gains), `report.md`, `report.json`, `checkpoint.json` (for `--resume`) and
`captures/` (`--save-captures fail|all`, for `replay`). The default thresholds are built in and are starting
values; override them with `--thresholds file.json`, for example
`{"non_eer": {"qpsk": {"evm_db": -20}}, "global": {"freq_abs_hz": 2}}`.

Further options:

| Option | Values (default first) | Effect |
|--------|------------------------|--------|
| `--profile-format` | `both`, `args`, `driver` | writes `profile_<path>.args`, the driver profile `profile_<path>.rpitx`, or both |
| `--profile-gain`, `--profile-amplitude-mode` | `envelope`, `constant`; `none`, `envelope`, `constant` | gain (of the envelope or of the constant-envelope modes) and `amplitude_mode` stored in the `.rpitx` file |
| `--check-driver-profile` | flag | after tuning, verifies every mode again with the driver opened with `profile=<.rpitx file>` |
| `verify --apply` | `settings`, `driver-profile` | applies the profile (`.json` or `.rpitx`) with `writeSetting()`, or through `profile=` |
| `--rx-host`, `--rx-remote` | `local`, `remote`; `tcp://host:55132` | receiver on this host, or through SoapyRemote (`--rx-args` then describes the remote device: `driver=rtlsdr` becomes `remote:driver=rtlsdr`) |
| `--rx-local-policy` | `warn`, `refuse`, `ignore` | what to do with a local USB receiver on a Pi Zero / 1 / 2 / 3 or CM1 / CM3 (USB shared with Ethernet, or a single core) |
| `--required-modes`, `--required-modes-eer` | `default`, `all`, `none`, list | modes that decide the outcome; the others are reported as optional. `default`: on the bare pin the constant-envelope modes, `bpsk`, `usb`, `lsb` and `iq_raw`; on the EER path all |
| `--allow-hardware-limit`, `--allow-hardware-limit-eer` | list, `all`, `none` (`qam16,ofdm` and `none`) | modes whose `HARDWARE_LIMIT` counts as passed |
| `--threshold-set` | `auto`, `non-eer`, `eer` | threshold table of each path, or the same table for every path (`eer` is the stricter one) |
| `--tune-optional` | `yes`, `no` | whether failing optional modes also trigger re-tuning |
| `--freqs` | list, e.g. `7.1M,14.2M,144.39M` | one tuning per band in `<out>/<band>/`, then `bands.md` / `bands.json` with every parameter per band, classified `independent` or `dependent` |
| `--band-reuse` | `none`, `start`, `skip` | every band from the defaults, from the profile of the previous band, or from it skipping the `--band-independent-stages` (default `S0,S5`) |
| `--band-tolerance` | `0.05` | values agree across bands when they differ by at most this times max(1, \|value\|), element by element for tables |
| `--budget-scope` | `band`, `run` | budget limits per band or for the whole run |
| `--resume` | file, `auto` | `auto` resumes every output directory (every band) from its own checkpoint |

Bench notes: always a band filter and a dummy load, and enough attenuation that the receiver sees at most about
−30 dBm (an EER amplifier can deliver watts). The receiver can be on the same Pi or on another host through
SoapyRemote (`--rx-host remote --rx-remote tcp://host:55132`); on a Pi Zero / 1 / 3 the USB
receiver shares the bus with Ethernet and can disturb the feeder thread (stage S0 shows it; `--rx-local-policy`
warns by default). `--rx-offset` keeps
the DC spike and the image of the receiver away from the signal. The frequency must be inside the range of both
radios (on HF an RTL-SDR needs direct sampling or an upconverter). `--freq-ref rx` (default) trusts the receiver
clock after `--rx-ppm`: use a TCXO receiver for absolute accuracy. The profile is specific to the board, the
frequency, the load and the temperature.

### `rpitx_reset`

Returns the hardware to idle after a process using the driver died: recovers the session record (see
[Crash recovery](#crash-recovery)); without a record it switches a clock output left on GPIO4 off and stops GPCLK0.
`--dma-channel N` also resets DMA channel N (refused when the kernel owns it, and refused when the kernel DMA channel
mask cannot be read from the device tree unless `--force` is given for a channel known to be free). Suitable for
`ExecStopPost=` in a systemd unit.

### `hpsdr_rpitx_bridge.py` — HPSDR Protocol 1 bridge

Makes the Pi look like an openHPSDR Protocol 1 radio on the network, so piHPSDR, Thetis, PowerSDR mRX, linHPSDR,
Quisk or SparkSDR discover it and transmit through rpitx.

```sh
sudo hpsdr_rpitx_bridge.py                          # follow the host TX frequency, Hermes emulation
sudo hpsdr_rpitx_bridge.py --freq 144.8M            # fixed TX frequency
sudo hpsdr_rpitx_bridge.py --freq-offset 116M       # transverter: host 28.8 MHz -> 144.8 MHz
sudo hpsdr_rpitx_bridge.py --amplitude-mode constant --follow-drive
sudo hpsdr_rpitx_bridge.py --key-dot-gpio 17 --key-dash-gpio 27 --ptt-gpio 22
sudo hpsdr_rpitx_bridge.py --bind 192.168.1.50 --allow 192.168.1.20   # reachable from the LAN: this interface, this host only
hpsdr_rpitx_bridge.py --dry-run --verbose           # protocol only, no transmitter, no root
```

| Area | Implementation |
|------|----------------|
| Discovery | UDP broadcast/directed on port 1024 and TCP; MAC, firmware, board id, receivers; Hermes-Lite 2 extra bytes |
| Boards | `metis`, `hermes` (default), `hermes2`/`griffin`, `angelia`, `orion`, `orion2`, `hermeslite` |
| Start / stop | EP6 I/Q and EP4 wideband, over UDP or TCP; while a host streams, a start from another address is refused unless `--takeover` is given (the same address can restart); a stop from another host is ignored |
| Host C&C (EP2) | Every address decoded (rate, receivers, duplex, TX/RX NCOs, drive, filters, attenuators, mic, PureSignal, CW keyer, HL2 extensions); `--verbose` logs changes |
| Status (EP6) | PTT/dot/dash, IO inputs inactive, firmware, forward power (`--fwd-counts`), supply voltage, HL2 temperature, TX FIFO level and acknowledges |
| Receive I/Q | Synthetic noise or zeros at 48/96/192/384 kHz, silent microphone; paced so the host TX stream runs at 48 kHz |
| Transmit | Host TX I/Q while MOX is set, on the TX NCO (or RX1 when duplex is off) |
| CW | Iambic A/B or straight keyer on GPIO paddles with host timing, HL2 CWX, GPIO PTT input |
| Watchdog | Streaming stops and TX is released after `--host-timeout` s without host packets |
| Access | Listens on loopback by default (`--bind`); only hosts of `--allow` are answered: `private` (default: loopback, private and link-local addresses), `any`, or comma-separated addresses / CIDR networks; only the host that started the stream can key the transmitter; at most 4 TCP connections (the oldest idle one makes room) |

Security: Protocol 1 has no authentication and the bridge runs as root and drives RF, so access is decided by the
source address only (on UDP, IP and port), which anyone on the same network segment can spoof. The bridge therefore
listens on loopback unless `--bind` names an interface (`--bind 0.0.0.0` listens everywhere; a warning is printed
whenever it is not loopback), keep `--allow` to the host that runs the SDR program, and put the Pi on a network you
trust. A socket bound to one address does not receive broadcast discovery: enter the Pi's address in the host
program instead of relying on automatic discovery (or bind `0.0.0.0` on a trusted network).

Notes: in SSB with a silent microphone there is no carrier (in `envelope` mode magnitudes below 0.5 / (PAD + 1)
of full scale switch the carrier off; the bridge prints a hint). With piHPSDR's *CW handled in Radio* the host does
not assert MOX for CW: connect paddles to the GPIO inputs or disable that option. piHPSDR limits Protocol 1 radios
to 61.44 MHz; use `--freq-offset` or a transverter LO for VHF.

---

## Precise amplitude and EER

The bare pin modulates amplitude through the drive strength of the output pad: 8 levels plus "off", roughly a
3-bit quantiser whose steps are not even linear (the drive setting changes the output resistance, and the top
levels saturate). The driver offers three ways to do better, which can be combined.

### 1. Calibrated tables and noise shaping (no extra hardware)

- `amplitude_lut` replaces the linear model `(level + 1) / 8` by the measured amplitude of each level, so the
  nearest level is really the nearest; `ampm_lut` compensates the phase shift of each level on the carrier.
  Measure both with `rpitx_amp_calibrate.py pad` on the band in use.
- `amplitude_quantizer=sigma_delta` feeds the quantisation error back (order 1 or 2): the average amplitude follows
  the envelope exactly and the error becomes noise pushed away from the carrier. The in-band improvement grows with
  the oversampling ratio (slot rate / signal bandwidth); `slots_per_sample` raises the slot rate further (the I/Q
  signal is interpolated). **The shaped noise lands up to ± slot rate / 2 from the carrier, in the neighbouring
  channels**, where a band filter does not remove it: use a high slot rate and a narrow output filter, and check
  the spectrum.
- `envelope_delay` aligns the envelope with the phase. The default 0.5 sample matches the held envelope of a slot
  with the phase ramp the PLL produces during it; a two-tone test (`rpitx_amp_calibrate.py delay`) finds the best
  value for a given setup.
- `envelope_floor` switches the carrier off when the envelope stays below a level for 2 ms (silence between
  words), instead of transmitting the quantisation noise.

### 2. RF-DAC: a second clock pin (two resistors and a transformer)

GPCLK0 is also available on **GPIO20** (ALT5). With `rf_dac=true` the DMA gates GPIO4 and GPIO20 independently;
summed through weighted resistors (e.g. GPIO4 through R and GPIO20 through 2R into a broadband transformer) the
two pins and the 8 drive levels give about three times more amplitude steps. Measure the real weights with
`rpitx_amp_calibrate.py rfdac` (`rfdac_lut`). GPIO 20–29 functions are frozen while transmitting.

### 3. Envelope Elimination and Restoration (external envelope modulator)

With `envelope_output=pcm` or `pwm`, GPIO4 carries only the **phase**: it runs at the fixed drive `eer_drive` for
every sample of a burst and drives a switching power amplifier (class D/E, e.g. paralleled BS170 or an IRF510
stage behind a 74AC buffer). The **envelope** is written by the same DMA slot into the pacing FIFO and leaves on a
DAC that drives the supply modulator of that amplifier (a power op-amp for QRP, or a buck/hybrid modulator):

```
I/Q ─► phase path: PLLC frac + GPIO4 on, fixed drive ──────────► GPIO4 ─► buffer ─► class D/E PA ─► filter ─► antenna
     └► envelope path: eer_lut⁻¹ → 16-bit code (same DMA slot) ─► DAC ─► supply modulator ─┘ V_DD(t)
```

- **`pcm`**: 16-bit stereo I2S master, 64 bit clocks per frame, both channels carry the envelope. Wire a
  **DC-coupled** I2S DAC (e.g. PCM5102A, SCK tied low): GPIO18 → BCK, GPIO19 → LRCK, GPIO21 → DIN. AC-coupled codecs
  cannot output an envelope. Code 0 … 32767 = envelope 0 … full scale (the positive half of the output).
- **`pwm`**: PWM channel 1 in mark/space mode on GPIO18 (ALT5) or GPIO12 (ALT0), period 2^`pwm_bits` PWM clocks,
  one period per slot; follow it with an LC/RC low-pass and a buffer, or drive the switch of a buck modulator. The
  PWM block then also paces the DMA (the PCM block is not used). Requires `dtparam=audio=off`.

Both paths come from one DMA slot, so they are sample-synchronous by construction. The DAC word leaves the pacing
FIFO a fixed number of slots after the registers of its slot take effect (DREQ level + 1: 17 slots for PCM, 9 for
PWM); the driver writes each envelope word that many slots earlier, and `envelope_delay` compensates what remains
(DAC filter delay, modulator bandwidth). Calibrate the supply modulator and the amplifier with
`rpitx_amp_calibrate.py eer` (`eer_lut`, `eer_ampm_lut`) and the delay with the two-tone test. Below
`envelope_floor` the clock pin is switched off too, which removes the drive feed-through of the amplifier during
silence.

The envelope bandwidth of SSB is several times the audio bandwidth: 48 kS/s is enough for 3 kHz SSB, 96 kS/s
gives margin. The supply modulator must have at least that bandwidth.

These timings and register sequences are verified against the simulated DMA engine of the tests; confirm them on
the target board (scope on GPIO4 and the DAC output, SDR spectrum) before transmitting on air.

[`rpitx_autotune.py`](#rpitx_autotunepy) runs these calibrations, the delay search and the choice of quantiser,
oversampling, scale and silence settings automatically in a closed loop with a receiver, and verifies the result on
every modulation mode.

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

Each I/Q sample becomes one **ring slot** (or `slots_per_sample` slots of the interpolated signal): three 32-bit
words in VideoCore memory (fractional PLL multiplier, pad drive word, `GPFSEL0` word, plus a `GPFSEL2` word with
`rf_dac` and an envelope word with an envelope DAC) and four or five DMA control blocks that copy them to the
peripherals. The last control block writes a word into the PCM transmit FIFO (the PWM FIFO with
`envelope_output=pwm`) and is paced by its DREQ line, so the engine advances exactly one slot per slot period; the
word is a dummy value, or the envelope code with an envelope DAC. The last control block points back to the first: the DMA plays the ring
forever without CPU intervention, and the feeder thread only has to stay ahead of it.

### Frequency synthesis: modulating PLLC

The carrier is GPCLK0 driven by **PLLC**. When a stream is created, the driver enables librpitx "advanced PLL
mode":

1. The clock-tree registers about to change are saved (`CORECLK_CNTL/DIV`, `EMMCCLK_CNTL/DIV`, `CM_GP0CTL/DIV`,
   `CM_PLLC`, `PLLC_CTRL/FRAC`, `PLLC_CORE0/1/2`, `PLLC_PER`, `A2W_PLLC_ANA0..3`; on BCM2711 also `EMMC2CLK_CNTL/DIV`)
   and recorded in the session file.
2. The VPU core clock is moved to PLLA (divider 4) and the EMMC clock to PLLD, so that nothing else depends on PLLC.
   On BCM2711 the EMMC2 generator (SD card) is moved to PLLD_PER as well when it is sourced from PLLC_PER, with the
   divider recomputed to keep its frequency. Generators still fed by PLLC afterwards (for example through
   `PLLC_CORE1/2`) are listed in a warning.
3. `PLLC_CORE0` is disabled, the PLLC peripheral channel is enabled with divider 1 (2 on BCM2711) and GPCLK0 is
   sourced from it, MASH 0 (integer divider, no noise shaping spurs).

For a carrier F (the PLL frequency F/harmonic), `ComputeBestLO()` chooses the GPCLK0 integer divider D: it scans
D from 4095 down to 2 for a VCO rate F·D between 200 MHz and 1.5 GHz such that the whole band F ± B (B = 0.58 × slot
rate, the largest instantaneous offset including the phase corrections) maps to the same **integer** PLL
multiplier. Above a 750 MHz carrier (375 MHz on BCM2711) the PLL pre-divider is used
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
(first-order error feedback, at most 0.5 rad applied per slot, the rest kept for the following slots), so the
carrier phase tracks the I/Q phase without drift over long PSK transmissions. AM/PM corrections (`ampm_lut`,
`eer_ampm_lut`) are added to the same carried phase.

The amplitude used for a slot is the envelope delayed by `envelope_delay` (linear interpolation of the recent
magnitudes). With an envelope DAC, the latency of its FIFO is compensated by writing the envelope words of a sample
into an earlier slot; the slots of the last samples of a burst then keep the idle envelope.

The three words of a data slot:

| Word | Value |
|------|-------|
| `PLLC_FRAC` | `0x5A000000 \| frac(f / harmonic + carry)` |
| `PADS_GPIO_0` | `0x5A000000 \| HYST(original) \| SLEW \| level` — envelope mode: the level of the amplitude state chosen for `a·iq_scale` (nearest or sigma-delta; without tables `level = round(min(a·iq_scale, 1)·(drive+1)) − 1`), original pad value when the state is "off"; constant mode: `level = drive`; EER: `eer_drive` |
| `GPFSEL0` | GPIO 0–9 functions sampled at activation, with GPIO4 = ALT0 (carrier on) or input (carrier off) |
| `GPFSEL2` (`rf_dac`) | GPIO 20–29 functions sampled at activation, with GPIO20 = ALT5 or input |
| envelope (`pcm` / `pwm`) | `code << 16 \| code` with `code = round(32767·c)` (PCM), or `round(2^pwm_bits·c)` (PWM), `c` = `eer_lut⁻¹(a·iq_scale)·eer_gain + eer_offset` limited to [0, 1] |

An **idle slot** holds the fractional word of the centre frequency, the original pad value and GPIO4 = input: the
GPCLK keeps running internally but nothing leaves the pin.

### Sample-rate pacing: the PCM block

The PCM/I2S controller is configured as a one-channel, 8-bit transmitter whose frame length (`PCM_MODE.FLEN`) is
the smallest value ≥ 10 that keeps the PCM clock divider between 2 and 4095 (with `envelope_output=pcm`: a 16-bit
stereo I2S master with 64 bit clocks per frame and packed FIFO words). The PCM clock comes from PLLD with MASH 1
and a 12-bit fractional divider, corrected by the same ppm as the carrier. The TX FIFO DREQ and panic thresholds
are 16 words; every frame consumes one word, so the pacing write of each slot blocks while the FIFO holds 16 words
— one slot per frame. With `envelope_output=pwm` PWM channel 1 plays the same role (mark/space, FIFO from DMA, DREQ
level 8, clock from PLLD at slot rate × 2^`pwm_bits`).

Every DMA start (activation, retune, restart after a DMA error) begins with idle slots: the engine runs through
them until the pacing FIFO reaches its DREQ level, so no data slot is ever played faster than the slot rate. The
FIFO is cleared and the transmitter restarted at each start, and stopped when the DMA stops, which silences an
envelope DAC. The produced rate (`sample_rate` sensor) differs from the nominal one by the divider quantisation,
below 1 ppm.

### DMA engine and memory

- **Memory:** one contiguous VideoCore allocation (`mem_alloc` with `MEM_FLAG_DIRECT` on BCM2836+, the uncached
  `0xC` alias, or `MEM_FLAG_L1_NONALLOCATING` on BCM2835; `HINT_PERMALOCK`), locked to obtain its bus address and
  mapped into the process through `/dev/mem`. Control blocks first (32-byte aligned), then the slot words:
  `ring × (4·32 + 3·4)` = 140 bytes per slot (up to `5·32 + 5·4` = 180 bytes with `rf_dac` and an envelope DAC). Every mailbox reply is checked (ioctl result, response code, tag
  response bit).
- **Control blocks:** `TI = NO_WIDE_BURSTS | WAIT_RESP`, plus `D_DREQ | PERMAP(2)` (PCM TX) or `PERMAP(5)` (PWM) for
  the pacing block; one 4-byte transfer each, destination the peripheral bus address (`0x7E102220` PLLC_FRAC,
  `0x7E10002C` PADS_GPIO_0, `0x7E200000` GPFSEL0, `0x7E200008` GPFSEL2, `0x7E203004` PCM FIFO, `0x7E20C018` PWM
  FIFO).
- **Channel:** `CS = PRIORITY(7) | PANIC_PRIORITY(7) | DISDEBUG | ACTIVE`. Only legacy channels are used (0–14,
  0–10 on BCM2711 where 11–14 are DMA4 channels with another control-block format). The kernel's channels
  (`dma-channel-mask` or `brcm,dma-channel-mask` of every `soc/dma@…` and `soc/dma-controller@…` node) are never
  touched; the default is the first channel of 7, 8, 9, 10, 6, 5 (BCM2711) or 14 … 5 that is neither the kernel's
  nor active. When no mask can be read, the channel is not guessed: `dma_channel` must be given. An explicit
  channel that is active is only reset with `dma_force=true`.

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
- When the FIFO is empty and the fill drops below `low = max(ring/8, rate/200, envelope look-ahead + guard + 16)`,
  it pads with idle slots; if a
  burst was in progress without `END_BURST`, an **underflow** is counted.
- If the DMA position is invalid or the channel is no longer `ACTIVE` for 5 consecutive polls, the engine is
  considered stopped: the event is logged with `DMA_CS` / `DMA_DEBUG`, `dma_errors` is incremented and the stream
  restarts with the unplayed samples.

**Start-up and pre-roll:** after activation the feeder fills the ring while the DMA is stopped. The DMA starts at
slot 0 when `preroll_ms` of signal is queued, when the burst end is signalled, when the FIFO is full, or when the
first queued sample has waited `preroll_ms` (so short bursts still go out).

**Retune:** the control thread posts the request with a sequence number and waits up to 3 s for the feeder to
take it (then until it completes). The feeder pauses the engine and records its exact position (an engine that
stopped by itself is not credited with laps), stops the DMA, **switches GPIO4 to input and restores the pads**
before touching the PLL, copies the unplayed data slots of the ring (the original I/Q samples are kept in a shadow
array, and every slot is marked as data or idle) into a replay buffer, reprograms PLLC and the PCM clock, resets
the ring and resumes: the replay buffer is sent before the FIFO. Idle slots before the first data slot are dropped;
idle slots between data slots are replayed as idle. After a DMA error without a valid engine position, up to
5 feeder periods of samples may be sent twice. A request that the PLL, the pacing clock or the envelope DAC cannot
satisfy is refused before anything is reprogrammed and leaves the previous configuration (including the crystal
corrections) in effect.

**Drain detection:** the feeder evaluates the drain condition in one place at the end of every period (every
sample pushed so far consumed from the FIFO, replay buffer empty, last data slot completed) and publishes the FIFO
push count it applies to; `WaitDrained()` compares it with the current push count. A retune clears it before the
ring is rebuilt, so a burst is never reported as sent while samples are in flight between the FIFO, the replay
buffer and the ring.

### The FIFO

`SpscFifo` is a bounded lock-free single-producer/single-consumer queue with 64-bit head and tail counters
(acquire/release), so the fill level is always `head − tail`. Its storage is allocated for `fifo_ms` at the
maximum sample rate; the usable capacity is `max(rate·fifo_ms/1000, 1024)` and is updated on every retune. A
blocked producer waits on a condition variable in 2 ms slices, woken by the feeder when it frees space.

### Shutdown, crash recovery and safety

- **Normal close** (`deactivateStream` / `closeStream` / device destruction): the feeder is joined, the DMA reset,
  GPIO4 (and GPIO20) returned to their original function (input if it was the clock output), the pads restored, the
  envelope DAC pins returned to their original function, the PCM block disabled and its clock restored, the PWM
  block restored, the clock tree restored (PLLC first, waiting for lock, then its channels, the
  VPU and EMMC clocks and GPCLK0), the VideoCore memory freed and the lock file cleared. The record stops naming
  the DMA channel and the VideoCore memory before they are released, so a crash in between can leak the block
  (recovered by a reboot) but never makes the next process free memory that another VideoCore client may own.
- **`exit()`:** an `atexit` handler performs an emergency stop (DMA reset, carrier off, GPCLK0 stopped) without
  invalidating the objects, so a later destructor still completes the shutdown.
- **Signals** (`signal_cleanup=true`): handlers for SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGSEGV, SIGBUS, SIGFPE, SIGILL
  and SIGABRT. When the previous disposition was the default action (the process is about to die) they perform
  async-signal-safe register writes (DMA reset, GPIO4 / GPIO20 input, pads, GPCLK0 off, envelope DAC stopped), restore the previous action and
  re-raise; ignored signals and application handlers are passed through untouched.

#### Crash recovery

While a transmitter exists, `/run/soapy_rpitx/soapy_rpitx.lock` (tmpfs, never survives a reboot; private
directory) is held with an exclusive `flock()` and contains a `key=value` record of everything the process owns:
PID, DMA channel and VideoCore memory handle, bus address and size (written as soon as the memory is allocated,
cleared before it is released, together with whether the kernel DMA channel mask was readable), original pad value and
functions of GPIO4 and of the other pins used, original PCM clock or PWM state, and the clock tree (saved and
recorded before it is first changed). The record ends with an `end=1` line: anything after it (left by an update
interrupted between writing a shorter record and cutting the file) is ignored. The
kernel releases the lock when the process dies, even with `kill -9`. The next process that acquires the lock and
finds a non-empty record knows the owner died without cleaning up and recovers: DMA channel reset (only when the
channel is idle or its current control block lies inside the recorded block; if another program has taken the channel
meanwhile, it is left alone and the report says "now used by another program; not reset"; a record without the
block location, from an older driver, uses the bus address given by the mailbox), GPIO4 and pads
restored, GPCLK0 stopped, PCM disabled and its clock restored, PWM restored, clock tree restored, and the dead
process's GPU memory released (VideoCore handles are global to the firmware). A record is first checked: a DMA
channel that is not a legacy channel of the SoC or that belongs to the kernel, pins outside GPIO 0–27 or invalid
clock values make it **discarded** (logged, nothing applied). When the kernel DMA mask cannot be read, the channel
is accepted only if the record says that the mask was unreadable when the channel was allocated as well. A record
without the end marker (interrupted write, or a record of an older driver version) is recovered, but its VideoCore
memory handle is not freed. `rpitx_reset` does the same on demand. A process
created with `fork()` without `exec()` shares the lock: while such a child lives, the next process is told that the
lock is held by a child of the terminated process.

#### Conflict checks at open

- another process holding the lock → "the transmitter is in use by process N";
- GPIO4 (and GPIO20, the PCM pins 18/19/21 or the PWM pin when used) claimed by any consumer on the SoC GPIO chip
  (`pinctrl-bcm2…`, via the GPIO character-device line info, v2 request with a v1 fallback; a warning is logged
  when neither answers);
- `soc/i2s@7e203000` enabled in the device tree (unless `envelope_output=pwm`); with `envelope_output=pwm`,
  `soc/pwm@7e20c000` or the analogue audio node enabled (a node without a `status` property counts as enabled);
- the lock directory or file not private to the user running the driver;
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
| "the PWM block is enabled" | `envelope_output=pwm`: set `dtparam=audio=off`, remove `pwm` overlays and reboot |
| "the kernel DMA channel mask cannot be read" | Set `dma_channel` to a channel outside the kernel mask (`xxd /proc/device-tree/soc/dma*/*dma-channel-mask`) |
| "the lock directory … must be … mode 0700" | `sudo rm -rf /run/soapy_rpitx` (it is recreated) |
| "in use by a child process of terminated process N" | A forked child kept the lock; end it |
| `envelope_clip` grows | The envelope exceeds full scale: lower `iq_scale` or the application level |
| `envelope_late` grows | The ring ran nearly empty during a burst: raise `preroll_ms` / `fifo_ms` |
| "VideoCore memory allocation failed" | Lower `ring_size`, raise `gpu_mem`, or run `sudo rpitx_reset` to free memory left by a crash |
| `underflows` grows | The application is too slow: raise `preroll_ms` and `fifo_ms`, write larger blocks |
| `late` grows | The feeder was delayed: keep `rt_priority` > 0, pin it (`cpu=3`), consider `isolcpus=3`, the `performance` governor |
| `dma_errors` grows | Something else reset or used the DMA channel; choose another channel |
| "crosses a PLL integer step" | Deviations beyond the step are clipped: move the frequency slightly or lower the sample rate |
| "PLLC did not report lock" | Very high frequencies may not lock on every chip; lower the frequency or use `harmonic` |
| Serial console / I2C / SPI garbled while transmitting | Expected: the VPU clock is on PLLA while the stream is open (`gpu_freq=250` keeps it predictable) |
| Carrier stuck after a crash | `sudo rpitx_reset` (or open any stream): the session record is recovered |
| "the PLLA peripheral channel is not running" | The firmware left PLLA off, so the VPU clock cannot leave PLLC; the board cannot transmit with this driver |
| "the transmitter thread did not respond within 3 s" | The feeder thread was starved (another `SCHED_FIFO` task?); the change was not applied, retry it |
| "not a regular file" / "writable by group or others" / "owned by another user" for a profile | Use a regular file with mode 0644 (or stricter), owned by root or by the user running the driver |
| "a directory of the path is owned by another user" / "writable by others" / "symbolic link" for a profile | Every directory of the path must be owned by root or by that user, must not be writable by others (unless sticky) and must not be a symbolic link: pass the real path |
| No carrier in SSB with the HPSDR bridge | Silent microphone; see the bridge notes above |

---

## Testing

```sh
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

The tests run on any Linux host; `tests/mock_hw.cpp` replaces the mailbox, `/dev/mem`, the device-tree queries and
the DMA engine (a thread that interprets the control-block chain, writes the simulated registers, and models the
PCM / PWM pacing FIFO with its DREQ level, recording per frame the registers in effect and the FIFO word output).

| Test | Covers |
|------|--------|
| `rpitx_unit_tests` | FIFO ordering under concurrency and capacity limit; ring accounting against a simulated engine (steady state, short stalls, lost laps, double stall, host starvation, stopped engine without lap credit); DSP frequency estimation, gating and phase continuity through zero; amplitude model, tables, sigma-delta averages, envelope delay and envelope/phase alignment, EER code mapping |
| `rpitx_devicetree_tests` | Peripheral base, legacy DMA channel limit and kernel DMA mask with old and current device-tree naming |
| `rpitx_transmitter_tests` | Feeder, pre-roll, drain, retune with the carrier off and ring replay, emergency stop then destruction, DMA restart, pad hysteresis, FIFO capacity vs rate, constructor failures (unreachable frequency, refused allocation, short ring), DMA channel selection (and no guess without the kernel mask), session lock and crash recovery (pins, PWM, unreadable kernel mask, record end marker, incomplete record), private lock directory, symbolic link and implausible records, early allocation record, device-tree nodes without `status`, signal handlers, paced DMA start, envelope delay, sigma-delta, AM/PM correction, RF-DAC, slots per sample, PCM and PWM envelope output with envelope/phase alignment, burst end before a short burst, replay of data slots only (retune before the DMA start and after an idle gap), drain during a concurrent retune, release recorded before the VideoCore memory is freed |
| `rpitx_driver_tests` | SoapySDR device: settings and ranges, strict number parsing, amplitude settings, frequency correction, stream arguments (including the envelope output), CF32/CS16/CS8 scaling, `END_BURST` drain, `closeStream()` during a drain, `activateStream()` on an active stream, stream status, sensors, profile files and `profile=`, profile file checks (devices, pipes, size, permissions, symbolic links), out-of-range integers and gains, timeout limits |

CI (`.github/workflows/ci.yml`) runs the tests in a plain build, with AddressSanitizer + UndefinedBehaviorSanitizer
and with ThreadSanitizer (`tests/tsan.supp` suppresses only the simulated-register accesses), and checks
`clang-format`, `shellcheck` on `rpi-cross.sh` and `pyflakes` on the Python tools.

`rpitx_autotune.py self-test` checks the analysis and the optimiser of the tuning tool on a simulated transmitter
and receiver (no radio, no root): recovery of known frequency, clock, distortion and error rates, convergence on
both paths, `HARDWARE_LIMIT` and `UNVERIFIABLE` endings, the budget limits and `--resume`. `--quick` is the short
variant.

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
  SoapyRPITX_Amplitude.hpp       amplitude states, sigma-delta, envelope delay, EER mapping (hardware independent)
  SoapyRPITX_Profile.hpp         profile file parser for profile=<file> (hardware independent)
  SoapyRPITX_Session.*           lock file, crash record and recovery, conflict checks
librpitx/src/                    vendored librpitx (dma, gpio/clock/PCM/PWM/pads, mailbox, dsp, device tree)
tools/
  rpitx_reset.cpp                recovery tool
  rpitx_tone_test.py             test transmitter (tone / WAV)
  rpitx_amp_calibrate.py         amplitude, AM/PM, RF-DAC, EER and envelope-delay calibration
  rpitx_autotune.py              closed-loop over-the-air test and tuning of every mode (bare pin and EER)
  hpsdr_rpitx_bridge.py          openHPSDR Protocol 1 radio emulation
tests/                           unit, device-tree, transmitter and driver tests, hardware mock, TSan suppressions
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
