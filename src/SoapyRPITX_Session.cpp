// Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
// Project Site: https://github.com/hiperiondev/SoapySDR_rpitx
//
// This is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// This software is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this software; see the file COPYING.  If not, write to
// the Free Software Foundation, Inc., 51 Franklin Street,
// Boston, MA 02110-1301, USA.

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "SoapyRPITX_Session.hpp"
#include "mailbox.hpp"
#include "rpi.hpp"

namespace {

std::string readAll(int fd) {
    std::string text;
    char buf[512];
    off_t off = 0;
    for (;;) {
        ssize_t n = pread(fd, buf, sizeof buf, off);
        if (n <= 0)
            break;
        text.append(buf, (size_t) n);
        off += n;
    }
    return text;
}

void put(std::ostringstream &out, const char *key, unsigned long v) {
    out << key << '=' << v << '\n';
}

} // namespace

std::string SoapyRPITX_FormatRecord(const SoapyRPITX_SessionRecord &r) {
    std::ostringstream out;
    out << "pid=" << r.pid << '\n';
    out << "dma_channel=" << r.dmaChannel << '\n';
    put(out, "mem_handle", r.memHandle);
    put(out, "pads", r.pads);
    put(out, "gpio_function", r.gpioFunction);
    if (r.pcmValid) {
        put(out, "pcm_ctl", r.pcmCtl);
        put(out, "pcm_div", r.pcmDiv);
    }
    for (const auto &pf : r.pinFunctions)
        put(out, ("pin_function_" + std::to_string(pf.first)).c_str(), pf.second);
    if (r.pwmValid) {
        put(out, "pwm_clk_ctl", r.pwmClkCtl);
        put(out, "pwm_clk_div", r.pwmClkDiv);
        put(out, "pwm_ctl", r.pwmCtl);
        put(out, "pwm_rng", r.pwmRng);
        put(out, "pwm_dmac", r.pwmDmac);
    }
    const ClockTreeState &t = r.clockTree;
    if (t.valid) {
        put(out, "core_ctl", t.coreCtl);
        put(out, "core_div", t.coreDiv);
        put(out, "emmc_ctl", t.emmcCtl);
        put(out, "emmc_div", t.emmcDiv);
        put(out, "gp0_ctl", t.gp0Ctl);
        put(out, "gp0_div", t.gp0Div);
        put(out, "cm_pllc", t.cmPllc);
        put(out, "pllc_ctrl", t.pllcCtrl);
        put(out, "pllc_frac", t.pllcFrac);
        put(out, "pllc_core0", t.pllcCore0);
        put(out, "pllc_core1", t.pllcCore1);
        put(out, "pllc_core2", t.pllcCore2);
        put(out, "pllc_per", t.pllcPer);
        for (int i = 0; i < 4; i++)
            put(out, ("pllc_ana" + std::to_string(i)).c_str(), t.pllcAna[i]);
    }
    return out.str();
}

