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

#include "mailbox.hpp"
#include "gpio.hpp"
#include <unistd.h>
#include <sys/timex.h>
#include <math.h>
#include <string.h>
#include <stdexcept>
#include <string>
#include "util.hpp"
#include "rpi.hpp"

gpio::gpio(uint32_t base, uint32_t len) {
    gpiolen = len;
    gpioreg = (uint32_t *) mapmem(GetPeripheralBase() + base, len);
    if (gpioreg == NULL)
        throw std::runtime_error("librpitx: cannot map peripheral registers through /dev/mem; root privileges are required");
}

gpio::~gpio() {
    if (gpioreg != NULL)
        unmapmem((void *) gpioreg, gpiolen);
}

uint32_t gpio::GetPeripheralBase() {
    uint32_t base = bcm_host_get_peripheral_address();
    dbg_printf(2, "Peri Base = %x SDRAM %x\n", base, bcm_host_get_sdram_address());
    if (base == 0xFE000000) {
        pi_is_2711 = true;
        XOSC_FREQUENCY = 54000000;
    } else if (base == 0x20000000 || base == 0x3F000000) {
        pi_is_2711 = false;
        XOSC_FREQUENCY = 19200000;
    } else {
        char msg[96];
        snprintf(msg, sizeof msg, "librpitx: unsupported peripheral base address 0x%08x", base);
        throw std::runtime_error(msg);
    }
    return base;
}

// ******************** DMA Registers ***************************************

dmagpio::dmagpio() :
        gpio(DMA_BASE, DMA_LEN) {
}

// ***************** Clock generators *****************************************

void cm_stop_generator(volatile uint32_t *cm, unsigned ctl) {
    uint32_t v = cm[ctl] & 0x00FFFFFFu & ~(CM_ENAB | CM_KILL | CM_BUSY);
    cm[ctl] = CM_PASSWD | v;
    for (int i = 0; i < 100 && (cm[ctl] & CM_BUSY); i++)
        usleep(10);
    if (cm[ctl] & CM_BUSY) {
        cm[ctl] = CM_PASSWD | v | CM_KILL;
        usleep(10);
        cm[ctl] = CM_PASSWD | v;
    }
}

void cm_program_generator(volatile uint32_t *cm, unsigned ctl, unsigned src, unsigned mash, uint32_t divi, uint32_t divf, bool enable) {
    cm_stop_generator(cm, ctl);
    uint32_t c = CM_PASSWD | ((mash & 3u) << CM_MASH_SHIFT) | (src & CM_SRC_MASK);
    cm[ctl] = c;
    cm[ctl + 1] = CM_PASSWD | ((divi & 0xFFFu) << 12) | (divf & 0xFFFu);
    usleep(10);
    if (enable)
        cm[ctl] = c | CM_ENAB;
}

uint64_t cm_pll_frequency(volatile uint32_t *cm, int pll, uint64_t xosc, bool bcm2711, double ppm) {
    unsigned ctrl, frac, channel;
    switch (pll) {
        case clk_osc:
            return (uint64_t) ((double) xosc * (1.0 - ppm * 1e-6));
        case clk_plla:
            ctrl = PLLA_CTRL;
            frac = PLLA_FRAC;
            channel = PLLA_PER;
            break;
        case clk_pllc:
            ctrl = PLLC_CTRL;
            frac = PLLC_FRAC;
            channel = PLLC_PER;
            break;
        case clk_plld:
            ctrl = PLLD_CTRL;
            frac = PLLD_FRAC;
            channel = PLLD_PER;
            break;
        case clk_hdmi:
            ctrl = PLLH_CTRL;
            frac = PLLH_FRAC;
            channel = PLLH_AUX;
            break;
        default:
            return 0;
    }

    uint32_t ndiv = cm[ctrl] & 0x3FF;
    uint32_t pdiv = (cm[ctrl] >> 12) & 0x7;
    uint32_t chdiv = cm[channel] & 0xFF;
    if (pdiv == 0 || chdiv == 0)
        return 0;

    double vco = (double) xosc * ((double) ndiv + (double) (cm[frac] & 0xFFFFF) / (double) (1 << 20));
    double f = vco / (double) pdiv / (double) chdiv;
    if (!bcm2711) // the peripheral channel of the older SoCs runs at twice the computed rate
        f *= 2.0;
    return (uint64_t) (f * (1.0 - ppm * 1e-6));
}

void cm_save_tree(volatile uint32_t *cm, ClockTreeState &s) {
    const uint32_t m = 0x00FFFFFFu;
    s.coreCtl = cm[CORECLK_CNTL] & m;
    s.coreDiv = cm[CORECLK_DIV] & m;
    s.emmcCtl = cm[EMMCCLK_CNTL] & m;
    s.emmcDiv = cm[EMMCCLK_DIV] & m;
    s.gp0Ctl = cm[GPCLK_CNTL] & m;
    s.gp0Div = cm[GPCLK_DIV] & m;
    s.cmPllc = cm[CM_PLLC] & m;
    s.pllcCtrl = cm[PLLC_CTRL] & m;
    s.pllcFrac = cm[PLLC_FRAC] & m;
    s.pllcCore0 = cm[PLLC_CORE0] & m;
    s.pllcCore1 = cm[PLLC_CORE1] & m;
    s.pllcCore2 = cm[PLLC_CORE2] & m;
    s.pllcPer = cm[PLLC_PER] & m;
    for (int i = 0; i < 4; i++)
        s.pllcAna[i] = cm[A2W_PLLC_ANA0 + i] & m;
    s.valid = true;
}

