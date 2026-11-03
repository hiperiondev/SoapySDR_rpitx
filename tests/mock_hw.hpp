/**
 * @file mock_hw.hpp
 * @brief Test doubles of the Raspberry Pi hardware used by the transmitter integration test.
 *
 * @copyright Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 */

#pragma once

#include <stdint.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

/** @brief Reset value of PADS_GPIO_0 in the simulation (8 mA drive, hysteresis and slew enabled, as on a Pi). */
constexpr uint32_t kDefaultPads = 0x1B;

/**
 * @brief Returns the simulated register block mapped at a peripheral offset.
 * @param offset Offset of the block from the peripheral base (e.g. DMA_BASE).
 * @return Register words, or nullptr when the block has not been mapped yet.
 */
volatile uint32_t *MockRegisters(unsigned offset);

/**
 * @brief Sets the DMA channel mask reported for the kernel by bcm_host_get_kernel_dma_mask().
 * @param mask  Channel bit mask.
 * @param known Whether the mask is reported as found in the device tree.
 */
void MockSetKernelDmaMask(uint32_t mask, bool known);

/** @brief Makes the next mem_alloc() fail as if the firmware refused it. */
void MockFailNextAlloc();

/**
 * @brief Allocates simulated VideoCore memory outside the code under test.
 * @param size Size in bytes.
 * @return Memory handle.
 */
unsigned MockAllocate(unsigned size);

/**
 * @brief Tells whether a simulated VideoCore handle is still allocated.
 * @param handle Memory handle.
 * @return True when allocated.
 */
bool MockAllocated(unsigned handle);

/**
 * @brief Returns the number of simulated VideoCore allocations alive.
 * @return Live allocations.
 */
size_t MockLiveAllocations();

/** @brief Content of one ring slot as executed by the simulated DMA engine. */
struct MockSlot {
    uint32_t frac = 0;      ///< PLLC fractional multiplier.
    uint32_t pad = 0;       ///< Pad drive level.
    bool hysteresis = true; ///< Input hysteresis of GPIO 0-27 enabled.
    bool carrierOn = false; ///< GPIO4 switched to the clock output.
};

/**
 * @brief Simulated DMA engine: while the channel ACTIVE bit is set it executes one ring slot per sample period
 *        by following the control-block chain, writes the GPFSEL0 word of each slot to the simulated GPIO
 *        block, and records what each slot would have output.
 */
class MockDmaEngine {
public:
    /**
     * @brief Starts the engine thread.
     * @param dmaChannel DMA channel to watch.
     * @param rate       Slots executed per second.
     */
    MockDmaEngine(int dmaChannel, uint32_t rate);

    /** @brief Stops the engine thread. */
    ~MockDmaEngine();

    /**
     * @brief Returns and clears the slots executed so far.
     * @return Executed slots in order.
     */
    std::vector<MockSlot> Take();

private:
    /** @brief Engine thread body. */
    void Run();

    int channel;                   ///< Watched DMA channel.
    uint32_t sampleRate;           ///< Slots per second.
    std::atomic<bool> stop{false}; ///< Stop request.
    std::mutex playedLock;         ///< Protects @ref played.
    std::vector<MockSlot> played;  ///< Executed slots.
    std::thread worker;            ///< Engine thread.
};
