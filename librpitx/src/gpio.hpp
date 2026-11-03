/**
 * @file gpio.hpp
 * @brief Register-level access to the BCM283x/BCM2711 peripherals used by librpitx (DMA, GPIO, clock
 *        manager, PCM and pad control).
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

#ifndef DEF_GPIO
#define DEF_GPIO
#include <stdint.h>
#include <cstdio>
#include <string>
#include <vector>

/** @brief Bus address of the peripheral window as seen by the DMA engine. */
#define PHYSICAL_BUS 0x7E000000

/**
 * @brief Memory mapping of one peripheral register block through /dev/mem.
 *
 * The constructor detects the SoC from the device tree: on BCM2711 (Raspberry Pi 4 / 400 / CM4)
 * @ref pi_is_2711 is set and the crystal frequency is 54 MHz, otherwise it is 19.2 MHz.
 */
class gpio {
public:
    bool pi_is_2711 = false;            ///< True on BCM2711.
    uint64_t XOSC_FREQUENCY = 19200000; ///< Crystal oscillator frequency in Hz.

public:
    volatile uint32_t *gpioreg = NULL; ///< Mapped registers (word addressed).
    uint32_t gpiolen;                  ///< Size of the mapping in bytes.

    /**
     * @brief Maps @p len bytes of peripheral registers at offset @p base from the peripheral base.
     * @param base Offset of the register block from the peripheral base.
     * @param len  Size of the block in bytes.
     * @throws std::runtime_error when the peripheral base is unknown or /dev/mem cannot be mapped
     *         (typically because the process is not root).
     */
    gpio(uint32_t base, uint32_t len);

    /** @brief Unmaps the registers. */
    ~gpio();

    gpio(const gpio &) = delete;
    gpio &operator=(const gpio &) = delete;

    /**
     * @brief Returns the ARM physical address of the peripherals and updates @ref pi_is_2711 and
     *        @ref XOSC_FREQUENCY.
     * @return Peripheral base address (0x20000000, 0x3F000000 or 0xFE000000).
     * @throws std::runtime_error when the device tree does not report one of these addresses.
     */
    uint32_t GetPeripheralBase();
};

/**
 * @name DMA controller
 * @{
 */
#define DMA_BASE (0x00007000) ///< Offset of the DMA controller.
#define DMA_LEN 0xF00         ///< Size of the DMA register block (channels 0-14).

#define BCM2708_DMA_SRC_IGNOR (1 << 11)      ///< TI: do not read the source.
#define BCM2708_DMA_SRC_INC (1 << 8)         ///< TI: increment the source address.
#define BCM2708_DMA_DST_IGNOR (1 << 7)       ///< TI: do not write the destination.
#define BCM2708_DMA_NO_WIDE_BURSTS (1 << 26) ///< TI: no wide bursts.
#define BCM2708_DMA_WAIT_RESP (1 << 3)       ///< TI: wait for the write response.
#define BCM2708_DMA_SET_INT (1 << 0)         ///< TI: raise an interrupt when the block completes.

#define BCM2708_DMA_D_DREQ (1 << 6)        ///< TI: pace writes with the destination DREQ.
#define BCM2708_DMA_PER_MAP(x) ((x) << 16) ///< TI: peripheral DREQ number.
#define BCM2708_DMA_END (1 << 1)           ///< CS: transfer complete flag.
#define BCM2708_DMA_RESET (1u << 31)       ///< CS: reset the channel.
#define BCM2708_DMA_ABORT (1 << 30)        ///< CS: abort the current control block.
#define BCM2708_DMA_INT (1 << 2)           ///< CS: interrupt status flag.

#define DMA_CS (0x00 / 4)        ///< Control and status register (word index).
#define DMA_CONBLK_AD (0x04 / 4) ///< Control block address register (word index).
#define DMA_DEBUG (0x20 / 4)     ///< Debug register (word index).
#define DMA_CHANNEL_WORDS 0x40   ///< Distance between two channels, in register words.

#define DMA_CS_RESET (1u << 31)                      ///< CS: reset.
#define DMA_CS_ABORT (1 << 30)                       ///< CS: abort.
#define DMA_CS_DISDEBUG (1 << 29)                    ///< CS: ignore the debug pause signal.
#define DMA_CS_WAIT_FOR_OUTSTANDING_WRITES (1 << 28) ///< CS: wait for outstanding writes.
#define DMA_CS_ERROR (1 << 8)                        ///< CS: the channel stopped on an error (details in DEBUG).
#define DMA_CS_INT (1 << 2)                          ///< CS: interrupt status.
#define DMA_CS_END (1 << 1)                          ///< CS: end flag.
#define DMA_CS_ACTIVE (1 << 0)                       ///< CS: channel active.
#define DMA_CS_PRIORITY(x) (((x) & 0xf) << 16)       ///< CS: AXI priority level (0-15).
#define DMA_CS_PANIC_PRIORITY(x) (((x) & 0xf) << 20) ///< CS: AXI panic priority level (0-15).
/** @} */

/** @brief Mapping of the DMA controller registers. */
class dmagpio : public gpio {
public:
    /** @brief Maps the DMA controller. */
    dmagpio();
};

/**
 * @name General-purpose I/O
 * @{
 */
#define GENERAL_BASE (0x00200000) ///< Offset of the GPIO block.
#define GENERAL_LEN 0xD0          ///< Size of the GPIO register block.

#define GPFSEL0 (0x00 / 4) ///< Function select GPIO 0-9.
#define GPFSEL1 (0x04 / 4) ///< Function select GPIO 10-19.
#define GPFSEL2 (0x08 / 4) ///< Function select GPIO 20-29.
/** @} */

/** @brief GPIO function-select codes. */
enum {
    fsel_input,  ///< Input.
    fsel_output, ///< Output.
    fsel_alt5,   ///< Alternate function 5.
    fsel_alt4,   ///< Alternate function 4.
    fsel_alt0,   ///< Alternate function 0.
    fsel_alt1,   ///< Alternate function 1.
    fsel_alt2,   ///< Alternate function 2.
    fsel_alt3    ///< Alternate function 3.
};

/** @brief GPIO function selection. */
class generalgpio : public gpio {
public:
    /** @brief Maps the GPIO block. */
    generalgpio();

    /** @brief Releases the mapping. */
    ~generalgpio();