void cm_restore_tree(volatile uint32_t *cm, const ClockTreeState &s) {
    if (!s.valid)
        return;

    cm_stop_generator(cm, GPCLK_CNTL);

    // PLLC first: its channels feed the clocks restored below. ANA3..ANA0 are written in that order so that
    // the four registers are transferred in one serdes cycle.
    for (int i = 3; i >= 0; i--) {
        cm[A2W_PLLC_ANA0 + i] = CM_PASSWD | s.pllcAna[i];
        usleep(100);
    }
    cm[PLLC_CTRL] = CM_PASSWD | s.pllcCtrl;
    usleep(100);
    cm[PLLC_FRAC] = CM_PASSWD | s.pllcFrac;
    for (int i = 0; i < 1000 && (cm[CM_LOCK] & CM_LOCK_FLOCKC) == 0; i++)
        usleep(10);
    if ((cm[CM_LOCK] & CM_LOCK_FLOCKC) == 0)
        dbg_printf(0, "Warning: PLLC not locked after restoring the clock tree\n");

    cm[PLLC_PER] = CM_PASSWD | s.pllcPer;
    cm[PLLC_CORE2] = CM_PASSWD | s.pllcCore2;
    cm[PLLC_CORE1] = CM_PASSWD | s.pllcCore1;
    cm[PLLC_CORE0] = CM_PASSWD | s.pllcCore0;
    usleep(100);
    cm[CM_PLLC] = CM_PASSWD | s.cmPllc;
    usleep(100);

    // The VPU clock cannot be stopped: its source is switched in place, as when it was moved to PLLA.
    cm[CORECLK_DIV] = CM_PASSWD | s.coreDiv;
    usleep(100);
    cm[CORECLK_CNTL] = CM_PASSWD | s.coreCtl;
    usleep(100);

    cm_program_generator(cm, EMMCCLK_CNTL, s.emmcCtl & CM_SRC_MASK, (s.emmcCtl >> CM_MASH_SHIFT) & 3, (s.emmcDiv >> 12) & 0xFFF, s.emmcDiv & 0xFFF,
                         (s.emmcCtl & CM_ENAB) != 0);
    cm_program_generator(cm, GPCLK_CNTL, s.gp0Ctl & CM_SRC_MASK, (s.gp0Ctl >> CM_MASH_SHIFT) & 3, (s.gp0Div >> 12) & 0xFFF, s.gp0Div & 0xFFF,
                         (s.gp0Ctl & CM_ENAB) != 0);
}

// ***************** CLK Registers *****************************************

clkgpio::clkgpio() :
        gpio(CLK_BASE, CLK_LEN) {
    SetppmFromNTP();
}

clkgpio::~clkgpio() {
    if (SavedTree.valid)
        cm_restore_tree(gpioreg, SavedTree);
    else
        cm_stop_generator(gpioreg, GPCLK_CNTL);
}

int clkgpio::SetPllNumber(int PllNo, int MashType) {
    if (dbg_getlevel() >= 2)
        print_clock_tree();
    pllnumber = PllNo < 8 ? PllNo : clk_pllc;
    Mash = MashType < 4 ? MashType : 0;
    cm_stop_generator(gpioreg, GPCLK_CNTL);
    gpioreg[GPCLK_CNTL] = CM_PASSWD | ((uint32_t) Mash << CM_MASH_SHIFT) | (uint32_t) pllnumber;
    Pllfrequency = GetPllFrequency(pllnumber);
    return 0;
}

uint64_t clkgpio::GetPllFrequency(int PllNo) {
    uint64_t f = cm_pll_frequency(gpioreg, PllNo, XOSC_FREQUENCY, pi_is_2711, clk_ppm);
    dbg_printf(1, "Pi4=%d Xosc = %llu Freq PLL no %d= %llu\n", pi_is_2711, (unsigned long long) XOSC_FREQUENCY, PllNo, (unsigned long long) f);
    return f;
}

int clkgpio::SetClkDivFrac(uint32_t Div, uint32_t Frac) {
    cm_program_generator(gpioreg, GPCLK_CNTL, (unsigned) pllnumber, (unsigned) Mash, Div, Frac, true);
    return 0;
}

int clkgpio::SetMasterMultFrac(uint32_t Mult, uint32_t Frac) {
    MasterMult = Mult;
    gpioreg[PLLC_CTRL] = (0x5a << 24) | (0x21 << 12) | Mult; // PDIV=1
    usleep(100);
    gpioreg[PLLC_FRAC] = 0x5A000000 | Frac;

    return 0;
}

double clkgpio::MultiplierScale() const {
    // GPCLK0 = XOSC x multiplier / scale, with the PLL pre-divider (PllFixDivider == 1, GPCLK divider 2) or the
    // fixed GPCLK divider; BCM2711 runs PLLC_PER at /2.
    if (PllFixDivider == 1)
        return pi_is_2711 ? 2.0 : 0.5;
    return pi_is_2711 ? (double) PllFixDivider * 2.0 : (double) PllFixDivider;
}

int clkgpio::SetFrequency(double Frequency) {
    if (ModulateFromMasterPLL) {
        double FloatMult = ((double) CentralFrequency + Frequency) * MultiplierScale() / ((double) XOSC_FREQUENCY * (1 - clk_ppm * 1e-6));
        uint32_t freqctl = (uint32_t) (FloatMult * (double) (1 << 20));
        SetMasterMultFrac(freqctl >> 20, freqctl & 0xFFFFF);
    } else {
        double Freqresult = (double) Pllfrequency / ((double) CentralFrequency + Frequency);
        uint32_t FreqDivider = (uint32_t) Freqresult;
        uint32_t FreqFractionnal = (uint32_t) (4096 * (Freqresult - (double) FreqDivider));
        if ((FreqDivider > 4096) || (FreqDivider < 2))
            dbg_printf(0, "Frequency out of range\n");
        dbg_printf(1, "DIV/FRAC %u/%u \n", FreqDivider, FreqFractionnal);

        SetClkDivFrac(FreqDivider, FreqFractionnal);
    }

    return 0;
}

