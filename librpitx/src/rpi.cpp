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

#include <dirent.h>
#include <string.h>

#include <cstdio>
#include <string>

#include "rpi.hpp"

// Reads one big-endian 32-bit cell at byte @p offset of a device-tree property.
static bool get_dt_cell(const char *filename, long offset, unsigned *value) {
    bool ok = false;
    FILE *fp = fopen(filename, "rb");
    if (fp) {
        unsigned char buf[4];
        if (fseek(fp, offset, SEEK_SET) == 0 && fread(buf, 1, sizeof buf, fp) == sizeof buf) {
            *value = (unsigned) buf[0] << 24 | (unsigned) buf[1] << 16 | (unsigned) buf[2] << 8 | (unsigned) buf[3];
            ok = true;
        }
        fclose(fp);
    }
    return ok;
}

unsigned bcm_host_get_peripheral_address(void) {
    const char *ranges = RPITX_DT_ROOT "/soc/ranges";
    unsigned address = 0;
    if (!get_dt_cell(ranges, 4, &address))
        return 0;
    // Two-cell parent addresses (BCM2711): the first cell is the high word.
    if (address == 0 && !get_dt_cell(ranges, 8, &address))
        return 0;
    return address;
}

unsigned bcm_host_get_sdram_address(void) {
    unsigned address = 0;
    if (!get_dt_cell(RPITX_DT_ROOT "/axi/vc_mem/reg", 8, &address))
        return 0;
    return address;
}

uint32_t bcm_host_get_kernel_dma_mask(bool *known) {
    *known = false;
    uint32_t mask = 0;

    std::string soc = RPITX_DT_ROOT "/soc";
    DIR *dir = opendir(soc.c_str());
    if (dir == nullptr)
        return 0;

    while (struct dirent *e = readdir(dir)) {
        if (strncmp(e->d_name, "dma@", 4) != 0)
            continue;
        std::string prop = soc + "/" + e->d_name + "/brcm,dma-channel-mask";
        unsigned value = 0;
        if (get_dt_cell(prop.c_str(), 0, &value)) {
            mask |= value;
            *known = true;
        }
    }
    closedir(dir);
    return mask;
}
