// Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
// Project Site: https://github.com/hiperiondev/SoapySDR_rpitx
//
// This is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// Host-side replacement of librpitx/src/mailbox.cpp and librpitx/src/rpi.cpp for tests: VideoCore
// allocations and /dev/mem mappings are served from process memory, the device-tree queries return a
// BCM2835, and a simulated DMA engine plays the control-block ring of the transmitter at a fixed sample rate.

#include "mock_hw.hpp"

#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include "gpio.hpp"
#include "mailbox.hpp"
#include "rpi.hpp"

namespace {

struct Allocation {
    Allocation() = default;
    Allocation(void *m, unsigned s) :
            mem(m),
            size(s) {
    }

    void *mem = nullptr;
    unsigned size = 0;
};

std::mutex g_lock;
std::map<unsigned, Allocation> g_maps;  // physical base -> mapping
std::map<unsigned, Allocation> g_vcmem; // memory handle -> allocation
unsigned g_nextHandle = 1;
uint32_t g_kernelDmaMask = 0;
bool g_kernelDmaMaskKnown = false;
bool g_failNextAlloc = false;

constexpr unsigned kPeriBase = 0x20000000; // BCM2835 peripheral base reported without a device tree

// Simulated register blocks live as long as the process, like the real peripherals; they are released
// at exit so that leak checkers only report allocations of the code under test.
struct MapsRelease {
    ~MapsRelease() {
        std::lock_guard<std::mutex> l(g_lock);
        for (auto &m : g_maps)
            free(m.second.mem);
        g_maps.clear();
    }
} g_mapsRelease;

void *allocPages(unsigned size) {
    size_t rounded = ((size_t) size + VC_PAGE_SIZE - 1) / VC_PAGE_SIZE * VC_PAGE_SIZE;
    void *p = aligned_alloc(VC_PAGE_SIZE, rounded);
    memset(p, 0, rounded);
    return p;
}

// Reset values that keep the clock computations of librpitx away from divisions by zero.
void initClockBlock(volatile uint32_t *clk) {
    clk[PLLD_CTRL] = (1u << 12) | 26u; // PDIV 1, NDIV 26 -> 499.2 MHz
    clk[PLLD_PER] = 1;
    clk[PLLC_CTRL] = (1u << 12) | 52u;
    clk[PLLC_PER] = 1;
    clk[CM_LOCK] = CM_LOCK_FLOCKC | CM_LOCK_FLOCKD;
}

} // namespace

extern "C" {

int mbox_open() {
    return 42;
}

void mbox_close(int) {
}

unsigned mem_alloc(int, unsigned size, unsigned, unsigned) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_failNextAlloc) {
        g_failNextAlloc = false;
        return 0;
    }
    unsigned h = g_nextHandle++;
    g_vcmem[h] = Allocation{allocPages(size), size};
    return h;
}

unsigned mem_free(int, unsigned handle) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_vcmem.find(handle);
    if (it == g_vcmem.end())
        return 1;
    free(it->second.mem);
    g_vcmem.erase(it);
    return 0;
}

unsigned mem_lock(int, unsigned handle) {
    std::lock_guard<std::mutex> l(g_lock);
    return g_vcmem.count(handle) ? 0x40000000u | (handle << 24) : 0;
}

unsigned mem_unlock(int, unsigned) {
    return 0;
}

void *mapmem(unsigned base, unsigned size) {
    std::lock_guard<std::mutex> l(g_lock);
    unsigned handle = base >> 24;
    if (base < kPeriBase && g_vcmem.count(handle))
        return g_vcmem[handle].mem;

    auto it = g_maps.find(base);
    if (it != g_maps.end())
        return it->second.mem;

    void *p = allocPages(size);
    g_maps[base] = Allocation{p, size};
    if (base == kPeriBase + CLK_BASE)
        initClockBlock((volatile uint32_t *) p);
    if (base == kPeriBase + PADS_GPIO)
        ((volatile uint32_t *) p)[PADS_GPIO_0] = kDefaultPads;
    return p;
}

void *unmapmem(void *, unsigned) {
    return NULL;
}
}

unsigned bcm_host_get_peripheral_address(void) {
    return kPeriBase;
}

unsigned bcm_host_get_sdram_address(void) {
    return 0x40000000u;
}