bool SoapyRPITX_ParseRecord(const std::string &text, SoapyRPITX_SessionRecord &r) {
    r = SoapyRPITX_SessionRecord();
    bool havePid = false;
    int treeFields = 0;
    int pcmFields = 0;
    int pwmFields = 0;

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq);
        char *end = nullptr;
        long long v = std::strtoll(line.c_str() + eq + 1, &end, 10);
        if (end == line.c_str() + eq + 1)
            continue;
        uint32_t u = (uint32_t) v;
        ClockTreeState &t = r.clockTree;

        if (key == "pid") {
            r.pid = (long) v;
            havePid = true;
        } else if (key == "dma_channel") {
            r.dmaChannel = (int) v;
        } else if (key == "mem_handle") {
            r.memHandle = u;
        } else if (key == "pads") {
            r.pads = u;
        } else if (key == "gpio_function") {
            r.gpioFunction = u & 7;
        } else if (key == "pcm_ctl") {
            r.pcmCtl = u;
            pcmFields++;
        } else if (key == "pcm_div") {
            r.pcmDiv = u;
            pcmFields++;
        } else if (key.compare(0, 13, "pin_function_") == 0 && key.size() > 13) {
            char *pinEnd = nullptr;
            long pin = std::strtol(key.c_str() + 13, &pinEnd, 10);
            if (*pinEnd == '\0' && pin >= 0 && pin <= 53)
                r.pinFunctions.emplace_back((unsigned) pin, u & 7);
        } else if (key.compare(0, 4, "pwm_") == 0) {
            uint32_t *field = nullptr;
            if (key == "pwm_clk_ctl")
                field = &r.pwmClkCtl;
            else if (key == "pwm_clk_div")
                field = &r.pwmClkDiv;
            else if (key == "pwm_ctl")
                field = &r.pwmCtl;
            else if (key == "pwm_rng")
                field = &r.pwmRng;
            else if (key == "pwm_dmac")
                field = &r.pwmDmac;
            if (field != nullptr) {
                *field = u;
                pwmFields++;
            }
        } else {
            uint32_t *field = nullptr;
            if (key == "core_ctl")
                field = &t.coreCtl;
            else if (key == "core_div")
                field = &t.coreDiv;
            else if (key == "emmc_ctl")
                field = &t.emmcCtl;
            else if (key == "emmc_div")
                field = &t.emmcDiv;
            else if (key == "gp0_ctl")
                field = &t.gp0Ctl;
            else if (key == "gp0_div")
                field = &t.gp0Div;
            else if (key == "cm_pllc")
                field = &t.cmPllc;
            else if (key == "pllc_ctrl")
                field = &t.pllcCtrl;
            else if (key == "pllc_frac")
                field = &t.pllcFrac;
            else if (key == "pllc_core0")
                field = &t.pllcCore0;
            else if (key == "pllc_core1")
                field = &t.pllcCore1;
            else if (key == "pllc_core2")
                field = &t.pllcCore2;
            else if (key == "pllc_per")
                field = &t.pllcPer;
            else if (key.size() == 9 && key.compare(0, 8, "pllc_ana") == 0 && key[8] >= '0' && key[8] <= '3')
                field = &t.pllcAna[key[8] - '0'];
            if (field != nullptr) {
                *field = u;
                treeFields++;
            }
        }
    }

    r.pcmValid = pcmFields == 2;
    r.pwmValid = pwmFields == 5;
    r.clockTree.valid = treeFields == 17;
    return havePid;
}

uint32_t SoapyRPITX_RestoredFunction(unsigned pin, uint32_t function) {
    if ((pin == 4 && function == fsel_alt0) || (pin == 20 && function == fsel_alt5))
        return fsel_input;
    return function & 7;
}

bool SoapyRPITX_RecordPlausible(const SoapyRPITX_SessionRecord &r, std::string &why) {
    auto validSource = [](uint32_t ctl) { return (ctl & CM_SRC_MASK) <= 7; };

    if (r.dmaChannel >= 0) {
        if (r.dmaChannel > bcm_host_get_max_legacy_dma_channel()) {
            why = "DMA channel " + std::to_string(r.dmaChannel) + " is not a legacy channel of this SoC";
            return false;
        }
        bool known = false;
        uint32_t mask = bcm_host_get_kernel_dma_mask(&known);
        if (!known || ((mask >> r.dmaChannel) & 1u) != 0) {
            why = "DMA channel " + std::to_string(r.dmaChannel) + (known ? " is reserved for the kernel" : " cannot be checked against the kernel mask");
            return false;
        }
    }
    if (r.pads > 0x1F) {
        why = "invalid pad value";
        return false;
    }
    for (const auto &pf : r.pinFunctions) {
        if (pf.first > 27) {
            why = "GPIO" + std::to_string(pf.first) + " is not used by the transmitter";
            return false;
        }
    }
    if (r.pcmValid && (r.pcmCtl & CM_SRC_MASK) > 7) {
        why = "invalid PCM clock";
        return false;
    }
    if (r.pwmValid && (r.pwmClkCtl & CM_SRC_MASK) > 7) {
        why = "invalid PWM clock";
        return false;
    }
    const ClockTreeState &t = r.clockTree;
    if (t.valid) {
        uint32_t pdiv = (t.pllcCtrl >> 12) & 7;
        uint32_t ndiv = t.pllcCtrl & 0x3FF;
        if (pdiv == 0 || ndiv == 0 || !validSource(t.coreCtl) || !validSource(t.emmcCtl) || !validSource(t.gp0Ctl)) {
            why = "invalid clock tree";
            return false;
        }
    }
    return true;
}

