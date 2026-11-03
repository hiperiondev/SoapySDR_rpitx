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

#include "dma.hpp"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdexcept>
#include <string>
#include "util.hpp"
#include "rpi.hpp"
#include "mailbox.hpp"
#include <unistd.h>

#define BUS_TO_PHYS(x) ((x) & ~0xC0000000)

namespace {

// Default channels, most preferred first.
const int kPreferred2711[] = {7, 8, 9, 10, 6, 5};
const int kPreferredLegacy[] = {14, 13, 12, 11, 10, 9, 8, 7, 6, 5};

std::string hex(uint32_t v) {
    char buf[16];
    snprintf(buf, sizeof buf, "0x%x", v);
    return buf;
}

} // namespace

dma::dma(int Channel, uint32_t CBSize, uint32_t UserMemSize, bool Force, const std::function<void(int, unsigned)> &Allocated) {
    const bool bcm2711 = dma_reg.pi_is_2711;
    const int maxChannel = bcm2711 ? 10 : 14;
    bool maskKnown = false;
    const uint32_t kernelMask = bcm_host_get_kernel_dma_mask(&maskKnown);
    auto active = [this](int ch) { return (dma_reg.gpioreg[DMA_CS + ch * DMA_CHANNEL_WORDS] & DMA_CS_ACTIVE) != 0; };
    auto kernelOwned = [&](int ch) { return maskKnown && ((kernelMask >> ch) & 1u) != 0; };

    if (Channel >= 0) {
        if (Channel > maxChannel)
            throw std::runtime_error("librpitx: DMA channel " + std::to_string(Channel) + " is not usable on this SoC (legacy channels 0-" +
                                     std::to_string(maxChannel) + ")");
        if (kernelOwned(Channel))
            throw std::runtime_error("librpitx: DMA channel " + std::to_string(Channel) + " is reserved for the Linux kernel (brcm,dma-channel-mask " +
                                     hex(kernelMask) + ")");
        if (active(Channel) && !Force)
            throw std::runtime_error("librpitx: DMA channel " + std::to_string(Channel) +
                                     " is active (in use by another program); select another channel or force it");
        channel = Channel;
    } else {
        if (!maskKnown)
            throw std::runtime_error("librpitx: the kernel DMA channel mask cannot be read from the device tree; select the DMA channel explicitly");
        const int *list = bcm2711 ? kPreferred2711 : kPreferredLegacy;
        size_t count = bcm2711 ? sizeof(kPreferred2711) / sizeof(int) : sizeof(kPreferredLegacy) / sizeof(int);
        channel = -1;
        for (size_t i = 0; i < count && channel < 0; i++) {
            if (!kernelOwned(list[i]) && !active(list[i]))
                channel = list[i];
        }
        if (channel < 0)
            throw std::runtime_error("librpitx: no free DMA channel (kernel mask " + hex(kernelMask) + ")");
    }
    dbg_printf(1, "dma using channel %d (kernel mask %s%x)\n", channel, maskKnown ? "0x" : "unknown, 0x", kernelMask);
    dbg_printf(1, "channel %d CBSize %u UsermemSize %u\n", channel, CBSize, UserMemSize);

    cbsize = CBSize;
    usermemsize = UserMemSize;

    GetRpiInfo();

    mbox.handle = mbox_open();
    if (mbox.handle < 0)
        throw std::runtime_error(std::string("librpitx: cannot open the VideoCore mailbox (" VCIO_DEVICE_FILE_NAME "): ") + strerror(errno));

    // Control blocks come first: they need 32-byte alignment, which the page alignment of the block provides.
    uint32_t MemoryRequired = (uint32_t) (CBSize * sizeof(dma_cb_t) + UserMemSize * sizeof(uint32_t));
    NumPages = (int) ((MemoryRequired + VC_PAGE_SIZE - 1) / VC_PAGE_SIZE);
    dbg_printf(1, "%u Size NUM PAGES %d PAGE_SIZE %d\n", MemoryRequired, NumPages, VC_PAGE_SIZE);

    mbox.mem_ref = mem_alloc(mbox.handle, (unsigned) NumPages * VC_PAGE_SIZE, VC_PAGE_SIZE, mem_flag);
    if (mbox.mem_ref == 0) {
        mbox_close(mbox.handle);
        throw std::runtime_error("librpitx: VideoCore memory allocation failed (check gpu_mem)");
    }

    mbox.bus_addr = mem_lock(mbox.handle, mbox.mem_ref);
    if (mbox.bus_addr == 0) {
        mem_free(mbox.handle, mbox.mem_ref);
        mbox_close(mbox.handle);
        throw std::runtime_error("librpitx: VideoCore memory lock failed");
    }
    if (Allocated) {
        try {
            Allocated(channel, mbox.mem_ref);
        } catch (...) {
            mem_unlock(mbox.handle, mbox.mem_ref);
            mem_free(mbox.handle, mbox.mem_ref);
            mbox_close(mbox.handle);
            throw;
        }
    }

    mbox.virt_addr = (uint8_t *) mapmem(BUS_TO_PHYS(mbox.bus_addr), (unsigned) NumPages * VC_PAGE_SIZE);
    if (mbox.virt_addr == nullptr) {
        mem_unlock(mbox.handle, mbox.mem_ref);
        mem_free(mbox.handle, mbox.mem_ref);
        mbox_close(mbox.handle);
        throw std::runtime_error("librpitx: cannot map DMA memory through /dev/mem; root privileges are required");
    }

    virtbase = mbox.virt_addr;
    cbarray = (dma_cb_t *) virtbase;
    usermem = (uint32_t *) (virtbase + CBSize * sizeof(dma_cb_t));

    dma_reg.gpioreg[DMA_CS + channel * DMA_CHANNEL_WORDS] = BCM2708_DMA_RESET | DMA_CS_INT;
    usleep(100);
    dma_reg.gpioreg[DMA_CONBLK_AD + channel * DMA_CHANNEL_WORDS] = mem_virt_to_phys((void *) cbarray);
}