    /**
     * @brief Sets the function of a GPIO (read-modify-write of its GPFSEL register).
     * @param pin  GPIO number (0-53).
     * @param mode Function-select code (fsel_*).
     * @return Always 0.
     */
    int setmode(uint32_t pin, uint32_t mode);
};

/**
 * @name Clock manager and PLLs
 * Register offsets from https://github.com/raspberrypi/linux/blob/rpi-4.9.y/drivers/clk/bcm/clk-bcm2835.c
 * (see also https://github.com/raspberrypi/linux/commit/76527b4e6a5dbe55e0b2d8ab533c2388b36c86be for the
 * jitter discussion).
 * @{
 */
#define CLK_BASE (0x00101000) ///< Offset of the clock manager.

#define CM_PASSWD (0x5Au << 24) ///< Password required by every clock-manager write.
#define CM_SRC_MASK 0xF         ///< Clock generator CTL: source field.
#define CM_ENAB (1u << 4)       ///< Clock generator CTL: enable.
#define CM_KILL (1u << 5)       ///< Clock generator CTL: stop and reset the generator immediately.
#define CM_BUSY (1u << 7)       ///< Clock generator CTL: generator running.
#define CM_MASH_SHIFT 9         ///< Clock generator CTL: MASH field shift.
#define CLK_LEN 0x1700          ///< Size of the clock manager block (covers PLLH_STS at 0x1660 and PLLB_* up to 0x16E0).

#define CORECLK_CNTL (0x08 / 4)  ///< Core (VPU) clock control.
#define CORECLK_DIV (0x0c / 4)   ///< Core (VPU) clock divider.
#define GPCLK_CNTL (0x70 / 4)    ///< GPCLK0 control.
#define GPCLK_DIV (0x74 / 4)     ///< GPCLK0 divider.
#define GPCLK_CNTL_2 (0x80 / 4)  ///< GPCLK2 control.
#define GPCLK_DIV_2 (0x84 / 4)   ///< GPCLK2 divider.
#define EMMCCLK_CNTL (0x1C0 / 4) ///< EMMC clock control.
#define EMMCCLK_DIV (0x1C4 / 4)  ///< EMMC clock divider.
#define CM_EMMC2CTL (0x1d0 / 4)  ///< EMMC2 clock control (BCM2711).
#define CM_EMMC2DIV (0x1d4 / 4)  ///< EMMC2 clock divider (BCM2711).
#define EMMC2CLK_CNTL CM_EMMC2CTL ///< EMMC2 clock control (BCM2711); drives the SD card.
#define EMMC2CLK_DIV CM_EMMC2DIV  ///< EMMC2 clock divider (BCM2711).

#define CM_VPUCTL 0x008       ///< VPU clock control (byte offset).
#define CM_VPUDIV 0x00c       ///< VPU clock divider (byte offset).
#define CM_SYSCTL 0x010       ///< System clock control (byte offset).
#define CM_SYSDIV 0x014       ///< System clock divider (byte offset).
#define CM_PERIACTL 0x018     ///< PERIA clock control (byte offset).
#define CM_PERIADIV 0x01c     ///< PERIA clock divider (byte offset).
#define CM_PERIICTL 0x020     ///< PERII clock control (byte offset).
#define CM_PERIIDIV 0x024     ///< PERII clock divider (byte offset).
#define CM_H264CTL 0x028      ///< H264 clock control (byte offset).
#define CM_H264DIV 0x02c      ///< H264 clock divider (byte offset).
#define CM_ISPCTL 0x030       ///< ISP clock control (byte offset).
#define CM_ISPDIV 0x034       ///< ISP clock divider (byte offset).
#define CM_V3DCTL 0x038       ///< V3D clock control (byte offset).
#define CM_V3DDIV 0x03c       ///< V3D clock divider (byte offset).
#define CM_CAM0CTL 0x040      ///< CAM0 clock control (byte offset).
#define CM_CAM0DIV 0x044      ///< CAM0 clock divider (byte offset).
#define CM_CAM1CTL 0x048      ///< CAM1 clock control (byte offset).
#define CM_CAM1DIV 0x04c      ///< CAM1 clock divider (byte offset).
#define CM_CCP2CTL 0x050      ///< CCP2 clock control (byte offset).
#define CM_CCP2DIV 0x054      ///< CCP2 clock divider (byte offset).
#define CM_DSI0ECTL 0x058     ///< DSI0E clock control (byte offset).
#define CM_DSI0EDIV 0x05c     ///< DSI0E clock divider (byte offset).
#define CM_DSI0PCTL 0x060     ///< DSI0P clock control (byte offset).
#define CM_DSI0PDIV 0x064     ///< DSI0P clock divider (byte offset).
#define CM_DPICTL 0x068       ///< DPI clock control (byte offset).
#define CM_DPIDIV 0x06c       ///< DPI clock divider (byte offset).
#define CM_GP0CTL 0x070       ///< GPCLK0 control (byte offset).
#define CM_GP0DIV 0x074       ///< GPCLK0 divider (byte offset).
#define CM_GP1CTL 0x078       ///< GPCLK1 control (byte offset).
#define CM_GP1DIV 0x07c       ///< GPCLK1 divider (byte offset).
#define CM_GP2CTL 0x080       ///< GPCLK2 control (byte offset).
#define CM_GP2DIV 0x084       ///< GPCLK2 divider (byte offset).
#define CM_HSMCTL 0x088       ///< HSM clock control (byte offset).
#define CM_HSMDIV 0x08c       ///< HSM clock divider (byte offset).
#define CM_OTPCTL 0x090       ///< OTP clock control (byte offset).
#define CM_OTPDIV 0x094       ///< OTP clock divider (byte offset).
#define CM_PCMCTL 0x098       ///< PCM clock control (byte offset).
#define CM_PCMDIV 0x09c       ///< PCM clock divider (byte offset).
#define CM_PWMCTL 0x0a0       ///< PWM clock control (byte offset).
#define CM_PWMDIV 0x0a4       ///< PWM clock divider (byte offset).
#define CM_SLIMCTL 0x0a8      ///< SLIMbus clock control (byte offset).
#define CM_SLIMDIV 0x0ac      ///< SLIMbus clock divider (byte offset).
#define CM_SMICTL 0x0b0       ///< SMI clock control (byte offset).
#define CM_SMIDIV 0x0b4       ///< SMI clock divider (byte offset).
#define CM_TCNTCTL 0x0c0      ///< Test counter control (byte offset).
#define CM_TCNT_SRC1_SHIFT 12 ///< Test counter source shift.
#define CM_TCNTCNT 0x0c4      ///< Test counter value (byte offset).
#define CM_TECCTL 0x0c8       ///< TEC clock control (byte offset).
#define CM_TECDIV 0x0cc       ///< TEC clock divider (byte offset).
#define CM_TD0CTL 0x0d0       ///< TD0 clock control (byte offset).
#define CM_TD0DIV 0x0d4       ///< TD0 clock divider (byte offset).
#define CM_TD1CTL 0x0d8       ///< TD1 clock control (byte offset).
#define CM_TD1DIV 0x0dc       ///< TD1 clock divider (byte offset).
#define CM_TSENSCTL 0x0e0     ///< Temperature sensor clock control (byte offset).
#define CM_TSENSDIV 0x0e4     ///< Temperature sensor clock divider (byte offset).
#define CM_TIMERCTL 0x0e8     ///< Timer clock control (byte offset).
#define CM_TIMERDIV 0x0ec     ///< Timer clock divider (byte offset).
#define CM_UARTCTL 0x0f0      ///< UART clock control (byte offset).
#define CM_UARTDIV 0x0f4      ///< UART clock divider (byte offset).
#define CM_VECCTL 0x0f8       ///< VEC clock control (byte offset).
#define CM_VECDIV 0x0fc       ///< VEC clock divider (byte offset).
#define CM_PULSECTL 0x190     ///< Pulse clock control (byte offset).
#define CM_PULSEDIV 0x194     ///< Pulse clock divider (byte offset).
#define CM_SDCCTL 0x1a8       ///< SDRAM controller clock control (byte offset).
#define CM_SDCDIV 0x1ac       ///< SDRAM controller clock divider (byte offset).
#define CM_ARMCTL 0x1b0       ///< ARM clock control (byte offset).
#define CM_AVEOCTL 0x1b8      ///< AVEO clock control (byte offset).
#define CM_AVEODIV 0x1bc      ///< AVEO clock divider (byte offset).
#define CM_EMMCCTL 0x1c0      ///< EMMC clock control (byte offset).
#define CM_EMMCDIV 0x1c4      ///< EMMC clock divider (byte offset).

