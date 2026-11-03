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
//
// rpitx_reset: returns the transmitter hardware to its idle state after a process using the SoapySDR rpitx
// driver was killed while transmitting.
//
// - The session record left in the lock file is recovered: DMA channel reset, carrier off, pads, PCM and
//   clock tree restored, VideoCore memory released.
// - Without a record, a clock output left on GPIO4 is switched off and GPCLK0 is stopped.
// - --dma-channel N additionally resets DMA channel N (e.g. after a crash of another librpitx program).
//
// Run as root. Suitable for ExecStopPost= in systemd units.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include "SoapyRPITX_Session.hpp"
#include "gpio.hpp"
#include "rpi.hpp"

namespace {

constexpr unsigned kRfGpio = 4;

void usage(const char *argv0) {
    std::fprintf(stderr, "usage: %s [--dma-channel N]\n", argv0);
}

} // namespace

int main(int argc, char **argv) {
    int extraChannel = -1;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--dma-channel") == 0 && i + 1 < argc) {
            char *end = nullptr;
            long v = std::strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 0 || v > 14) {
                std::fprintf(stderr, "invalid DMA channel '%s'\n", argv[i]);
                return 2;
            }
            extraChannel = (int) v;
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    try {
        SoapyRPITX_Session session(kRfGpio);
        if (session.Recovered()) {
            std::printf("recovered the transmitter left by %s\n", session.RecoveryReport().c_str());
        } else {
            generalgpio g;
            if (((g.gpioreg[GPFSEL0] >> (3 * kRfGpio)) & 7) == fsel_alt0) {
                g.setmode(kRfGpio, fsel_input);
                std::printf("GPIO%u clock output switched off\n", kRfGpio);
            }
            gpio cm(CLK_BASE, CLK_LEN);
            if (cm.gpioreg[GPCLK_CNTL] & CM_ENAB) {
                cm_stop_generator(cm.gpioreg, GPCLK_CNTL);
                std::printf("GPCLK0 stopped\n");
            }
            std::printf("no transmitter session to recover\n");
        }

        if (extraChannel >= 0) {
            bool known = false;
            uint32_t mask = bcm_host_get_kernel_dma_mask(&known);
            if (known && ((mask >> extraChannel) & 1u)) {
                std::fprintf(stderr, "DMA channel %d is reserved for the Linux kernel; not reset\n", extraChannel);
                return 1;
            }
            dmagpio d;
            d.gpioreg[DMA_CS + extraChannel * DMA_CHANNEL_WORDS] = BCM2708_DMA_RESET;
            usleep(1000);
            d.gpioreg[DMA_CS + extraChannel * DMA_CHANNEL_WORDS] = BCM2708_DMA_INT | BCM2708_DMA_END;
            std::printf("DMA channel %d reset\n", extraChannel);
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    return 0;
}