uint32_t clkgpio::GetMasterFrac(double Frequency) {
    if (!ModulateFromMasterPLL)
        return 0;

    double FloatMult = ((double) CentralFrequency + Frequency) * MultiplierScale() / ((double) XOSC_FREQUENCY * (1 - clk_ppm * 1e-6));
    if (!(FloatMult > 0))
        return 0;
    uint32_t freqctl = (uint32_t) (FloatMult * (double) (1 << 20));
    uint32_t IntMultiply = freqctl >> 20;
    uint32_t FracMultiply = freqctl & 0xFFFFF;

    // Only the fractional part is rewritten by DMA: saturate rather than wrap into the next integer step.
    if (MasterMult != 0) {
        if (IntMultiply > MasterMult)
            return 0xFFFFF;
        if (IntMultiply < MasterMult)
            return 0;
    }
    return FracMultiply;
}

double clkgpio::GetMasterFracFrequency(uint32_t Frac) {
    if (!ModulateFromMasterPLL)
        return 0;
    double mult = (double) MasterMult + (double) (Frac & 0xFFFFF) / (double) (1 << 20);
    return mult * (double) XOSC_FREQUENCY * (1 - clk_ppm * 1e-6) / MultiplierScale() - (double) CentralFrequency;
}

int clkgpio::ComputeBestLO(uint64_t Frequency, int Bandwidth) {
    // Algorithm adapted from https://github.com/SaucySoliton/PiFmRds/blob/master/src/pi_fm_rds.c
    // Choose an integer divider for GPCLK0.
    // Constants taken https://github.com/raspberrypi/linux/blob/ffd7bf4085b09447e5db96edd74e524f118ca3fe/drivers/clk/bcm/clk-bcm2835.c#L1763
    // The kernel minimum PLL rate is 600 MHz; a lower limit is used here.
    const double MIN_PLL_RATE = 200e6;
    const double MIN_PLL_RATE_USE_PDIV = 1500e6; // above this VCO rate the PLL pre-divider is used
    const double MAX_PLL_RATE = 4e9;
    const double xtal_freq_recip = 1.0 / (double) XOSC_FREQUENCY; // nominal crystal, without ppm correction
    const double f = (double) Frequency;
    int best_divider = 0;
    bool cross_boundary = false;

    if (f < MIN_PLL_RATE / 4095) {
        dbg_printf(1, "Frequency too low\n");
        return -1;
    }
    if (f * 2 > MAX_PLL_RATE) {
        dbg_printf(1, "Frequency too high\n");
        return -1;
    }
    if (f * 2 > MIN_PLL_RATE_USE_PDIV) {
        best_divider = 1; // PLL pre-divider
        double scale = (pi_is_2711 ? 1.0 : 0.5) * xtal_freq_recip;
        cross_boundary = (int) ((f + Bandwidth) * scale) != (int) ((f - Bandwidth) * scale);
    } else {
        for (int divider = 4095; divider > 1; divider--) { // 1 is allowed only for MASH=0
            if (f * divider < MIN_PLL_RATE)
                continue; // widest accepted frequency range
            if (f * divider > MIN_PLL_RATE_USE_PDIV)
                continue; // by experiment on Rpi3B
            int max_int_multiplier = (int) ((f + Bandwidth) * divider * xtal_freq_recip);
            int min_int_multiplier = (int) ((f - Bandwidth) * divider * xtal_freq_recip);
            best_divider = divider;
            cross_boundary = min_int_multiplier != max_int_multiplier;
            if (!cross_boundary)
                break; // the band does not cross an integer multiplier
        }
    }
    if (best_divider == 0) {
        dbg_printf(1, "Central frequency not available\n");
        return -1;
    }

    PllFixDivider = best_divider;
    dbg_printf(1, "Found PLL solution for frequency %4.1fMHz : divider:%d VCO: %4.1fMHz%s\n", (f / 1e6), PllFixDivider,
               (f / 1e6) * ((PllFixDivider == 1) ? 2.0 : (double) PllFixDivider), cross_boundary ? " (cross boundary)" : "");
    return cross_boundary ? 1 : 0;
}