#define CM_LOCK (0x114 / 4)               ///< PLL lock status (word index).
#define CM_LOCK_FLOCKH (1 << 12)          ///< PLLH locked.
#define CM_LOCK_FLOCKD (1 << 11)          ///< PLLD locked.
#define CM_LOCK_FLOCKC (1 << 10)          ///< PLLC locked.
#define CM_LOCK_FLOCKB (1 << 9)           ///< PLLB locked.
#define CM_LOCK_FLOCKA (1 << 8)           ///< PLLA locked.
#define A2W_PLL_CHANNEL_DISABLE (1u << 8) ///< PLL channel divider register: channel output disabled.

#define CM_PLLA (0x104 / 4) ///< PLLA control (word index).
#define CM_PLLC (0x108 / 4) ///< PLLC control (word index).
#define CM_PLLD (0x10c / 4) ///< PLLD control (word index).
#define CM_PLLH (0x110 / 4) ///< PLLH control (word index).
#define CM_PLLB (0x170 / 4) ///< PLLB control (word index).

#define A2W_PLLA_ANA0 (0x1010 / 4) ///< PLLA analog control 0.
#define A2W_PLLC_ANA0 (0x1030 / 4) ///< PLLC analog control 0.
#define A2W_PLLD_ANA0 (0x1050 / 4) ///< PLLD analog control 0.
#define A2W_PLLH_ANA0 (0x1070 / 4) ///< PLLH analog control 0.
#define A2W_PLLB_ANA0 (0x10f0 / 4) ///< PLLB analog control 0.
#define A2W_PLL_KA_SHIFT 7         ///< Loop filter KA field shift.
#define A2W_PLL_KI_SHIFT 19        ///< Loop filter KI field shift.
#define A2W_PLL_KP_SHIFT 15        ///< Loop filter KP field shift.

#define PLLA_CTRL (0x1100 / 4) ///< PLLA multiplier/pre-divider.
#define PLLA_FRAC (0x1200 / 4) ///< PLLA fractional multiplier.
#define PLLA_DSI0 (0x1300 / 4) ///< PLLA DSI0 channel divider.
#define PLLA_CORE (0x1400 / 4) ///< PLLA core channel divider.
#define PLLA_PER (0x1500 / 4)  ///< PLLA peripheral channel divider.
#define PLLA_CCP2 (0x1600 / 4) ///< PLLA CCP2 channel divider.

#define PLLB_CTRL (0x11e0 / 4) ///< PLLB multiplier/pre-divider.
#define PLLB_FRAC (0x12e0 / 4) ///< PLLB fractional multiplier.
#define PLLB_ARM (0x13e0 / 4)  ///< PLLB ARM channel divider.
#define PLLB_SP0 (0x14e0 / 4)  ///< PLLB SP0 channel divider.
#define PLLB_SP1 (0x15e0 / 4)  ///< PLLB SP1 channel divider.
#define PLLB_SP2 (0x16e0 / 4)  ///< PLLB SP2 channel divider.

#define PLLC_CTRL (0x1120 / 4)  ///< PLLC multiplier/pre-divider.
#define PLLC_FRAC (0x1220 / 4)  ///< PLLC fractional multiplier.
#define PLLC_CORE2 (0x1320 / 4) ///< PLLC core2 channel divider.
#define PLLC_CORE1 (0x1420 / 4) ///< PLLC core1 channel divider.
#define PLLC_PER (0x1520 / 4)   ///< PLLC peripheral channel divider.
#define PLLC_CORE0 (0x1620 / 4) ///< PLLC core0 channel divider.

#define PLLD_CTRL (0x1140 / 4) ///< PLLD multiplier/pre-divider.
#define PLLD_FRAC (0x1240 / 4) ///< PLLD fractional multiplier.
#define PLLD_DSI0 (0x1340 / 4) ///< PLLD DSI0 channel divider.
#define PLLD_CORE (0x1440 / 4) ///< PLLD core channel divider.
#define PLLD_PER (0x1540 / 4)  ///< PLLD peripheral channel divider.
#define PLLD_DSI1 (0x1640 / 4) ///< PLLD DSI1 channel divider.