std::string SoapyRPITX_RecoverHardware(const SoapyRPITX_SessionRecord &r, unsigned gpioLine) {
    std::ostringstream log;
    log << "process " << r.pid << ":";

    // Stop the transmission first: the DMA rewrites GPFSEL0 on every data slot.
    if (r.dmaChannel >= 0 && r.dmaChannel <= bcm_host_get_max_legacy_dma_channel()) {
        try {
            dmagpio d;
            d.gpioreg[DMA_CS + r.dmaChannel * DMA_CHANNEL_WORDS] = BCM2708_DMA_RESET;
            usleep(1000);
            d.gpioreg[DMA_CS + r.dmaChannel * DMA_CHANNEL_WORDS] = BCM2708_DMA_INT | BCM2708_DMA_END;
            log << " DMA channel " << r.dmaChannel << " reset;";
        } catch (const std::exception &e) {
            log << " DMA reset failed (" << e.what() << ");";
        }
    }

    try {
        generalgpio g;
        g.setmode(gpioLine, SoapyRPITX_RestoredFunction(gpioLine, r.gpioFunction));
        log << " GPIO" << gpioLine << " restored;";
        for (const auto &pf : r.pinFunctions) {
            g.setmode(pf.first, SoapyRPITX_RestoredFunction(pf.first, pf.second));
            log << " GPIO" << pf.first << " restored;";
        }
    } catch (const std::exception &e) {
        log << " GPIO restore failed (" << e.what() << ");";
    }

    try {
        padgpio p;
        p.gpioreg[PADS_GPIO_0] = (0x5Au << 24) | (r.pads & 0x1F);
        log << " pads restored;";
    } catch (const std::exception &e) {
        log << " pad restore failed (" << e.what() << ");";
    }

    try {
        gpio cm(CLK_BASE, CLK_LEN);
        cm_stop_generator(cm.gpioreg, GPCLK_CNTL);
        if (r.pcmValid) {
            try {
                gpio pcm(PCM_BASE, PCM_LEN);
                pcm.gpioreg[PCM_CS_A] = 0;
            } catch (const std::exception &) {
            }
        }
        if (r.pcmValid)
            cm_program_generator(cm.gpioreg, PCMCLK_CNTL, r.pcmCtl & CM_SRC_MASK, (r.pcmCtl >> CM_MASH_SHIFT) & 3, (r.pcmDiv >> 12) & 0xFFF, r.pcmDiv & 0xFFF,
                                 (r.pcmCtl & CM_ENAB) != 0);
        if (r.pwmValid) {
            try {
                gpio pwm(PWM_BASE, PWM_LEN);
                pwm.gpioreg[PWM_CTL] = 0;
                pwm.gpioreg[PWM_DMAC] = 0;
                cm_program_generator(cm.gpioreg, PWMCLK_CNTL, r.pwmClkCtl & CM_SRC_MASK, (r.pwmClkCtl >> CM_MASH_SHIFT) & 3, (r.pwmClkDiv >> 12) & 0xFFF,
                                     r.pwmClkDiv & 0xFFF, (r.pwmClkCtl & CM_ENAB) != 0);
                pwm.gpioreg[PWM_RNG1] = r.pwmRng;
                pwm.gpioreg[PWM_DMAC] = r.pwmDmac;
                pwm.gpioreg[PWM_CTL] = r.pwmCtl;
                log << " PWM restored;";
            } catch (const std::exception &e) {
                log << " PWM restore failed (" << e.what() << ");";
            }
        }
        if (r.clockTree.valid) {
            cm_restore_tree(cm.gpioreg, r.clockTree);
            log << " clock tree restored;";
        } else {
            log << " GPCLK0 stopped;";
        }
    } catch (const std::exception &e) {
        log << " clock restore failed (" << e.what() << ");";
    }

    if (r.memHandle != 0) {
        int mb = mbox_open();
        if (mb >= 0) {
            mem_unlock(mb, r.memHandle);
            if (mem_free(mb, r.memHandle) == 0)
                log << " VideoCore memory released;";
            else
                log << " VideoCore memory release failed;";
            mbox_close(mb);
        } else {
            log << " mailbox not accessible, VideoCore memory not released;";
        }
    }
    return log.str();
}

bool SoapyRPITX_GpioLineClaimed(unsigned line, std::string &consumer) {
    for (int chip = 0; chip < 16; chip++) {
        std::string path = "/dev/gpiochip" + std::to_string(chip);
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;

        struct gpiochip_info info;
        memset(&info, 0, sizeof info);
        bool soc = ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 && strncmp(info.label, "pinctrl-bcm2", 12) == 0;
        if (!soc || line >= info.lines) {
            close(fd);
            continue;
        }

        struct gpioline_info li;
        memset(&li, 0, sizeof li);
        li.line_offset = line;
        bool claimed = false;
        if (ioctl(fd, GPIO_GET_LINEINFO_IOCTL, &li) == 0 && (li.flags & GPIOLINE_FLAG_KERNEL) != 0) {
            claimed = true;
            consumer.assign(li.consumer, strnlen(li.consumer, sizeof li.consumer));
        }
        close(fd);
        return claimed;
    }
    return false;
}

