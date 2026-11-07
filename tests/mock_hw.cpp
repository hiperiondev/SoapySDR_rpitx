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
#include <deque>
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
bool g_kernelDmaMaskKnown = true;
bool g_failNextAlloc = false;
std::function<void()> g_vcMapHook;
std::function<void(unsigned)> g_vcFreeHook;

constexpr unsigned kPeriBase = 0x20000000; // BCM2835 peripheral base reported without a device tree
constexpr unsigned kMaxHandle = 31;        // highest memory handle whose physical address stays below kPeriBase

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
    clk[PLLA_CTRL] = (1u << 12) | 104u; // PDIV 1, NDIV 104 -> 1996.8 MHz
    clk[PLLA_PER] = 1;
    clk[CM_LOCK] = CM_LOCK_FLOCKA | CM_LOCK_FLOCKC | CM_LOCK_FLOCKD;
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
    // Handles are recycled, as the firmware does: the bus address of handle h is 0x40000000 | (h << 24), and
    // from handle 32 on its physical address would fall in the simulated peripheral range.
    unsigned h = 0;
    for (unsigned tries = 0; tries < kMaxHandle; tries++) {
        unsigned candidate = g_nextHandle;
        g_nextHandle = g_nextHandle % kMaxHandle + 1;
        if (!g_vcmem.count(candidate)) {
            h = candidate;
            break;
        }
    }
    if (h == 0)
        return 0; // every handle in use
    g_vcmem[h] = Allocation{allocPages(size), size};
    return h;
}

unsigned mem_free(int, unsigned handle) {
    std::function<void(unsigned)> hook;
    {
        std::lock_guard<std::mutex> l(g_lock);
        hook.swap(g_vcFreeHook);
    }
    if (hook)
        hook(handle);

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
    std::unique_lock<std::mutex> l(g_lock);
    unsigned handle = base >> 24;
    if (base < kPeriBase && g_vcmem.count(handle)) {
        void *mem = g_vcmem[handle].mem;
        std::function<void()> hook;
        hook.swap(g_vcMapHook);
        l.unlock();
        if (hook)
            hook();
        return mem;
    }

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

int bcm_host_get_max_legacy_dma_channel(void) {
    return 14;
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

void MockOnNextVcMap(std::function<void()> hook) {
    std::lock_guard<std::mutex> l(g_lock);
    g_vcMapHook = std::move(hook);
}

void MockOnNextVcFree(std::function<void(unsigned)> hook) {
    std::lock_guard<std::mutex> l(g_lock);
    g_vcFreeHook = std::move(hook);
}

__attribute__((noinline)) uint32_t MockPeek(unsigned offset, unsigned index) {
    return MockRegisters(offset)[index];
}

__attribute__((noinline)) void MockPoke(unsigned offset, unsigned index, uint32_t value) {
    MockRegisters(offset)[index] = value;
}

__attribute__((noinline)) void MockPokeBits(unsigned offset, unsigned index, uint32_t clear, uint32_t set) {
    volatile uint32_t *r = MockRegisters(offset);
    r[index] = (r[index] & ~clear) | set;
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
    bool wasActive = false;
    std::deque<uint32_t> fifo;
    uint32_t frac = 0, pad = kDefaultPads, fsel0 = 0, fsel2 = 0;

    // Executes control blocks until a DREQ-paced block finds the FIFO at its DREQ level.
    auto runChain = [&](volatile uint32_t *cb) {
        for (int guard = 0; guard < 1000000; guard++) {
            uint32_t bus = *cb;
            unsigned handle = (bus >> 24) & 0x3F;
            uint8_t *mem;
            {
                std::lock_guard<std::mutex> l(g_lock);
                auto it = g_vcmem.find(handle);
                if (it == g_vcmem.end())
                    return;
                mem = (uint8_t *) it->second.mem;
            }
            const uint32_t busBase = 0x40000000u | (handle << 24);
            const volatile uint32_t *block = (const volatile uint32_t *) (mem + (bus - busBase));
            const uint32_t info = block[0], src = block[1], dst = block[2], next = block[5];
            const uint32_t value = *(const volatile uint32_t *) (mem + (src - busBase));

            if (info & BCM2708_DMA_D_DREQ) {
                uint32_t level = 1;
                if (dst == (uint32_t) dma_pcm) {
                    volatile uint32_t *pcm = MockRegisters(PCM_BASE);
                    level = pcm != nullptr ? (pcm[PCM_DREQ_A] >> 8) & 0x7F : 1;
                } else if (dst == (uint32_t) dma_pwm) {
                    volatile uint32_t *p = MockRegisters(PWM_BASE);
                    level = p != nullptr ? p[PWM_DMAC] & 0xFF : 1;
                }
                if (level == 0)
                    level = 1;
                if (fifo.size() >= level)
                    return;
                fifo.push_back(value);
            } else {
                volatile uint32_t *regs = nullptr;
                uint32_t word = 0;
                if (dst == (uint32_t) dma_pllc_frac) {
                    frac = value;
                    regs = MockRegisters(CLK_BASE);
                    word = PLLC_FRAC;
                } else if (dst == (uint32_t) dma_pad) {
                    pad = value;
                    regs = MockRegisters(PADS_GPIO);
                    word = PADS_GPIO_0;
                } else if (dst == (uint32_t) dma_fsel) {
                    fsel0 = value;
                    regs = MockRegisters(GENERAL_BASE);
                    word = GPFSEL0;
                } else if (dst == (uint32_t) dma_fsel2) {
                    fsel2 = value;
                    regs = MockRegisters(GENERAL_BASE);
                    word = GPFSEL2;
                }
                if (regs != nullptr)
                    regs[word] = value;
            }
            *cb = next;
        }
    };

    auto record = [&](uint32_t out, bool outValid) {
        MockSlot s;
        s.frac = frac & 0xFFFFF;
        s.pad = pad & 0x7;
        s.hysteresis = (pad & PADS_HYST) != 0;
        s.carrierOn = ((fsel0 >> 12) & 7) == fsel_alt0;
        s.carrier2On = (fsel2 & 7) == fsel_alt5;
        s.dac = out;
        s.dacValid = outValid;
        std::lock_guard<std::mutex> l(playedLock);
        played.push_back(s);
    };

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
        if ((*cs & DMA_CS_ACTIVE) == 0) {
            credit = 0;
            wasActive = false;
            continue;
        }

        if (!wasActive) {
            // Start: the pacing FIFO was cleared; the engine runs until the FIFO reaches its DREQ level.
            wasActive = true;
            fifo.clear();
            credit = 0;
            runChain(cb);
            record(0, false);
            continue;
        }

        while (credit >= 1 && (*cs & DMA_CS_ACTIVE)) {
            uint32_t out = 0;
            bool outValid = false;
            if (!fifo.empty()) {
                out = fifo.front();
                fifo.pop_front();
                outValid = true;
            }
            runChain(cb);
            record(out, outValid);
            credit -= 1;
        }
    }
}