int clkgpio::SetCenterFrequency(uint64_t Frequency, int Bandwidth) {
    if (!ModulateFromMasterPLL) {
        CentralFrequency = Frequency;
        Pllfrequency = GetPllFrequency(pllnumber);
        SetFrequency(0);
        return 0;
    }

    // Choose the PLL divider first; nothing is reprogrammed when the frequency is unreachable.
    int lo = pi_is_2711 ? ComputeBestLO(Frequency * 2, Bandwidth * 2) : ComputeBestLO(Frequency, Bandwidth);
    if (lo < 0)
        return -1;
    CrossBoundary = lo == 1;
    CentralFrequency = Frequency;

    // The PLL pre-divider halves the VCO, compensated by a GPCLK divider of 2. The generator is configured
    // stopped and started once PLLC has its new multiplier.
    cm_program_generator(gpioreg, GPCLK_CNTL, (unsigned) pllnumber, (unsigned) Mash, PllFixDivider == 1 ? 2 : (uint32_t) PllFixDivider, 0, false);

    // Enable the PLL pre-divider only for divider 1.
    uint32_t ana[4];
    for (int i = 3; i >= 0; i--) {
        ana[i] = gpioreg[(A2W_PLLC_ANA0) + i];
        usleep(100);
        ana[i] &= ~(1 << 14);
    }

    if (PllFixDivider == 1) {
        dbg_printf(1, "Use PLL Prediv\n");
        ana[1] |= (1 << 14);
    }

    // ANA register setup is done as a series of writes to ANA3-ANA0, in that order. This lets all 4
    // registers be written as a single cycle of the serdes interface (taking 100 xosc clocks), whereas
    // updating ana0, 1 and 3 individually through their partial-write registers would take one serdes
    // cycle each.
    for (int i = 3; i >= 0; i--) {
        ana[i] = (ana[i] & 0x00FFFFFFu) | (0x5Au << 24);
        gpioreg[(A2W_PLLC_ANA0) + i] = ana[i];
        usleep(100);
    }

    SetFrequency(0);
    if (WaitPllcLock(2000))
        dbg_printf(1, "Master PLLC Locked\n");
    else
        dbg_printf(1, "Warning: master PLLC not locked\n");

    gpioreg[GPCLK_CNTL] = CM_PASSWD | ((uint32_t) Mash << CM_MASH_SHIFT) | (uint32_t) pllnumber | CM_ENAB;
    usleep(100);
    return 0;
}

bool clkgpio::CrossesBoundary() const {
    return CrossBoundary;
}