#define PLLH_CTRL (0x1160 / 4) ///< PLLH multiplier/pre-divider.
#define PLLH_FRAC (0x1260 / 4) ///< PLLH fractional multiplier.
#define PLLH_AUX (0x1360 / 4)  ///< PLLH auxiliary channel divider.
#define PLLH_RCAL (0x1460 / 4) ///< PLLH RCAL channel divider.
#define PLLH_PIX (0x1560 / 4)  ///< PLLH pixel channel divider.
#define PLLH_STS (0x1660 / 4)  ///< PLLH status.

#define XOSC_CTRL (0x1190 / 4) ///< Crystal oscillator control.
/** @} */

/** @brief Clock sources selectable by a clock generator. */
enum {
    clk_gnd,    ///< Ground (clock off).
    clk_osc,    ///< Crystal oscillator.
    clk_debug0, ///< Test/debug 0.
    clk_debug1, ///< Test/debug 1.
    clk_plla,   ///< PLLA peripheral channel.
    clk_pllc,   ///< PLLC peripheral channel.
    clk_plld,   ///< PLLD peripheral channel.
    clk_hdmi    ///< HDMI auxiliary.
};

/**
 * @brief Clock-tree registers saved before librpitx modulates PLLC, so that they can be restored.
 *
 * The values are raw register contents without the password byte.
 */
struct ClockTreeState {
    bool valid = false;                 ///< True when the registers below hold a snapshot.
    uint32_t coreCtl = 0;               ///< CORECLK_CNTL.
    uint32_t coreDiv = 0;               ///< CORECLK_DIV.
    uint32_t emmcCtl = 0;               ///< EMMCCLK_CNTL.
    uint32_t emmcDiv = 0;               ///< EMMCCLK_DIV.
    bool emmc2Valid = false;            ///< True when @ref emmc2Ctl and @ref emmc2Div hold a snapshot (BCM2711 only).
    uint32_t emmc2Ctl = 0;              ///< EMMC2CLK_CNTL (BCM2711; the SD card clock).
    uint32_t emmc2Div = 0;              ///< EMMC2CLK_DIV (BCM2711).
    uint32_t gp0Ctl = 0;                ///< GPCLK_CNTL (GPCLK0).
    uint32_t gp0Div = 0;                ///< GPCLK_DIV (GPCLK0).
    uint32_t cmPllc = 0;                ///< CM_PLLC.
    uint32_t pllcCtrl = 0;              ///< PLLC_CTRL.
    uint32_t pllcFrac = 0;              ///< PLLC_FRAC.
    uint32_t pllcCore0 = 0;             ///< PLLC_CORE0.
    uint32_t pllcCore1 = 0;             ///< PLLC_CORE1.
    uint32_t pllcCore2 = 0;             ///< PLLC_CORE2.
    uint32_t pllcPer = 0;               ///< PLLC_PER.
    uint32_t pllcAna[4] = {0, 0, 0, 0}; ///< A2W_PLLC_ANA0..3.
};

/**
 * @brief Reads the clock-tree registers modified by advanced (master PLL) mode.
 * @param cm      Mapped clock manager (@ref CLK_BASE).
 * @param state   Receives the snapshot (@ref ClockTreeState::valid is set).
 * @param bcm2711 True on BCM2711: the EMMC2 generator is saved as well (@ref ClockTreeState::emmc2Valid is set).
 */
void cm_save_tree(volatile uint32_t *cm, ClockTreeState &state, bool bcm2711 = false);

/**
 * @brief Writes back a snapshot taken by cm_save_tree().
 *
 * GPCLK0 is stopped first, then PLLC is reprogrammed and given up to 10 ms to lock, then its channel
 * dividers, the VPU core clock and the EMMC and EMMC2 clocks are returned to their original source, and GPCLK0 gets
 * its original configuration. Nothing is written when @p state is not valid.
 *
 * @param cm    Mapped clock manager.
 * @param state Snapshot.
 */
void cm_restore_tree(volatile uint32_t *cm, const ClockTreeState &state);

/**
 * @brief Moves the EMMC2 generator (SD card clock, BCM2711) from the PLLC peripheral channel to PLLD.
 *
 * Does nothing when the generator is not sourced from PLLC_PER (source 5). Otherwise the generator is
 * reprogrammed with source 6 (PLLD_PER) and a divider recomputed from the current PLLC_PER and PLLD_PER
 * frequencies so that the output frequency is kept; when either frequency cannot be computed the divider is
 * left as it was. The MASH setting and the enable state are preserved.
 *
 * @param cm      Mapped clock manager.
 * @param xosc    Crystal frequency in Hz.
 * @param bcm2711 True on BCM2711 (the function does nothing otherwise).
 * @return True when the generator was moved.
 */
bool cm_move_emmc2_off_pllc(volatile uint32_t *cm, uint64_t xosc, bool bcm2711);

/**
 * @brief Lists the enabled clock generators of the clock manager that are still sourced from PLLC (source 5).
 *
 * GPCLK0, the generator of librpitx itself, is not listed.
 *
 * @param cm      Mapped clock manager.
 * @param bcm2711 True on BCM2711 (adds the EMMC2 generator to the scan).
 * @return Names of the generators fed by PLLC (empty when none).
 */
std::vector<std::string> cm_pllc_clients(volatile uint32_t *cm, bool bcm2711);

/**
 * @brief Stops a clock generator and waits until it is idle.
 *
 * The generator is disabled, BUSY is polled for up to 1 ms and the generator is killed when it does not
 * stop in time.
 *
 * @param cm  Mapped clock manager.
 * @param ctl Word index of the generator CTL register (its DIV register follows it).
 */
void cm_stop_generator(volatile uint32_t *cm, unsigned ctl);

/**
 * @brief Programs a clock generator with the stop / configure / start sequence required by the hardware.
 * @param cm     Mapped clock manager.
 * @param ctl    Word index of the generator CTL register (its DIV register follows it).
 * @param src    Clock source (clk_*).
 * @param mash   MASH order (0-3).
 * @param divi   Integer divider (12 bits).
 * @param divf   Fractional divider (12 bits).
 * @param enable Start the generator after configuring it.
 */
void cm_program_generator(volatile uint32_t *cm, unsigned ctl, unsigned src, unsigned mash, uint32_t divi, uint32_t divf, bool enable);

