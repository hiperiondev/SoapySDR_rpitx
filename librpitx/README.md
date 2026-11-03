**librpitx** Radio frequency transmitter library for Raspberry Pi (B, B+, PI2, PI3, PI4 and PI zero)

It is now the base of *rpitx* project, separate from application

_Copyright (c) Evariste Courjaud F5OEO. Code is GPL_V3

# Upstream installation

```sh
git clone https://github.com/F5OEO/librpitx
cd librpitx/src
make
```

# Copy vendored in SoapySDR_rpitx

This directory is a **modified copy** of upstream librpitx, reduced to the parts used by the SoapySDR driver
(`dma`, `dsp`, `gpio`, `mailbox`, `rpi`, `util`). It is compiled into the SoapySDR module by the top-level
`CMakeLists.txt` with hidden symbol visibility; installing upstream librpitx separately is not needed, and the
module does not link against it. The upstream revision the copy was taken from was not recorded.

Differences from upstream:

| File | Difference |
|------|------------|
| `mailbox.cpp` / `mailbox.hpp` | Every property request checks the ioctl result, the firmware response code and the tag response bit; `mem_alloc()` / `mem_lock()` return 0 and `mem_free()` / `mem_unlock()` a non-zero status on failure. `mbox_open()` only opens `/dev/vcio` (read/write, close-on-exec): no device node is created. `mapmem()` returns `NULL` and `unmapmem()` only logs on failure (the library never calls `exit()`); both use the system page size and map the page offset of unaligned bases. `VC_PAGE_SIZE` is the VideoCore allocation unit. The unused QPU / code-execution / clock-query helpers were removed. |
| `rpi.cpp` / `rpi.hpp` | Return 0 when the device tree cannot be read (no silent BCM2835 fallback). `bcm_host_get_kernel_dma_mask()` reads the `brcm,dma-channel-mask` of the DMA controllers. `RPITX_DT_ROOT` can be overridden at build time. |
| `gpio.cpp` / `gpio.hpp` | `gpio::gpio()` throws `std::runtime_error` when the registers cannot be mapped or the peripheral base is unknown. Clock generators are programmed with the stop / wait-BUSY / configure / start sequence (`cm_program_generator()`, `cm_stop_generator()`). `cm_pll_frequency()` computes every PLL with masked dividers and returns 0 instead of dividing by zero. Advanced (master PLL) mode saves the clock tree (`ClockTreeState`, `cm_save_tree()`) and `~clkgpio()` restores it (`cm_restore_tree()`). `clkgpio` members are initialised; the constructor no longer changes the pads. `SetCenterFrequency()` returns -1 without reprogramming anything when no PLL divider fits, keeps the GPCLK0 generator stopped while PLLC is reprogrammed, doubles the bandwidth with the frequency on BCM2711, checks the integer-step crossing on the pre-divider path too, and reports it through `CrossesBoundary()`. `GetMasterFrac()` saturates at the integer multiplier boundary instead of wrapping; `GetMasterFracFrequency()` is its inverse. `WaitPllcLock()` and `IsPllcLocked()` added. The ppm sign is documented (positive when the crystal runs slow, as the NTP estimate) and `GetPllFrequency()` no longer overwrites the correction with the NTP value. `pcmgpio` maps the clock manager directly instead of owning a second `clkgpio`, applies the crystal correction, reports the produced frame rate, and its destructor disables the PCM block and restores the PCM clock. `padgpio::setlevel()` keeps hysteresis and slew; `PADS_GPIO_LEN` covers the whole pad block. The clock tree dump goes to stderr at debug level 2. The PWM block, `SetPhase()`, `enableclk()` / `disableclk()`, `setpulloff()`, `GetRealFrequency()`, `GetFrequencyResolution()` and `SetPLLMasterLoop()` were removed (unused). |
| `gpio.hpp` | `DMA_CS_PRIORITY()` / `DMA_CS_PANIC_PRIORITY()` shift the masked value, so the DMA channel really gets the requested AXI priority. `DMA_CS_ERROR`, `DMA_CHANNEL_WORDS`, clock-manager and pad bit definitions added. |
| `dma.cpp` / `dma.hpp` | Only legacy channels are used (0-10 on BCM2711, 0-14 otherwise); channels owned by the kernel are refused, an active channel is only reset when forced, and the default is the first free channel of a preference list. Allocation failures throw `std::runtime_error`; the destructor unmaps the memory, unlocks and frees it (failures are logged) and closes the mailbox. `getcbposition()` validates the address and computes the offset without truncating 64-bit pointers. `GetStatus()`, `GetDebugStatus()`, `GetChannel()` and `GetMemoryHandle()` added; the unused `bufferdma` cursor API (`GetBufferAvailable()`, `GetUserMemIndex()`, `PushSample()`, `ResetBuffer()`), `isunderflow()` and `mem_phys_to_virt()` were removed. |
| `dsp.cpp` / `dsp.hpp` | The instantaneous frequency is the wrapped phase difference (no unbounded phase accumulator). Samples below `magnitude_gate` report a zero frequency and keep the last valid phase, so the next valid sample applies the whole phase change (phase reversals through zero are preserved). `reset()` added; unused angle helpers removed. |
| `util.hpp` | Documented; `dbg_printf()` is checked as a printf format. |
| `librpitx.hpp`, `rpi.hpp`, `mailbox.hpp` | Include guards; `librpitx.hpp` only includes the vendored headers. |
| `raspberry_pi_revision.*` | Removed (not used). |