void clkgpio::print_clock_tree(void) {
    fprintf(stderr, "PLLC_DIG0=%08x\n", gpioreg[(0x1020 / 4)]);
    fprintf(stderr, "PLLC_DIG1=%08x\n", gpioreg[(0x1024 / 4)]);
    fprintf(stderr, "PLLC_DIG2=%08x\n", gpioreg[(0x1028 / 4)]);
    fprintf(stderr, "PLLC_DIG3=%08x\n", gpioreg[(0x102c / 4)]);
    fprintf(stderr, "PLLC_ANA0=%08x\n", gpioreg[(0x1030 / 4)]);
    fprintf(stderr, "PLLC_ANA1=%08x\n", gpioreg[(0x1034 / 4)]);
    fprintf(stderr, "PLLC_ANA2=%08x\n", gpioreg[(0x1038 / 4)]);
    fprintf(stderr, "PLLC_ANA3=%08x\n", gpioreg[(0x103c / 4)]);
    fprintf(stderr, "PLLC_DIG0R=%08x\n", gpioreg[(0x1820 / 4)]);
    fprintf(stderr, "PLLC_DIG1R=%08x\n", gpioreg[(0x1824 / 4)]);
    fprintf(stderr, "PLLC_DIG2R=%08x\n", gpioreg[(0x1828 / 4)]);
    fprintf(stderr, "PLLC_DIG3R=%08x\n", gpioreg[(0x182c / 4)]);

    fprintf(stderr, "PLLA_ANA0=%08x\n", gpioreg[(0x1010 / 4)]);
    fprintf(stderr, "PLLA_ANA1=%08x prediv=%d\n", gpioreg[(0x1014 / 4)], (gpioreg[(0x1014 / 4)] >> 14) & 1);
    fprintf(stderr, "PLLA_ANA2=%08x\n", gpioreg[(0x1018 / 4)]);
    fprintf(stderr, "PLLA_ANA3=%08x\n", gpioreg[(0x101c / 4)]);

    fprintf(stderr, "GNRIC CTL=%08x DIV=%8x  ", gpioreg[0], gpioreg[1]);
    fprintf(stderr, "VPU   CTL=%08x DIV=%8x\n", gpioreg[2], gpioreg[3]);
    fprintf(stderr, "SYS   CTL=%08x DIV=%8x  ", gpioreg[4], gpioreg[5]);
    fprintf(stderr, "PERIA CTL=%08x DIV=%8x\n", gpioreg[6], gpioreg[7]);
    fprintf(stderr, "PERII CTL=%08x DIV=%8x  ", gpioreg[8], gpioreg[9]);
    fprintf(stderr, "H264  CTL=%08x DIV=%8x\n", gpioreg[10], gpioreg[11]);
    fprintf(stderr, "ISP   CTL=%08x DIV=%8x  ", gpioreg[12], gpioreg[13]);
    fprintf(stderr, "V3D   CTL=%08x DIV=%8x\n", gpioreg[14], gpioreg[15]);

    fprintf(stderr, "CAM0  CTL=%08x DIV=%8x  ", gpioreg[16], gpioreg[17]);
    fprintf(stderr, "CAM1  CTL=%08x DIV=%8x\n", gpioreg[18], gpioreg[19]);
    fprintf(stderr, "CCP2  CTL=%08x DIV=%8x  ", gpioreg[20], gpioreg[21]);
    fprintf(stderr, "DSI0E CTL=%08x DIV=%8x\n", gpioreg[22], gpioreg[23]);
    fprintf(stderr, "DSI0P CTL=%08x DIV=%8x  ", gpioreg[24], gpioreg[25]);
    fprintf(stderr, "DPI   CTL=%08x DIV=%8x\n", gpioreg[26], gpioreg[27]);
    fprintf(stderr, "GP0   CTL=%08x DIV=%8x  ", gpioreg[0x70 / 4], gpioreg[0x74 / 4]);
    fprintf(stderr, "GP1   CTL=%08x DIV=%8x\n", gpioreg[30], gpioreg[31]);

    fprintf(stderr, "GP2   CTL=%08x DIV=%8x  ", gpioreg[32], gpioreg[33]);
    fprintf(stderr, "HSM   CTL=%08x DIV=%8x\n", gpioreg[34], gpioreg[35]);
    fprintf(stderr, "OTP   CTL=%08x DIV=%8x  ", gpioreg[36], gpioreg[37]);
    fprintf(stderr, "PCM   CTL=%08x DIV=%8x\n", gpioreg[38], gpioreg[39]);
    fprintf(stderr, "PWM   CTL=%08x DIV=%8x  ", gpioreg[40], gpioreg[41]);
    fprintf(stderr, "SLIM  CTL=%08x DIV=%8x\n", gpioreg[42], gpioreg[43]);
    fprintf(stderr, "SMI   CTL=%08x DIV=%8x  ", gpioreg[44], gpioreg[45]);
    fprintf(stderr, "SMPS  CTL=%08x DIV=%8x\n", gpioreg[46], gpioreg[47]);

    fprintf(stderr, "TCNT  CTL=%08x DIV=%8x  ", gpioreg[48], gpioreg[49]);
    fprintf(stderr, "TEC   CTL=%08x DIV=%8x\n", gpioreg[50], gpioreg[51]);
    fprintf(stderr, "TD0   CTL=%08x DIV=%8x  ", gpioreg[52], gpioreg[53]);
    fprintf(stderr, "TD1   CTL=%08x DIV=%8x\n", gpioreg[54], gpioreg[55]);

    fprintf(stderr, "TSENS CTL=%08x DIV=%8x  ", gpioreg[56], gpioreg[57]);
    fprintf(stderr, "TIMER CTL=%08x DIV=%8x\n", gpioreg[58], gpioreg[59]);
    fprintf(stderr, "UART  CTL=%08x DIV=%8x  ", gpioreg[60], gpioreg[61]);
    fprintf(stderr, "VEC   CTL=%08x DIV=%8x\n", gpioreg[62], gpioreg[63]);

    fprintf(stderr, "PULSE CTL=%08x DIV=%8x  ", gpioreg[100], gpioreg[101]);
    fprintf(stderr, "PLLT  CTL=%08x DIV=????????\n", gpioreg[76]);

    fprintf(stderr, "DSI1E CTL=%08x DIV=%8x  ", gpioreg[86], gpioreg[87]);
    fprintf(stderr, "DSI1P CTL=%08x DIV=%8x\n", gpioreg[88], gpioreg[89]);
    fprintf(stderr, "AVE0  CTL=%08x DIV=%8x\n", gpioreg[90], gpioreg[91]);

    fprintf(stderr, "CMPLLA=%08x  ", gpioreg[0x104 / 4]);
    fprintf(stderr, "CMPLLC=%08x \n", gpioreg[0x108 / 4]);
    fprintf(stderr, "CMPLLD=%08x   ", gpioreg[0x10C / 4]);
    fprintf(stderr, "CMPLLH=%08x \n", gpioreg[0x110 / 4]);

    fprintf(stderr, "EMMC  CTL=%08x DIV=%8x\n", gpioreg[112], gpioreg[113]);

    // Sometimes calculated frequencies are off by a factor of 2
    // ANA1 bit 14 may indicate that a /2 prescaler is active
    double xoscmhz = XOSC_FREQUENCY / 1e6;
    fprintf(stderr, "PLLA PDIV=%d NDIV=%d FRAC=%d  ", (gpioreg[PLLA_CTRL] >> 12) & 0x7, gpioreg[PLLA_CTRL] & 0x3ff, gpioreg[PLLA_FRAC]);
    fprintf(stderr, " %f MHz\n", xoscmhz * ((float) (gpioreg[PLLA_CTRL] & 0x3ff) + ((float) gpioreg[PLLA_FRAC]) / ((float) (1 << 20))));
    fprintf(stderr, "DSI0=%d CORE=%d PER=%d CCP2=%d\n\n", gpioreg[PLLA_DSI0], gpioreg[PLLA_CORE], gpioreg[PLLA_PER], gpioreg[PLLA_CCP2]);

    fprintf(stderr, "PLLB PDIV=%d NDIV=%d FRAC=%d  ", (gpioreg[PLLB_CTRL] >> 12) & 0x7, gpioreg[PLLB_CTRL] & 0x3ff, gpioreg[PLLB_FRAC]);
    fprintf(stderr, " %f MHz\n", xoscmhz * ((float) (gpioreg[PLLB_CTRL] & 0x3ff) + ((float) gpioreg[PLLB_FRAC]) / ((float) (1 << 20))));
    fprintf(stderr, "ARM=%d SP0=%d SP1=%d SP2=%d\n\n", gpioreg[PLLB_ARM], gpioreg[PLLB_SP0], gpioreg[PLLB_SP1], gpioreg[PLLB_SP2]);

    fprintf(stderr, "PLLC PDIV=%d NDIV=%d FRAC=%d  ", (gpioreg[PLLC_CTRL] >> 12) & 0x7, gpioreg[PLLC_CTRL] & 0x3ff, gpioreg[PLLC_FRAC]);
    fprintf(stderr, " %f MHz\n", xoscmhz * ((float) (gpioreg[PLLC_CTRL] & 0x3ff) + ((float) gpioreg[PLLC_FRAC]) / ((float) (1 << 20))));
    fprintf(stderr, "CORE2=%d CORE1=%d PER=%d CORE0=%d\n\n", gpioreg[PLLC_CORE2], gpioreg[PLLC_CORE1], gpioreg[PLLC_PER], gpioreg[PLLC_CORE0]);

    fprintf(stderr, "PLLD %x PDIV=%d NDIV=%d FRAC=%d  ", gpioreg[PLLD_CTRL], (gpioreg[PLLD_CTRL] >> 12) & 0x7, gpioreg[PLLD_CTRL] & 0x3ff, gpioreg[PLLD_FRAC]);
    fprintf(stderr, " %f MHz\n", xoscmhz * ((float) (gpioreg[PLLD_CTRL] & 0x3ff) + ((float) gpioreg[PLLD_FRAC]) / ((float) (1 << 20))));
    fprintf(stderr, "DSI0=%d CORE=%d PER=%d DSI1=%d\n\n", gpioreg[PLLD_DSI0], gpioreg[PLLD_CORE], gpioreg[PLLD_PER], gpioreg[PLLD_DSI1]);

    fprintf(stderr, "PLLH PDIV=%d NDIV=%d FRAC=%d  ", (gpioreg[PLLH_CTRL] >> 12) & 0x7, gpioreg[PLLH_CTRL] & 0x3ff, gpioreg[PLLH_FRAC]);
    fprintf(stderr, " %f MHz\n", xoscmhz * ((float) (gpioreg[PLLH_CTRL] & 0x3ff) + ((float) gpioreg[PLLH_FRAC]) / ((float) (1 << 20))));
    fprintf(stderr, "AUX=%d RCAL=%d PIX=%d STS=%d\n\n", gpioreg[PLLH_AUX], gpioreg[PLLH_RCAL], gpioreg[PLLH_PIX], gpioreg[PLLH_STS]);
}