/**
 * @brief Computes the output frequency of a clock source from the PLL registers.
 *
 * @param cm      Mapped clock manager.
 * @param pll     Clock source (clk_*).
 * @param xosc    Crystal frequency in Hz.
 * @param bcm2711 True on BCM2711.
 * @param ppm     Crystal correction in ppm (positive when the crystal runs slow, see clkgpio::Setppm()).
 * @return Frequency in Hz, or 0 when the source is unknown or its dividers are not programmed.
 */
uint64_t cm_pll_frequency(volatile uint32_t *cm, int pll, uint64_t xosc, bool bcm2711, double ppm);

/**
 * @brief GPCLK0 clock generator, optionally driven by modulating the PLLC fractional multiplier.
 *
 * In advanced (master PLL) mode the carrier is produced by PLLC divided by a fixed integer
 * (@ref PllFixDivider) and frequency modulation is applied by rewriting only the 20-bit fractional part of
 * the PLLC multiplier. The integer part (@ref MasterMult) is chosen by SetCenterFrequency() so that the
 * requested bandwidth stays within one integer step; GetMasterFrac() saturates instead of wrapping when a
 * frequency falls outside that step.
 *
 * Enabling advanced mode saves the clock tree; the destructor restores it.
 */
class clkgpio : public gpio {
protected:
    int pllnumber = clk_pllc;           ///< Clock source of GPCLK0.
    int Mash = 0;                       ///< MASH noise-shaping order.
    uint64_t Pllfrequency = 0;          ///< Frequency of the selected source in Hz.
    bool ModulateFromMasterPLL = false; ///< True in advanced (master PLL) mode.
    uint64_t CentralFrequency = 0;      ///< Carrier frequency in Hz.
    generalgpio gengpio;                ///< GPIO block used to route GPCLK0 to a pin.
    double clk_ppm = 0;                 ///< Crystal correction in ppm (positive when the crystal runs slow).
    uint32_t MasterMult = 0;            ///< Integer PLLC multiplier programmed for @ref CentralFrequency.
    bool CrossBoundary = false;         ///< The last carrier cannot keep its bandwidth within one integer step.
    ClockTreeState SavedTree;           ///< Clock tree before advanced mode was enabled.

public:
    int PllFixDivider = 8; ///< Fixed divider between PLLC and GPCLK0 in advanced mode (1 means PLL pre-divider).

    /** @brief Maps the clock manager and reads the NTP frequency correction. */
    clkgpio();

    /** @brief Restores the clock tree saved by SetAdvancedPllMode(), or stops GPCLK0 when none was saved. */
    ~clkgpio();

    /**
     * @brief Selects the GPCLK0 source (the generator is left stopped).
     * @param PllNo    Clock source (clk_*); values >= 8 select PLLC.
     * @param MashType MASH order (0-3); larger values select 0.
     * @return Always 0.
     */
    int SetPllNumber(int PllNo, int MashType);

    /**
     * @brief Returns the frequency of a clock source, corrected by @ref clk_ppm.
     * @param PllNo Clock source (clk_*).
     * @return Frequency in Hz (0 when it cannot be computed).
     */
    uint64_t GetPllFrequency(int PllNo);

    /** @brief Prints the clock and PLL registers to stderr (debug level 2). */
    void print_clock_tree(void);

    /**
     * @brief Sets the GPCLK0 output to @ref CentralFrequency + @p Frequency.
     * @param Frequency Offset from the carrier in Hz.
     * @return Always 0.
     */
    int SetFrequency(double Frequency);

    /**
     * @brief Programs the GPCLK0 divider and starts the generator.
     * @param Div  Integer part (2-4095).
     * @param Frac Fractional part (12 bits).
     * @return Always 0.
     */
    int SetClkDivFrac(uint32_t Div, uint32_t Frac);

    /**
     * @brief Enables or disables advanced (master PLL) mode.
     *
     * Enabling it saves the clock tree, moves the VPU clock to PLLA and the EMMC clock to PLLD so that PLLC
     * can be modulated. The VPU clock feeds the mini UART, SPI and I2C controllers, so their rates change
     * while advanced mode is in effect.
     *
     * @param Advanced True to enable.
     * @throws std::runtime_error when enabling it while the PLLA peripheral channel is not running (nothing is
     *         changed).
     */
    void SetAdvancedPllMode(bool Advanced);

    /**
     * @brief Saves the clock tree modified by advanced mode, without changing anything.
     *
     * SetAdvancedPllMode() saves it as well when no snapshot exists; calling this first lets the caller record
     * the snapshot (for crash recovery) before the first clock-manager write.
     */
    void SaveClockTree();

    /** @brief Restores the clock tree saved by SetAdvancedPllMode() (no effect when none was saved). */
    void RestoreClockTree();

    /**
     * @brief Returns the clock tree saved by SetAdvancedPllMode().
     * @return Snapshot (not valid before advanced mode is enabled).
     */
    const ClockTreeState &SavedClockTree() const;

    /**
     * @brief Sets the carrier frequency and the bandwidth that must be reachable around it.
     * @param Frequency Carrier frequency in Hz.
     * @param Bandwidth Half-width in Hz of the band that must not cross an integer PLL multiplier.
     * @return 0 on success, -1 when no PLL configuration can produce @p Frequency.
     */
    int SetCenterFrequency(uint64_t Frequency, int Bandwidth);

    /**
     * @brief Tells whether SetCenterFrequency() would find a PLL configuration (no register is written and no
     *        member is changed, so it can run on any thread).
     * @param Frequency Carrier frequency in Hz.
     * @param Bandwidth Half-width in Hz of the band that must not cross an integer PLL multiplier.
     * @return 0 when a divider keeps the band within one integer step, 1 when the best divider crosses one
     *         (SetCenterFrequency() would then set CrossesBoundary()), -1 when no PLL configuration can produce
     *         @p Frequency.
     */
    int CheckCenterFrequency(uint64_t Frequency, int Bandwidth) const;

    /**
     * @brief Reads the kernel NTP frequency estimate (no state is changed).
     * @param ppm Receives the correction in ppm when the estimate is valid.
     * @return True when the estimate is valid and within +/-200 ppm (the value SetppmFromNTP() would apply).
     */
    static bool ReadNtpPpm(double &ppm);

    /**
     * @brief Tells whether the band requested by the last SetCenterFrequency() crosses an integer multiplier.
     *
     * When true, the part of the band beyond the integer step is clipped by GetMasterFrac().
     *
     * @return True when the modulation can saturate.
     */
    bool CrossesBoundary() const;

