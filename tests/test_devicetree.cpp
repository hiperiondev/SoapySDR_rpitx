// Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
// Project Site: https://github.com/hiperiondev/SoapySDR_rpitx
//
// This is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// Test of the device-tree queries of librpitx/src/rpi.cpp against generated trees: peripheral base, legacy DMA
// channel limit and the kernel DMA channel mask with the old ("dma@", "brcm,dma-channel-mask") and the current
// ("dma-controller@", "dma-channel-mask") naming.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "rpi.hpp"

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                                                            \
    do {                                                                                                                                                       \
        if (!(cond)) {                                                                                                                                         \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                                                      \
            failures++;                                                                                                                                        \
        }                                                                                                                                                      \
    } while (0)

const std::string kRoot = RPITX_DT_ROOT;

void writeCells(const std::string &path, std::initializer_list<unsigned> cells) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (unsigned c : cells) {
        const char b[4] = {(char) (c >> 24), (char) (c >> 16), (char) (c >> 8), (char) c};
        out.write(b, 4);
    }
}

void reset() {
    std::string cmd = "rm -rf '" + kRoot + "' && mkdir -p '" + kRoot + "/soc'";
    CHECK(std::system(cmd.c_str()) == 0);
}

void testPeripheralBase() {
    reset();
    writeCells(kRoot + "/soc/ranges", {0x7E000000, 0x3F000000, 0x01000000});
    CHECK(bcm_host_get_peripheral_address() == 0x3F000000);
    CHECK(bcm_host_get_max_legacy_dma_channel() == 14);

    writeCells(kRoot + "/soc/ranges", {0x7E000000, 0x0, 0xFE000000, 0x01800000});
    CHECK(bcm_host_get_peripheral_address() == 0xFE000000);
    CHECK(bcm_host_get_max_legacy_dma_channel() == 10);
}

void testDmaMask() {
    reset();
    bool known = true;
    CHECK(bcm_host_get_kernel_dma_mask(&known) == 0 && !known);

    // Older device trees.
    mkdir((kRoot + "/soc/dma@7e007000").c_str(), 0755);
    writeCells(kRoot + "/soc/dma@7e007000/brcm,dma-channel-mask", {0x7f35});
    CHECK(bcm_host_get_kernel_dma_mask(&known) == 0x7f35 && known);

    // Current device trees.
    reset();
    mkdir((kRoot + "/soc/dma-controller@7e007000").c_str(), 0755);
    writeCells(kRoot + "/soc/dma-controller@7e007000/dma-channel-mask", {0x07f5});
    CHECK(bcm_host_get_kernel_dma_mask(&known) == 0x07f5 && known);

    // Several controllers are combined; other nodes are ignored.
    mkdir((kRoot + "/soc/dma@7e007b00").c_str(), 0755);
    writeCells(kRoot + "/soc/dma@7e007b00/brcm,dma-channel-mask", {0x3000});
    mkdir((kRoot + "/soc/gpio@7e200000").c_str(), 0755);
    writeCells(kRoot + "/soc/gpio@7e200000/dma-channel-mask", {0x1});
    CHECK(bcm_host_get_kernel_dma_mask(&known) == 0x37f5 && known);
}

} // namespace

int main() {
    testPeripheralBase();
    testDmaMask();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all device-tree tests passed\n");
    return 0;
}