// https://elinux.org/The_Undocumented_Pi
// See also https://github.com/raspberrypi/linux/blob/ffd7bf4085b09447e5db96edd74e524f118ca3fe/drivers/clk/bcm/clk-bcm2835.c#L695
void clkgpio::SaveClockTree() {
    if (!SavedTree.valid)
        cm_save_tree(gpioreg, SavedTree);
}

void clkgpio::SetAdvancedPllMode(bool Advanced) {
    ModulateFromMasterPLL = Advanced;
    if (ModulateFromMasterPLL) {
        SaveClockTree();

        // Move the clocks that depend on PLLC away from it, since PLLC is modulated.
        // switch the core over to PLLA
        gpioreg[CORECLK_DIV] = (0x5a << 24) | (4 << 12); // core div 4
        usleep(100);
        gpioreg[CORECLK_CNTL] = (0x5a << 24) | (1 << 4) | (4); // run, src=PLLA

        // switch the EMMC over to PLLD
        uint32_t clktmp = gpioreg[EMMCCLK_CNTL];
        gpioreg[EMMCCLK_CNTL] = (0xF0F & clktmp) | (0x5a << 24); // clear run
        usleep(100);
        gpioreg[EMMCCLK_CNTL] = (0xF00 & clktmp) | (0x5a << 24) | (6); // src=PLLD
        usleep(100);
        gpioreg[EMMCCLK_CNTL] = (0xF00 & clktmp) | (0x5a << 24) | (1 << 4) | (6); // run , src=PLLD

        SetPllNumber(clk_pllc, 0); // Use PLL_C , Do not USE MASH which generates spurious

        gpioreg[CM_PLLC] = 0x5A00022A; // Enable PllC_PER
        usleep(100);

        gpioreg[PLLC_CORE0] = 0x5A000000 | (1 << 8); //Disable
        if (pi_is_2711)
            gpioreg[PLLC_PER] = 0x5A000002; // Divisor by 2 (1 seems not working on pi4)
        else {
            gpioreg[PLLC_PER] = 0x5A000001; // Divisor 1 for max frequence
        }

        usleep(100);
    }
}

void clkgpio::RestoreClockTree() {
    if (!SavedTree.valid)
        return;
    cm_restore_tree(gpioreg, SavedTree);
    SavedTree.valid = false;
    ModulateFromMasterPLL = false;
}

const ClockTreeState &clkgpio::SavedClockTree() const {
    return SavedTree;
}

void clkgpio::Setppm(double ppm) {
    clk_ppm = ppm;
}

double clkgpio::Getppm() const {
    return clk_ppm;
}

bool clkgpio::IsPllcLocked() {
    return (gpioreg[CM_LOCK] & CM_LOCK_FLOCKC) != 0;
}

bool clkgpio::WaitPllcLock(int timeoutUs) {
    for (int waited = 0; !IsPllcLocked(); waited += 10) {
        if (waited >= timeoutUs)
            return false;
        usleep(10);
    }
    return true;
}

void clkgpio::SetppmFromNTP() {
    struct timex ntx;
    memset(&ntx, 0, sizeof ntx);
    // Use the kernel NTP frequency estimate as the crystal correction.

    ntx.modes = 0; // read only
    int status = ntp_adjtime(&ntx);

    if (status != TIME_OK) {
        dbg_printf(1, "Warning: NTP calibrate failed\n");
    } else {
        double ntp_ppm = (double) ntx.freq / (double) (1 << 16);
        dbg_printf(1, "Info:NTP find offset %ld freq %ld pps=%ld ppm=%f\n", (long) ntx.offset, (long) ntx.freq, (long) ntx.ppsfreq, ntp_ppm);

        if (fabs(ntp_ppm) < 200)
            Setppm(ntp_ppm);
    }
}

// ************************************** GENERAL GPIO *****************************************************

generalgpio::generalgpio() :
        gpio(GENERAL_BASE, GENERAL_LEN) {
}

generalgpio::~generalgpio() {
}

int generalgpio::setmode(uint32_t pin, uint32_t mode) {
    uint32_t reg = pin / 10;
    uint32_t shift = (pin % 10) * 3;

    gpioreg[reg] = (gpioreg[reg] & ~(7u << shift)) | (mode << shift);

    return 0;
}

// ********************************** PCM GPIO (I2S) **********************************