    /**
     * @brief Chooses the PLLC divider for a carrier.
     * @param Frequency Carrier frequency in Hz (doubled by the caller on BCM2711).
     * @param Bandwidth Half-width in Hz of the band that must not cross an integer multiplier (doubled by the
     *                  caller on BCM2711).
     * @return 0 when a divider keeps the band within one integer step, 1 when the best divider found crosses
     *         an integer step, -1 when the frequency is out of range.
     */
    int ComputeBestLO(uint64_t Frequency, int Bandwidth);

    /**
     * @brief Side-effect-free search of ComputeBestLO().
     * @param Frequency Frequency (doubled by the caller on BCM2711).
     * @param Bandwidth Half-width of the band (doubled by the caller on BCM2711).
     * @param dividerOut Receives the divider found (0 when none).
     * @param quiet     Suppress the diagnostic output.
     * @return Same as ComputeBestLO().
     */
    int QueryBestLO(uint64_t Frequency, int Bandwidth, int &dividerOut, bool quiet) const;

    /**
     * @brief Programs the PLLC multiplier.
     * @param Mult Integer part.
     * @param Frac Fractional part (20 bits).
     * @return Always 0.
     */
    int SetMasterMultFrac(uint32_t Mult, uint32_t Frac);

    /**
     * @brief Computes the PLLC fractional multiplier for a frequency offset.
     *
     * The value saturates at 0 or 0xFFFFF when the offset would require a different integer multiplier.
     *
     * @param Frequency Offset from the carrier in Hz.
     * @return 20-bit fractional multiplier, or 0 when advanced mode is disabled.
     */
    uint32_t GetMasterFrac(double Frequency);

    /**
     * @brief Returns the offset from the carrier actually produced by a fractional multiplier.
     *
     * Inverse of GetMasterFrac(), including its quantisation.
     *
     * @param Frac 20-bit fractional multiplier.
     * @return Offset in Hz, or 0 when advanced mode is disabled.
     */
    double GetMasterFracFrequency(uint32_t Frac);

    /**
     * @brief Sets the crystal correction.
     *
     * The crystal is modelled as running at nominal × (1 − ppm × 1e-6): a positive value means that the
     * crystal runs slow (the board would otherwise transmit below the requested frequency). This is the sign
     * of the kernel NTP frequency estimate.
     *
     * @param ppm Correction in ppm.
     */
    void Setppm(double ppm);

    /**
     * @brief Returns the crystal correction.
     * @return Correction in ppm (see Setppm()).
     */
    double Getppm() const;

    /** @brief Sets the crystal correction from the kernel NTP frequency estimate when it is valid. */
    void SetppmFromNTP();

    /**
     * @brief Tells whether PLLC reports lock.
     * @return True when the CM_LOCK FLOCKC bit is set.
     */
    bool IsPllcLocked();

    /**
     * @brief Waits for PLLC lock.
     * @param timeoutUs Maximum waiting time in microseconds.
     * @return True when PLLC reported lock in time.
     */
    bool WaitPllcLock(int timeoutUs);

private:
    /**
     * @brief Returns the factor between the GPCLK0 frequency and the PLLC multiplier × crystal.
     * @return Multiplier per Hz of output, times the crystal frequency.
     */
    double MultiplierScale() const;
};

/**
 * @name PCM / I2S
 * @{
 */
#define PCM_BASE (0x00203000) ///< Offset of the PCM block.
#define PCM_LEN 0x24          ///< Size of the PCM register block.

#define PCM_CS_A (0x00 / 4)      ///< Control and status.
#define PCM_FIFO_A (0x04 / 4)    ///< FIFO data.
#define PCM_MODE_A (0x08 / 4)    ///< Mode.
#define PCM_RXC_A (0x0c / 4)     ///< Receive configuration.
#define PCM_TXC_A (0x10 / 4)     ///< Transmit configuration.
#define PCM_DREQ_A (0x14 / 4)    ///< DMA request levels.
#define PCM_INTEN_A (0x18 / 4)   ///< Interrupt enables.
#define PCM_INT_STC_A (0x1c / 4) ///< Interrupt status.
#define PCM_GRAY (0x20 / 4)      ///< Gray mode control.

#define PCMCLK_CNTL (38) ///< PCM clock control (clock manager word index).
#define PCMCLK_DIV (39)  ///< PCM clock divider (clock manager word index).

#define PCM_CS_EN (1u << 0)      ///< CS: PCM block enabled.
#define PCM_CS_TXON (1u << 2)    ///< CS: transmitter running.
#define PCM_CS_TXCLR (1u << 3)   ///< CS: clear the TX FIFO.
#define PCM_CS_DMAEN (1u << 9)   ///< CS: DMA requests enabled.
#define PCM_CS_STBY (1u << 25)   ///< CS: FIFO RAM released from standby.
#define PCM_MODE_FTXP (1u << 24) ///< MODE: one TX FIFO word carries both 16-bit channels.
#define PCM_MODE_CLKI (1u << 22) ///< MODE: bit clock inverted.
#define PCM_MODE_FSI (1u << 20)  ///< MODE: frame sync inverted.

#define PCM_FIFO_DEPTH 64       ///< TX FIFO depth in words.
#define PCM_TX_DREQ_LEVEL 16    ///< TX DREQ and panic threshold in words: the FIFO is kept at this level.
#define PCM_DAC_FRAME_LENGTH 64 ///< Bit clocks per frame in DAC (I2S) mode: two 32-bit channel slots.
/** @} */

/**
 * @brief PCM block used as the DMA pacing source (one FIFO word is consumed per sample period) and, optionally,
 *        as a 16-bit I2S output carrying the word of each slot.
 *
 * The constructor only maps the registers and saves the PCM clock generator; the block is first changed by
 * SetFrequency() or SetDacFrequency(). The destructor then disables the block and restores the clock.
 *
 * The TX FIFO is kept at @ref PCM_TX_DREQ_LEVEL words, so the word written by a slot leaves the block
 * @ref PCM_TX_DREQ_LEVEL frames after the registers written by the same slot take effect.
 */