void dma::GetRpiInfo() {
    dram_phys_base = bcm_host_get_sdram_address();

    mem_flag = MEM_FLAG_HINT_PERMALOCK | MEM_FLAG_NO_INIT;
    switch (dram_phys_base) {
        case 0x40000000:
            mem_flag |= MEM_FLAG_L1_NONALLOCATING;
            break;
        case 0xC0000000:
            mem_flag |= MEM_FLAG_DIRECT;
            break;
        default:
            throw std::runtime_error("librpitx: unknown SDRAM bus alias " + hex(dram_phys_base) + " (device tree /axi/vc_mem not readable)");
    }
}

dma::~dma() {
    stop();

    unmapmem((void *) mbox.virt_addr, (unsigned) NumPages * VC_PAGE_SIZE);
    if (mem_unlock(mbox.handle, mbox.mem_ref) != 0)
        dbg_printf(0, "VideoCore memory unlock failed\n");
    if (mem_free(mbox.handle, mbox.mem_ref) != 0)
        dbg_printf(0, "VideoCore memory release failed\n");
    mbox_close(mbox.handle);
}

uint32_t dma::mem_virt_to_phys(volatile void *virt) {
    uint32_t offset = (uint32_t) ((volatile uint8_t *) virt - mbox.virt_addr);
    return mbox.bus_addr + offset;
}

int dma::GetChannel() const {
    return channel;
}

unsigned dma::GetMemoryHandle() const {
    return mbox.mem_ref;
}

int dma::start() {
    dma_reg.gpioreg[DMA_CS + channel * DMA_CHANNEL_WORDS] = BCM2708_DMA_RESET;
    usleep(100);
    dma_reg.gpioreg[DMA_CONBLK_AD + channel * DMA_CHANNEL_WORDS] = mem_virt_to_phys((void *) cbarray);
    dma_reg.gpioreg[DMA_DEBUG + channel * DMA_CHANNEL_WORDS] = 7; // clear debug error flags
    usleep(100);
    dma_reg.gpioreg[DMA_CS + channel * DMA_CHANNEL_WORDS] = DMA_CS_PRIORITY(7) | DMA_CS_PANIC_PRIORITY(7) | DMA_CS_DISDEBUG | DMA_CS_ACTIVE;
    Started = true;
    return 0;
}