pcmgpio::pcmgpio() :
        gpio(PCM_BASE, PCM_LEN),
        clk(CLK_BASE, CLK_LEN) {
    savedCtl = clk.gpioreg[PCMCLK_CNTL] & 0x00FFFFFFu;
    savedDiv = clk.gpioreg[PCMCLK_DIV] & 0x00FFFFFFu;
}

pcmgpio::~pcmgpio() {
    if (!used)
        return;
    gpioreg[PCM_CS_A] = 0; // PCM block, transmitter and DMA requests off
    cm_program_generator(clk.gpioreg, PCMCLK_CNTL, savedCtl & CM_SRC_MASK, (savedCtl >> CM_MASH_SHIFT) & 3, (savedDiv >> 12) & 0xFFF, savedDiv & 0xFFF,
                         (savedCtl & CM_ENAB) != 0);
}

int pcmgpio::SetPllNumber(int PllNo, int MashType) {
    pllnumber = PllNo < 8 ? PllNo : clk_pllc;
    Mash = MashType < 4 ? MashType : 0;
    Pllfrequency = GetPllFrequency(pllnumber);
    return 0;
}

uint64_t pcmgpio::GetPllFrequency(int PllNo) {
    return cm_pll_frequency(clk.gpioreg, PllNo, clk.XOSC_FREQUENCY, clk.pi_is_2711, clk_ppm);
}

void pcmgpio::Setppm(double ppm) {
    clk_ppm = ppm;
    Pllfrequency = GetPllFrequency(pllnumber);
}

double pcmgpio::Getppm() const {
    return clk_ppm;
}

int pcmgpio::ComputePrediv(uint64_t Frequency) const {
    if (Frequency == 0)
        return -1;
    for (int prediv = 10; prediv < 1000; prediv++) {
        double Freqresult = (double) Pllfrequency / (double) (Frequency * (uint64_t) prediv);
        if ((Freqresult < 4096.0) && (Freqresult >= 2.0))
            return prediv;
    }
    return -1;
}

bool pcmgpio::FrequencyFits(uint64_t Frequency) const {
    return ComputePrediv(Frequency) > 0;
}

bool pcmgpio::ProgramClock(double bitRate) {
    if (!(bitRate > 0))
        return false;
    double Freqresult = (double) Pllfrequency / bitRate;
    if (!(Freqresult >= 2.0 && Freqresult < 4096.0))
        return false;
    divInt = (uint32_t) Freqresult;
    divFrac = (uint32_t) (4096 * (Freqresult - (double) divInt));
    dbg_printf(1, "PCM clk=%u / %u\n", divInt, divFrac);
    cm_program_generator(clk.gpioreg, PCMCLK_CNTL, (unsigned) pllnumber, (unsigned) Mash, divInt, divFrac, true);
    return true;
}

int pcmgpio::SetFrequency(uint64_t Frequency) {
    int prediv = ComputePrediv(Frequency);
    if (prediv < 0) {
        dbg_printf(0, "PCM: no frame length for %llu Hz\n", (unsigned long long) Frequency);
        return -1;
    }
    dbg_printf(1, "PCM prediv = %d\n", prediv);

    used = true;
    gpioreg[PCM_CS_A] = PCM_CS_EN | PCM_CS_STBY; // block enabled, transmitter and receiver off
    Prediv = prediv;
    if (!ProgramClock((double) Frequency * (double) Prediv)) {
        dbg_printf(0, "PCM Frequency out of range\n");
        return -1;
    }
    SetPrediv(Prediv);
    return 0;
}

bool pcmgpio::DacFrequencyFits(uint64_t Frequency) const {
    double Freqresult = (double) Pllfrequency / ((double) Frequency * PCM_DAC_FRAME_LENGTH);
    return Freqresult >= 2.0 && Freqresult < 4096.0;
}

int pcmgpio::SetDacFrequency(uint64_t Frequency) {
    if (!DacFrequencyFits(Frequency))
        return -1;

    used = true;
    gpioreg[PCM_CS_A] = PCM_CS_EN | PCM_CS_STBY;
    usleep(100);
    Prediv = PCM_DAC_FRAME_LENGTH;
    ProgramClock((double) Frequency * PCM_DAC_FRAME_LENGTH);

    // I2S: frame sync low for the left channel, data one bit clock after the frame edge, MSB first.
    const uint32_t half = PCM_DAC_FRAME_LENGTH / 2;
    gpioreg[PCM_MODE_A] = PCM_MODE_FTXP | PCM_MODE_CLKI | PCM_MODE_FSI | ((uint32_t) (PCM_DAC_FRAME_LENGTH - 1) << 10) | half;
    usleep(100);
    // Channel 1 and 2: enabled, 16 bits (width field 8 = 8 + 8 bits), positions 1 and half + 1.
    gpioreg[PCM_TXC_A] = (1u << 30) | (1u << 20) | (8u << 16) | (1u << 14) | ((half + 1) << 4) | 8u;
    usleep(100);
    gpioreg[PCM_DREQ_A] = (uint32_t) PCM_TX_DREQ_LEVEL << 24 | (uint32_t) PCM_TX_DREQ_LEVEL << 8;
    usleep(100);
    StartTx();
    return 0;
}

void pcmgpio::StartTx() {
    if (!used)
        return;
    gpioreg[PCM_CS_A] = (gpioreg[PCM_CS_A] & ~(PCM_CS_TXON | PCM_CS_DMAEN)) | PCM_CS_TXCLR;
    usleep(10);
    gpioreg[PCM_CS_A] |= PCM_CS_DMAEN;
    gpioreg[PCM_CS_A] |= PCM_CS_TXON;
}