class pcmgpio : public gpio {
protected:
    gpio clk;                  ///< Clock manager mapping.
    int pllnumber = clk_plld;  ///< Clock source of the PCM clock.
    int Mash = 0;              ///< MASH order of the PCM clock.
    int Prediv = 0;            ///< PCM frame length in bit clocks.
    uint64_t Pllfrequency = 0; ///< Frequency of the selected source in Hz.
    double clk_ppm = 0;        ///< Crystal correction in ppm (see clkgpio::Setppm()).
    uint32_t savedCtl = 0;     ///< PCMCLK_CNTL at construction.
    uint32_t savedDiv = 0;     ///< PCMCLK_DIV at construction.
    uint32_t divInt = 0;       ///< Programmed integer divider.
    uint32_t divFrac = 0;      ///< Programmed fractional divider.
    bool used = false;         ///< The block has been configured (and must be restored).

    /**
     * @brief Computes and programs the PCM clock divider for a bit-clock rate.
     * @param bitRate Bit clock in Hz.
     * @return False when the divider is outside 2..4095 (nothing is programmed).
     */
    bool ProgramClock(double bitRate);

    /**
     * @brief Sets the frame length and starts PCM transmission with DMA requests.
     * @param predivisor Frame length in bit clocks (at most 1000).
     * @return Always 0.
     */
    int SetPrediv(int predivisor);

public:
    /** @brief Maps the PCM block and saves the PCM clock generator. */
    pcmgpio();

    /** @brief Disables the PCM block and restores the PCM clock generator when the block was configured. */
    ~pcmgpio();

    /**
     * @brief Selects the PCM clock source.
     * @param PllNo    Clock source (clk_*).
     * @param MashType MASH order (0-3).
     * @return Always 0.
     */
    int SetPllNumber(int PllNo, int MashType);

    /**
     * @brief Returns the frequency of a clock source, corrected by @ref clk_ppm.
     * @param PllNo Clock source (clk_*).
     * @return Frequency in Hz.
     */
    uint64_t GetPllFrequency(int PllNo);

    /**
     * @brief Sets the crystal correction applied to the PCM clock (takes effect at the next SetFrequency()).
     * @param ppm Correction in ppm (see clkgpio::Setppm()).
     */
    void Setppm(double ppm);

    /**
     * @brief Returns the crystal correction applied to the PCM clock.
     * @return Correction in ppm.
     */
    double Getppm() const;

    /**
     * @brief Sets the frame (sample) rate and starts the PCM block as a one-channel, 8-bit pacing source.
     * @param Frequency Frame rate in Hz.
     * @return 0, or -1 when no frame length keeps the clock divider in range for this rate (nothing is changed).
     */
    int SetFrequency(uint64_t Frequency);

    /**
     * @brief Tells whether SetFrequency() can produce a frame rate with the current source and correction.
     * @param Frequency Frame rate in Hz.
     * @return True when a frame length of 10 to 999 bit clocks keeps the clock divider within 2..4095.
     */
    bool FrequencyFits(uint64_t Frequency) const;

    /**
     * @brief Same as FrequencyFits() for another crystal correction (no state is changed, any thread).
     * @param Frequency Frame rate in Hz.
     * @param ppm       Correction in ppm.
     * @return True when the frame rate can be produced with that correction.
     */
    bool FrequencyFitsAt(uint64_t Frequency, double ppm) const;

    /**
     * @brief Sets the frame rate and starts the PCM block as a 16-bit stereo I2S master transmitter.
     *
     * The frame is @ref PCM_DAC_FRAME_LENGTH bit clocks long (frame sync high for half of it), both channels
     * are 16 bits with a one-bit delay, and one FIFO word carries both channels (packed mode). The pins are
     * not switched by this call.
     *
     * @param Frequency Frame rate in Hz.
     * @return 0, or -1 when the clock divider is out of range for this rate (nothing is changed).
     */
    int SetDacFrequency(uint64_t Frequency);

    /**
     * @brief Tells whether SetDacFrequency() can produce a frame rate with the current source and correction.
     * @param Frequency Frame rate in Hz.
     * @return True when the clock divider is within 2..4095.
     */
    bool DacFrequencyFits(uint64_t Frequency) const;

    /**
     * @brief Same as DacFrequencyFits() for another crystal correction (no state is changed, any thread).
     * @param Frequency Frame rate in Hz.
     * @param ppm       Correction in ppm.
     * @return True when the clock divider is within 2..4095.
     */
    bool DacFrequencyFitsAt(uint64_t Frequency, double ppm) const;

    /** @brief Clears the TX FIFO and (re)starts transmission and DMA requests (no effect before configuration). */
    void StartTx();

    /** @brief Stops transmission and DMA requests, the frame clock keeps running (no effect before configuration). */
    void StopTx();

    /**
     * @brief Returns the frame rate produced by the programmed dividers.
     * @return Frame rate in Hz (0 before SetFrequency()).
     */
    double GetFrameRate() const;

    /**
     * @brief Chooses the smallest frame length that keeps the clock divider in range.
     * @param Frequency Frame rate in Hz.
     * @return Frame length in bit clocks (10 to 999), or -1 when no frame length fits.
     */
    int ComputePrediv(uint64_t Frequency) const;

    /** @brief ComputePrediv() for a given source frequency. */
    static int ComputePrediv(uint64_t Frequency, uint64_t sourceFrequency);
};

/**
 * @name Pad control (output drive strength)
 * @{
 */
#define PADS_GPIO (0x00100000) ///< Offset of the pad control block.
#define PADS_GPIO_LEN 0x40     ///< Size of the pad control mapping in bytes.

#define PADS_GPIO_0 (0x2C / 4) ///< Pads of GPIO 0-27.
#define PADS_GPIO_1 (0x30 / 4) ///< Pads of GPIO 28-45.
#define PADS_GPIO_2 (0x34 / 4) ///< Pads of GPIO 46-53.

#define PADS_DRIVE_MASK 0x7 ///< Pad register: drive strength 0-7 (2 mA to 16 mA).
#define PADS_HYST (1u << 3) ///< Pad register: input hysteresis enabled.
#define PADS_SLEW (1u << 4) ///< Pad register: slew rate not limited.
/** @} */

/**
 * @name PWM
 * @{
 */
#define PWM_BASE (0x0020C000) ///< Offset of the PWM block (PWM0 on BCM2711).
#define PWM_LEN 0x28          ///< Size of the PWM register block.

#define PWM_CTL (0x00 / 4)  ///< Control.
#define PWM_STA (0x04 / 4)  ///< Status.
#define PWM_DMAC (0x08 / 4) ///< DMA configuration.
#define PWM_RNG1 (0x10 / 4) ///< Channel 1 range.
#define PWM_DAT1 (0x14 / 4) ///< Channel 1 data.
#define PWM_FIF1 (0x18 / 4) ///< FIFO input.