int dma::stop() {
    dma_reg.gpioreg[DMA_CS + channel * DMA_CHANNEL_WORDS] = BCM2708_DMA_RESET;
    usleep(1000);
    dma_reg.gpioreg[DMA_CS + channel * DMA_CHANNEL_WORDS] = BCM2708_DMA_INT | BCM2708_DMA_END;
    usleep(100);
    dma_reg.gpioreg[DMA_CONBLK_AD + channel * DMA_CHANNEL_WORDS] = mem_virt_to_phys((void *) cbarray);
    usleep(100);
    dma_reg.gpioreg[DMA_DEBUG + channel * DMA_CHANNEL_WORDS] = 7; // clear debug error flags
    usleep(100);
    Started = false;
    return 0;
}

int dma::getcbposition() {
    uint32_t dmacb = (uint32_t) (dma_reg.gpioreg[DMA_CONBLK_AD + channel * DMA_CHANNEL_WORDS]);
    uint32_t cbbytes = cbsize * (uint32_t) sizeof(dma_cb_t);

    if (dmacb < mbox.bus_addr || dmacb >= mbox.bus_addr + cbbytes)
        return -1;
    return (int) (dmacb - mbox.bus_addr);
}

bool dma::isrunning() {
    return (GetStatus() & DMA_CS_ACTIVE) != 0;
}

uint32_t dma::GetStatus() {
    return dma_reg.gpioreg[DMA_CS + channel * DMA_CHANNEL_WORDS];
}

uint32_t dma::GetDebugStatus() {
    return dma_reg.gpioreg[DMA_DEBUG + channel * DMA_CHANNEL_WORDS];
}

bool dma::SetCB(dma_cb_t *cbp, uint32_t dma_flag, uint32_t src, uint32_t dst, uint32_t repeat) {
    cbp->info = dma_flag;
    cbp->src = src;
    cbp->dst = dst;
    cbp->length = 4 * repeat;
    cbp->stride = 0;
    cbp->next = mem_virt_to_phys(cbp + 1);
    cbp->pad[0] = 0;
    cbp->pad[1] = 0;
    return true;
}

bool dma::SetEasyCB(dma_cb_t *cbp, uint32_t index, dma_common_reg dst, uint32_t repeat) {
    uint32_t flag = BCM2708_DMA_NO_WIDE_BURSTS | BCM2708_DMA_WAIT_RESP;
    uint32_t src = mem_virt_to_phys(&usermem[index]);
    if (dst == dma_pcm)
        flag |= BCM2708_DMA_D_DREQ | BCM2708_DMA_PER_MAP(DREQ_PCM_TX);
    else if (dst == dma_pwm)
        flag |= BCM2708_DMA_D_DREQ | BCM2708_DMA_PER_MAP(DREQ_PWM);
    SetCB(cbp, flag, src, (uint32_t) dst, repeat);
    return true;
}

// ************************************** BUFFER DMA **************************************

bufferdma::bufferdma(int Channel, uint32_t tbuffersize, uint32_t tcbbysample, uint32_t tregisterbysample, bool Force,
                     const std::function<void(int, unsigned)> &Allocated) :
        dma(Channel, tbuffersize * tcbbysample, tbuffersize * tregisterbysample, Force, Allocated) {
    buffersize = tbuffersize;
    cbbysample = tcbbysample;
    registerbysample = tregisterbysample;
    dbg_printf(1, "BufferSize %u , cb %u user %u\n", buffersize, buffersize * cbbysample, buffersize * registerbysample);

    sampletab = usermem;
}

void bufferdma::SetDmaAlgo() {
}

int bufferdma::GetCurrentSample() {
    if (!Started)
        return -1;

    int CurrentCbPos = getcbposition();
    if (CurrentCbPos < 0) {
        dbg_printf(2, "DMA WEIRD STATE\n");
        return -1;
    }
    return (int) ((uint32_t) CurrentCbPos / ((uint32_t) sizeof(dma_cb_t) * cbbysample));
}