namespace {

bool nodeEnabled(const char *path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::string status;
    std::getline(in, status, '\0');
    return status == "okay" || status == "ok";
}

// Owner of the lock: the recorded process, or a child that inherited the lock when the recorded process no
// longer exists.
std::string lockOwner(int fd) {
    SoapyRPITX_SessionRecord r;
    if (!SoapyRPITX_ParseRecord(readAll(fd), r))
        return "";
    if (r.pid > 0 && kill((pid_t) r.pid, 0) != 0 && errno == ESRCH)
        return " by a child process of terminated process " + std::to_string(r.pid) + " (a fork that inherited the lock)";
    return " by process " + std::to_string(r.pid);
}

void checkPrivateDirectory(const std::string &dir) {
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
        throw std::runtime_error("rpitx: cannot create the lock directory " + dir + ": " + strerror(errno));
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0)
        throw std::runtime_error("rpitx: cannot access the lock directory " + dir + ": " + strerror(errno));
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) != 0)
        throw std::runtime_error("rpitx: the lock directory " + dir + " must be a directory owned by the user running the driver, with mode 0700");
}

} // namespace

bool SoapyRPITX_I2sEnabled() {
    return nodeEnabled(RPITX_DT_ROOT "/soc/i2s@7e203000/status");
}

bool SoapyRPITX_PwmEnabled() {
    return nodeEnabled(RPITX_DT_ROOT "/soc/pwm@7e20c000/status") || nodeEnabled(RPITX_DT_ROOT "/soc/audio/status") ||
           nodeEnabled(RPITX_DT_ROOT "/audio/status");
}

SoapyRPITX_Session::SoapyRPITX_Session(unsigned gpioLine) {
    checkPrivateDirectory(RPITX_LOCK_DIR);
    std::string path = LockPath();
    fd = open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        throw std::runtime_error("rpitx: cannot open the lock file " + path + ": " + strerror(errno));

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1) {
        close(fd);
        fd = -1;
        throw std::runtime_error("rpitx: the lock file " + path + " must be a regular file owned by the user running the driver");
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int err = errno;
        std::string owner = lockOwner(fd);
        close(fd);
        fd = -1;
        if (err == EWOULDBLOCK)
            throw std::runtime_error("rpitx: the transmitter is in use" + owner + " (" + path + ")");
        throw std::runtime_error("rpitx: cannot lock " + path + ": " + strerror(err));
    }

    SoapyRPITX_SessionRecord stale;
    if (SoapyRPITX_ParseRecord(readAll(fd), stale)) {
        std::string why;
        if (SoapyRPITX_RecordPlausible(stale, why)) {
            report = SoapyRPITX_RecoverHardware(stale, gpioLine);
            recovered = true;
        } else {
            report = "process " + std::to_string(stale.pid) + ": " + why;
            discarded = true;
        }
    }
    Clear();
}

SoapyRPITX_Session::~SoapyRPITX_Session() {
    if (fd >= 0) {
        Clear();
        close(fd);
    }
}

bool SoapyRPITX_Session::Recovered() const {
    return recovered;
}

bool SoapyRPITX_Session::Discarded() const {
    return discarded;
}

const std::string &SoapyRPITX_Session::RecoveryReport() const {
    return report;
}

void SoapyRPITX_Session::Write(const SoapyRPITX_SessionRecord &r) {
    SoapyRPITX_SessionRecord copy = r;
    copy.pid = (long) getpid();
    std::string text = SoapyRPITX_FormatRecord(copy);
    // The new record overwrites the old one before the file is cut to its length, so the file never holds an
    // empty record while the transmitter owns hardware.
    ssize_t n = pwrite(fd, text.data(), text.size(), 0);
    if (n < 0)
        std::fprintf(stderr, "rpitx: cannot write %s: %s\n", LockPath().c_str(), strerror(errno));
    else if ((size_t) n != text.size())
        std::fprintf(stderr, "rpitx: cannot write %s: short write (%zd of %zu bytes)\n", LockPath().c_str(), n, text.size());
    else if (ftruncate(fd, (off_t) text.size()) != 0)
        std::fprintf(stderr, "rpitx: cannot truncate %s: %s\n", LockPath().c_str(), strerror(errno));
    fdatasync(fd);
}

void SoapyRPITX_Session::Clear() {
    if (ftruncate(fd, 0) != 0)
        std::fprintf(stderr, "rpitx: cannot clear %s: %s\n", LockPath().c_str(), strerror(errno));
}

std::string SoapyRPITX_Session::LockPath() {
    return RPITX_LOCK_DIR "/" RPITX_LOCK_FILE_NAME;
}
