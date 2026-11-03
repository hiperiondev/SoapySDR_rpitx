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
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
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
    r.clockTree.valid = treeFields == 17;
    return havePid;
}

std::string SoapyRPITX_RecoverHardware(const SoapyRPITX_SessionRecord &r, unsigned gpioLine) {
    std::ostringstream log;
    log << "process " << r.pid << ":";

    // Stop the transmission first: the DMA rewrites GPFSEL0 on every data slot.
    if (r.dmaChannel >= 0 && r.dmaChannel <= 14) {
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
        uint32_t function = r.gpioFunction == fsel_alt0 ? (uint32_t) fsel_input : r.gpioFunction;
        g.setmode(gpioLine, function);
        log << " GPIO" << gpioLine << " restored;";
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
        try {
            gpio pcm(PCM_BASE, PCM_LEN);
            pcm.gpioreg[PCM_CS_A] = 0;
        } catch (const std::exception &) {
        }
        if (r.pcmValid)
            cm_program_generator(cm.gpioreg, PCMCLK_CNTL, r.pcmCtl & CM_SRC_MASK, (r.pcmCtl >> CM_MASH_SHIFT) & 3, (r.pcmDiv >> 12) & 0xFFF, r.pcmDiv & 0xFFF,
                                 (r.pcmCtl & CM_ENAB) != 0);
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

bool SoapyRPITX_I2sEnabled() {
    std::ifstream in(RPITX_DT_ROOT "/soc/i2s@7e203000/status", std::ios::binary);
    if (!in)
        return false;
    std::string status;
    std::getline(in, status, '\0');
    return status == "okay" || status == "ok";
}

SoapyRPITX_Session::SoapyRPITX_Session(unsigned gpioLine) {
    std::string path = LockPath();
    fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        throw std::runtime_error("rpitx: cannot open the lock file " + path + ": " + strerror(errno));

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int err = errno;
        SoapyRPITX_SessionRecord r;
        std::string owner = SoapyRPITX_ParseRecord(readAll(fd), r) ? " by process " + std::to_string(r.pid) : "";
        close(fd);
        fd = -1;
        if (err == EWOULDBLOCK)
            throw std::runtime_error("rpitx: the transmitter is in use" + owner + " (" + path + ")");
        throw std::runtime_error("rpitx: cannot lock " + path + ": " + strerror(err));
    }

    SoapyRPITX_SessionRecord stale;
    if (SoapyRPITX_ParseRecord(readAll(fd), stale)) {
        report = SoapyRPITX_RecoverHardware(stale, gpioLine);
        recovered = true;
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

const std::string &SoapyRPITX_Session::RecoveryReport() const {
    return report;
}

void SoapyRPITX_Session::Write(const SoapyRPITX_SessionRecord &r) {
    SoapyRPITX_SessionRecord copy = r;
    copy.pid = (long) getpid();
    std::string text = SoapyRPITX_FormatRecord(copy);
    if (ftruncate(fd, 0) != 0 || pwrite(fd, text.data(), text.size(), 0) != (ssize_t) text.size())
        std::fprintf(stderr, "rpitx: cannot write %s: %s\n", LockPath().c_str(), strerror(errno));
    fdatasync(fd);
}

void SoapyRPITX_Session::Clear() {
    if (ftruncate(fd, 0) != 0)
        std::fprintf(stderr, "rpitx: cannot clear %s: %s\n", LockPath().c_str(), strerror(errno));
}

std::string SoapyRPITX_Session::LockPath() {
    return RPITX_LOCK_DIR "/" RPITX_LOCK_FILE_NAME;
}