uint32_t bcm_host_get_kernel_dma_mask(bool *known) {
    std::lock_guard<std::mutex> l(g_lock);
    *known = g_kernelDmaMaskKnown;
    return g_kernelDmaMask;
}

void MockSetKernelDmaMask(uint32_t mask, bool known) {
    std::lock_guard<std::mutex> l(g_lock);
    g_kernelDmaMask = mask;
    g_kernelDmaMaskKnown = known;
}

void MockFailNextAlloc() {
    std::lock_guard<std::mutex> l(g_lock);
    g_failNextAlloc = true;
}

unsigned MockAllocate(unsigned size) {
    return mem_alloc(0, size, VC_PAGE_SIZE, 0);
}

bool MockAllocated(unsigned handle) {
    std::lock_guard<std::mutex> l(g_lock);
    return g_vcmem.count(handle) != 0;
}

size_t MockLiveAllocations() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_vcmem.size();
}

volatile uint32_t *MockRegisters(unsigned offset) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_maps.find(kPeriBase + offset);
    return it == g_maps.end() ? nullptr : (volatile uint32_t *) it->second.mem;
}

MockDmaEngine::MockDmaEngine(int dmaChannel, uint32_t rate) :
        channel(dmaChannel),
        sampleRate(rate) {
    worker = std::thread(&MockDmaEngine::Run, this);
}

MockDmaEngine::~MockDmaEngine() {
    stop.store(true);
    worker.join();
}

std::vector<MockSlot> MockDmaEngine::Take() {
    std::lock_guard<std::mutex> l(playedLock);
    std::vector<MockSlot> out;
    out.swap(played);
    return out;
}

void MockDmaEngine::Run() {
    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    double credit = 0;

    while (!stop.load()) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        auto now = clock::now();
        credit += std::chrono::duration<double>(now - last).count() * sampleRate;
        last = now;

        volatile uint32_t *dmaRegs = MockRegisters(DMA_BASE);
        if (dmaRegs == nullptr) {
            credit = 0;
            continue;
        }
        volatile uint32_t *cs = &dmaRegs[DMA_CS + channel * DMA_CHANNEL_WORDS];
        volatile uint32_t *cb = &dmaRegs[DMA_CONBLK_AD + channel * DMA_CHANNEL_WORDS];
        volatile uint32_t *gpioRegs = MockRegisters(GENERAL_BASE);
        if ((*cs & DMA_CS_ACTIVE) == 0) {
            credit = 0;
            continue;
        }

        uint32_t bus = *cb;
        unsigned handle = (bus >> 24) & 0x3F;
        void *mem;
        {
            std::lock_guard<std::mutex> l(g_lock);
            auto it = g_vcmem.find(handle);
            if (it == g_vcmem.end())
                continue;
            mem = it->second.mem;
        }

        // Control-block layout of the transmitter: 4 blocks of 32 bytes per slot, the user words
        // (FRAC, PAD, FSEL) read by the first three blocks.
        const uint32_t *cbs = (const uint32_t *) mem;
        while (credit >= 1 && (*cs & DMA_CS_ACTIVE)) {
            uint32_t offset = *cb - (0x40000000u | (handle << 24));
            uint32_t first = offset / 32 / 4 * 4; // first control block of the executing slot
            const uint32_t *padCb = cbs + first * 8;
            uint32_t padSrc = padCb[1] - (0x40000000u | (handle << 24));
            const uint32_t *words = (const uint32_t *) ((const uint8_t *) mem + padSrc) - 1; // PAD is word 1

            MockSlot s;
            s.frac = words[0] & 0xFFFFF;
            s.pad = words[1] & 0x7;
            s.hysteresis = (words[1] & PADS_HYST) != 0;
            s.carrierOn = ((words[2] >> 12) & 7) == fsel_alt0;
            if (gpioRegs != nullptr)
                gpioRegs[GPFSEL0] = words[2];
            {
                std::lock_guard<std::mutex> l(playedLock);
                played.push_back(s);
            }

            // Next slot: follow the "next" pointer of the last control block of the slot.
            const uint32_t *lastCb = cbs + (first + 3) * 8;
            *cb = lastCb[5];
            credit -= 1;
        }
    }
}
