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
#include <functional>
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
 * @brief Calls a function, once, when the next VideoCore allocation is mapped into the process (after it was
 *        allocated and locked).
 * @param hook Function to call (empty to cancel).
 */
void MockOnNextVcMap(std::function<void()> hook);

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

/** @brief Output of the transmitter during one frame (period of the pacing FIFO), as seen by the simulated DMA engine. */
struct MockSlot {
    uint32_t frac = 0;       ///< PLLC fractional multiplier.
    uint32_t pad = 0;        ///< Pad drive level.
    bool hysteresis = true;  ///< Input hysteresis of GPIO 0-27 enabled.
    bool carrierOn = false;  ///< GPIO4 switched to the clock output.
    bool carrier2On = false; ///< GPIO20 switched to the clock output (ALT5).
    uint32_t dac = 0;        ///< Word leaving the pacing FIFO in this frame (envelope code with an envelope DAC).
    bool dacValid = false;   ///< A word left the pacing FIFO in this frame.
};

/**
 * @brief Simulated DMA engine: while the channel ACTIVE bit is set it interprets the control-block chain,
 *        writes each block's word to the simulated register it addresses, and models the pacing FIFO (PCM or
 *        PWM, with the DREQ level programmed in the simulated registers): a DREQ-paced block waits while the
 *        FIFO holds the DREQ level, and one word leaves the FIFO per frame. One MockSlot is recorded per frame
 *        with the registers in effect and the FIFO word output.
 */
class MockDmaEngine {
public:
    /**
     * @brief Starts the engine thread.
     * @param dmaChannel DMA channel to watch.
     * @param rate       Frames per second (the slot rate of the transmitter).
     */
    MockDmaEngine(int dmaChannel, uint32_t rate);

    /** @brief Stops the engine thread. */
    ~MockDmaEngine();

    /**
     * @brief Returns and clears the frames recorded so far.
     * @return Recorded frames in order.
     */
    std::vector<MockSlot> Take();

private:
    /** @brief Engine thread body. */
    void Run();

    int channel;                   ///< Watched DMA channel.
    uint32_t sampleRate;           ///< Frames per second.
    std::atomic<bool> stop{false}; ///< Stop request.
    std::mutex playedLock;         ///< Protects @ref played.
    std::vector<MockSlot> played;  ///< Recorded frames.
    std::thread worker;            ///< Engine thread.
};