void pcmgpio::StopTx() {
    if (!used)
        return;
    gpioreg[PCM_CS_A] &= ~(PCM_CS_TXON | PCM_CS_DMAEN);
}

double pcmgpio::GetFrameRate() const {
    if (divInt == 0 || Prediv == 0)
        return 0;
    return (double) Pllfrequency / (((double) divInt + (double) divFrac / 4096.0) * (double) Prediv);
}

int pcmgpio::SetPrediv(int predivisor) {
    // The frame rate (sample rate) is the PCM clock divided by the frame length.
    if (predivisor > 1000) {
        dbg_printf(1, "PCM prediv should be <1000");
        predivisor = 1000;
    }

    gpioreg[PCM_TXC_A] = 0 << 31 | 1 << 30 | 0 << 20 | 0 << 16; // 1 channel, 8 bits
    usleep(100);

    gpioreg[PCM_MODE_A] = (uint32_t) (predivisor - 1) << 10; // SHOULD NOT EXCEED 1000 !!!
    usleep(100);
    gpioreg[PCM_DREQ_A] = (uint32_t) PCM_TX_DREQ_LEVEL << 24 | (uint32_t) PCM_TX_DREQ_LEVEL << 8; // TX DREQ and panic levels
    usleep(100);
    StartTx();

    return 0;
}

// ********************************** PWM GPIO **********************************

pwmgpio::pwmgpio() :
        gpio(PWM_BASE, PWM_LEN),
        clk(CLK_BASE, CLK_LEN) {
    savedClkCtl = clk.gpioreg[PWMCLK_CNTL] & 0x00FFFFFFu;
    savedClkDiv = clk.gpioreg[PWMCLK_DIV] & 0x00FFFFFFu;
    savedCtl = gpioreg[PWM_CTL];
    savedRng = gpioreg[PWM_RNG1];
    savedDmac = gpioreg[PWM_DMAC];
    Pllfrequency = cm_pll_frequency(clk.gpioreg, pllnumber, clk.XOSC_FREQUENCY, clk.pi_is_2711, clk_ppm);
}

pwmgpio::~pwmgpio() {
    if (!used)
        return;
    gpioreg[PWM_CTL] = 0;
    gpioreg[PWM_DMAC] = 0;
    usleep(10);
    cm_program_generator(clk.gpioreg, PWMCLK_CNTL, savedClkCtl & CM_SRC_MASK, (savedClkCtl >> CM_MASH_SHIFT) & 3, (savedClkDiv >> 12) & 0xFFF,
                         savedClkDiv & 0xFFF, (savedClkCtl & CM_ENAB) != 0);
    gpioreg[PWM_RNG1] = savedRng;
    gpioreg[PWM_DMAC] = savedDmac;
    gpioreg[PWM_CTL] = savedCtl;
}

void pwmgpio::Setppm(double ppm) {
    clk_ppm = ppm;
    Pllfrequency = cm_pll_frequency(clk.gpioreg, pllnumber, clk.XOSC_FREQUENCY, clk.pi_is_2711, clk_ppm);
}

double pwmgpio::Getppm() const {
    return clk_ppm;
}

bool pwmgpio::FrequencyFits(uint64_t Frequency, uint32_t Range) const {
    if (Range < 2)
        return false;
    double Freqresult = (double) Pllfrequency / ((double) Frequency * (double) Range);
    return Freqresult >= 2.0 && Freqresult < 4096.0;
}

int pwmgpio::SetFrequency(uint64_t Frequency, uint32_t Range) {
    if (!FrequencyFits(Frequency, Range))
        return -1;
    double Freqresult = (double) Pllfrequency / ((double) Frequency * (double) Range);

    used = true;
    range = Range;
    divInt = (uint32_t) Freqresult;
    divFrac = (uint32_t) (4096 * (Freqresult - (double) divInt));

    gpioreg[PWM_CTL] = 0;
    gpioreg[PWM_DMAC] = 0;
    usleep(10);
    cm_program_generator(clk.gpioreg, PWMCLK_CNTL, (unsigned) pllnumber, (unsigned) Mash, divInt, divFrac, true);
    gpioreg[PWM_RNG1] = range;
    usleep(10);
    gpioreg[PWM_DMAC] = PWM_DMAC_ENAB | ((uint32_t) PWM_DREQ_LEVEL << 8) | (uint32_t) PWM_DREQ_LEVEL;
    StartTx();
    return 0;
}

void pwmgpio::StartTx() {
    if (!used)
        return;
    gpioreg[PWM_CTL] = PWM_CTL_CLRF1;
    usleep(10);
    gpioreg[PWM_CTL] = PWM_CTL_PWEN1 | PWM_CTL_USEF1 | PWM_CTL_MSEN1;
}

void pwmgpio::StopTx() {
    if (!used)
        return;
    gpioreg[PWM_CTL] = 0;
}

double pwmgpio::GetFrameRate() const {
    if (divInt == 0 || range == 0)
        return 0;
    return (double) Pllfrequency / (((double) divInt + (double) divFrac / 4096.0) * (double) range);
}

uint32_t pwmgpio::Range() const {
    return range;
}

bool pwmgpio::Used() const {
    return used;
}

// ********************************** PADGPIO (Amplitude) **********************************

padgpio::padgpio() :
        gpio(PADS_GPIO, PADS_GPIO_LEN) {
}

padgpio::~padgpio() {
}

int padgpio::setlevel(int level) {
    uint32_t keep = gpioreg[PADS_GPIO_0] & (PADS_HYST | PADS_SLEW);
    gpioreg[PADS_GPIO_0] = (0x5Au << 24) | keep | ((uint32_t) level & PADS_DRIVE_MASK);
    return 0;
}