#define PWM_CTL_PWEN1 (1u << 0)  ///< CTL: channel 1 enabled.
#define PWM_CTL_USEF1 (1u << 5)  ///< CTL: channel 1 takes its data from the FIFO.
#define PWM_CTL_CLRF1 (1u << 6)  ///< CTL: clear the FIFO.
#define PWM_CTL_MSEN1 (1u << 7)  ///< CTL: channel 1 in mark/space mode.
#define PWM_DMAC_ENAB (1u << 31) ///< DMAC: DMA requests enabled.

#define PWM_FIFO_DEPTH 16 ///< FIFO depth in words.
#define PWM_DREQ_LEVEL 8  ///< DREQ and panic threshold in words: the FIFO is kept at this level.

#define PWMCLK_CNTL (40) ///< PWM clock control (clock manager word index).
#define PWMCLK_DIV (41)  ///< PWM clock divider (clock manager word index).
/** @} */

/**
 * @brief PWM channel 1 used as the DMA pacing source and as a mark/space DAC: each FIFO word is the duty
 *        (0 to range) of one period, and one word is consumed per period.
 *
 * The constructor maps the block and saves its registers and its clock generator; nothing is changed until
 * SetFrequency(). The destructor then stops the channel and restores the saved state.
 *
 * The FIFO is kept at @ref PWM_DREQ_LEVEL words, so the word written by a slot is output
 * @ref PWM_DREQ_LEVEL periods after the registers written by the same slot take effect.
 */
class pwmgpio : public gpio {
protected:
    gpio clk;                  ///< Clock manager mapping.
    int pllnumber = clk_plld;  ///< Clock source of the PWM clock.
    int Mash = 1;              ///< MASH order of the PWM clock.
    uint64_t Pllfrequency = 0; ///< Frequency of the source in Hz.
    double clk_ppm = 0;        ///< Crystal correction in ppm (see clkgpio::Setppm()).
    uint32_t range = 0;        ///< Programmed range (period in PWM clocks).
    uint32_t divInt = 0;       ///< Programmed integer divider.
    uint32_t divFrac = 0;      ///< Programmed fractional divider.
    bool used = false;         ///< The block has been configured (and must be restored).

public:
    uint32_t savedClkCtl = 0; ///< PWMCLK_CNTL at construction (without password).
    uint32_t savedClkDiv = 0; ///< PWMCLK_DIV at construction (without password).
    uint32_t savedCtl = 0;    ///< PWM_CTL at construction.
    uint32_t savedRng = 0;    ///< PWM_RNG1 at construction.
    uint32_t savedDmac = 0;   ///< PWM_DMAC at construction.

    /** @brief Maps the PWM block and the clock manager and saves their state. */
    pwmgpio();

    /** @brief Stops the channel and restores the saved state when the block was configured. */
    ~pwmgpio();

    /**
     * @brief Sets the crystal correction applied to the PWM clock (takes effect at the next SetFrequency()).
     * @param ppm Correction in ppm (see clkgpio::Setppm()).
     */
    void Setppm(double ppm);

    /**
     * @brief Returns the crystal correction applied to the PWM clock.
     * @return Correction in ppm.
     */
    double Getppm() const;

    /**
     * @brief Programs the period rate and the range and starts the channel with DMA requests.
     * @param Frequency Period (sample) rate in Hz.
     * @param Range     Period length in PWM clocks (the full-scale duty).
     * @return 0, or -1 when the clock divider is out of range (nothing is changed).
     */
    int SetFrequency(uint64_t Frequency, uint32_t Range);

    /**
     * @brief Tells whether SetFrequency() can produce a period rate with the current source and correction.
     * @param Frequency Period rate in Hz.
     * @param Range     Period length in PWM clocks.
     * @return True when the clock divider is within 2..4095.
     */
    bool FrequencyFits(uint64_t Frequency, uint32_t Range) const;

    /**
     * @brief Same as FrequencyFits() for another crystal correction (no state is changed, any thread).
     * @param Frequency Period rate in Hz.
     * @param Range     Period length in PWM clocks.
     * @param ppm       Correction in ppm.
     * @return True when the clock divider is within 2..4095.
     */
    bool FrequencyFitsAt(uint64_t Frequency, uint32_t Range, double ppm) const;

    /** @brief Clears the FIFO and (re)starts the channel and DMA requests (no effect before configuration). */
    void StartTx();

    /** @brief Stops the channel, the output stays low (no effect before configuration). */
    void StopTx();

    /**
     * @brief Returns the period rate produced by the programmed divider.
     * @return Rate in Hz (0 before SetFrequency()).
     */
    double GetFrameRate() const;

    /**
     * @brief Returns the programmed range.
     * @return Full-scale duty value.
     */
    uint32_t Range() const;

    /**
     * @brief Tells whether the block has been configured.
     * @return True after a successful SetFrequency().
     */
    bool Used() const;
};

/** @brief Pad drive strength of GPIO 0-27, used as a coarse amplitude control. */
class padgpio : public gpio {
public:
    /** @brief Maps the pad control block. */
    padgpio();

    /** @brief Releases the mapping. */
    ~padgpio();

    /**
     * @brief Sets the drive strength of GPIO 0-27, keeping their hysteresis and slew settings.
     * @param level Drive level 0-7 (2 mA to 16 mA in 2 mA steps).
     * @return Always 0.
     */
    int setlevel(int level);
};

/** @brief Bus addresses of the registers written by the DMA control blocks. */
enum dma_common_reg {
    dma_pllc_frac = 0x7E000000 + (PLLC_FRAC << 2) + CLK_BASE, ///< PLLC fractional multiplier.
    dma_pcm = 0x7E000000 + (PCM_FIFO_A << 2) + PCM_BASE,      ///< PCM FIFO.
    dma_fsel = 0x7E000000 + (GPFSEL0 << 2) + GENERAL_BASE,    ///< GPIO function select 0-9.
    dma_fsel2 = 0x7E000000 + (GPFSEL2 << 2) + GENERAL_BASE,   ///< GPIO function select 20-29.
    dma_pad = 0x7E000000 + (PADS_GPIO_0 << 2) + PADS_GPIO,    ///< Pad control of GPIO 0-27.
    dma_pwm = 0x7E000000 + (PWM_FIF1 << 2) + PWM_BASE         ///< PWM FIFO.
};

#endif
