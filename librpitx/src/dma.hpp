/**
 * @file dma.hpp
 * @brief DMA channel management and circular sample-buffer DMA for librpitx.
 *
 * @copyright Copyright (C) 2018 Evariste COURJAUD F5OEO
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DEF_DMA
#define DEF_DMA

#include <stdint.h>
#include "gpio.hpp"
#include "util.hpp"

/**
 * @name VideoCore memory allocation flags
 * Flags passed to the mailbox "allocate memory" property
 * (https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface).
 * @{
 */
#define MEM_FLAG_DISCARDABLE (1 << 0)                                   ///< Buffer can be resized to 0 at any time (cached data).
#define MEM_FLAG_NORMAL (0 << 2)                                        ///< Normal allocating alias; must not be used from the ARM.
#define MEM_FLAG_DIRECT (1 << 2)                                        ///< 0xC alias, uncached.
#define MEM_FLAG_COHERENT (2 << 2)                                      ///< 0x8 alias, non-allocating in L2 but coherent.
#define MEM_FLAG_L1_NONALLOCATING (MEM_FLAG_DIRECT | MEM_FLAG_COHERENT) ///< Allocating in L2.
#define MEM_FLAG_ZERO (1 << 4)                                          ///< Initialise the buffer to all zeros.
#define MEM_FLAG_NO_INIT (1 << 5)                                       ///< Do not initialise the buffer (default is all ones).
#define MEM_FLAG_HINT_PERMALOCK (1 << 6)                                ///< Buffer is likely to stay locked for long periods.
/** @} */

/**
 * @name DMA peripheral request (DREQ) lines
 * Peripheral mapping numbers used to pace a DMA channel (BCM2835 ARM Peripherals, page 61).
 * @{
 */
#define DREQ_PCM_TX 2       ///< PCM transmit FIFO.
#define DREQ_PCM_RX 3       ///< PCM receive FIFO.
#define DREQ_SMI 4          ///< Secondary memory interface.
#define DREQ_PWM 5          ///< PWM FIFO.
#define DREQ_SPI_TX 6       ///< SPI transmit.
#define DREQ_SPI_RX 7       ///< SPI receive.
#define DREQ_SPI_SLAVE_TX 8 ///< BSC/SPI slave transmit.
#define DREQ_SPI_SLAVE_RX 9 ///< BSC/SPI slave receive.
/** @} */

/**
 * @brief One DMA channel plus a block of VideoCore memory holding its control blocks and user data.
 *
 * The memory block starts with @ref cbsize control blocks followed by @ref usermemsize 32-bit words of
 * user memory. The memory is allocated through the VideoCore mailbox, locked, and mapped into the process
 * through /dev/mem, so the object requires root privileges (or equivalent access to /dev/mem and /dev/vcio).
 *
 * Only legacy DMA channels are driven (0-14 on BCM2835/6/7, 0-10 on BCM2711, where 11-14 are DMA4 channels
 * with a different control-block format). Channels reserved for the Linux DMA engine driver by the device
 * tree ("brcm,dma-channel-mask") are never used, and a channel found active is not reset unless forced.
 */
class dma {
protected:
    /** @brief Mailbox allocation state. */
    struct {
        int handle = -1;              ///< Mailbox file descriptor returned by mbox_open().
        unsigned mem_ref = 0;         ///< VideoCore memory handle returned by mem_alloc().
        unsigned bus_addr = 0;        ///< Bus address of the block returned by mem_lock().
        uint8_t *virt_addr = nullptr; ///< Process virtual address of the block returned by mapmem().
    } mbox;

    /** @brief Hardware DMA control block (8 words, 32 bytes, 32-byte aligned). */
    typedef struct {
        uint32_t info;   ///< Transfer information (TI register).
        uint32_t src;    ///< Source bus address.
        uint32_t dst;    ///< Destination bus address.
        uint32_t length; ///< Transfer length in bytes.
        uint32_t stride; ///< 2D stride (unused, always 0).
        uint32_t next;   ///< Bus address of the next control block.
        uint32_t pad[2]; ///< Reserved, must be zero.
    } dma_cb_t;

    uint8_t *virtbase = nullptr; ///< Start of the mapped block (equals mbox.virt_addr).
    int NumPages = 0;            ///< Number of VideoCore pages (@ref VC_PAGE_SIZE) allocated and mapped.
    int channel = 0;             ///< DMA channel in use.
    dmagpio dma_reg;             ///< Mapping of the DMA controller registers.

    uint32_t mem_flag = 0;       ///< Allocation flags chosen from the SDRAM bus alias.
    uint32_t dram_phys_base = 0; ///< SDRAM bus alias reported by the device tree.

public:
    dma_cb_t *cbarray = nullptr; ///< First control block.
    uint32_t cbsize = 0;         ///< Number of control blocks.
    uint32_t *usermem = nullptr; ///< First word of user memory (placed after the control blocks).
    uint32_t usermemsize = 0;    ///< Number of 32-bit words of user memory.
    bool Started = false;        ///< True while the channel has been started by start().

    /**
     * @brief Selects and resets a DMA channel and allocates DMA memory.
     *
     * @param Channel     DMA channel to use. A negative value selects the first free channel of the board's
     *                    preference list (7, 8, 9, 10, 6, 5 on BCM2711; 14 down to 5 on the other models)
     *                    that the kernel does not own and that is not active.
     * @param CBSize      Number of control blocks to allocate.
     * @param UserMemSize Number of 32-bit user-memory words to allocate after the control blocks.
     * @param Force       Reset an explicitly selected channel even when it is active.
     *
     * @throws std::runtime_error when the channel is not usable (DMA4 channel on BCM2711, reserved for the
     *         kernel, or active and not forced), when no default channel is free, when the mailbox cannot be
     *         opened, the VideoCore memory cannot be allocated or locked, or /dev/mem cannot be mapped
     *         (typically because the process is not root).
     */
    dma(int Channel, uint32_t CBSize, uint32_t UserMemSize, bool Force = false);

    /** @brief Stops the channel, unmaps and releases the VideoCore memory and closes the mailbox. */
    ~dma();

    dma(const dma &) = delete;
    dma &operator=(const dma &) = delete;

    /**
     * @brief Converts a pointer inside the DMA memory block to its bus address.
     * @param virt Pointer inside the mapped block.
     * @return Bus address seen by the DMA engine.
     */
    uint32_t mem_virt_to_phys(volatile void *virt);

    /**
     * @brief Selects the allocation flags from the SDRAM bus alias of the running board.
     * @throws std::runtime_error when the device tree does not report the SDRAM alias.
     */
    void GetRpiInfo();

    /**
     * @brief Returns the DMA channel in use.
     * @return Channel number.
     */
    int GetChannel() const;

    /**
     * @brief Returns the VideoCore memory handle of the DMA block.
     * @return Handle returned by mem_alloc().
     */
    unsigned GetMemoryHandle() const;

    /**
     * @brief Starts the channel at the first control block.
     * @return Always 0.
     */
    int start();

    /**
     * @brief Resets the channel, rewinds it to the first control block and clears the error flags.
     * @return Always 0.
     */
    int stop();

    /**
     * @brief Returns the byte offset of the control block currently loaded by the channel.
     * @return Offset from @ref cbarray in bytes, or -1 when the channel reports an address outside the
     *         control-block array.
     */
    int getcbposition();

    /**
     * @brief Tells whether the channel ACTIVE bit is set.
     * @return True when the channel is running.
     */
    bool isrunning();

    /**
     * @brief Returns the control and status register of the channel.
     * @return DMA_CS value (DMA_CS_ACTIVE, DMA_CS_ERROR, ...).
     */
    uint32_t GetStatus();

    /**
     * @brief Returns the debug register of the channel.
     * @return DMA_DEBUG value (read, FIFO and last-transfer error flags).
     */
    uint32_t GetDebugStatus();

    /**
     * @brief Fills a control block.
     *
     * @param cbp      Control block to fill.
     * @param dma_flag Transfer information (TI) word.
     * @param src      Source bus address.
     * @param dst      Destination bus address.
     * @param repeat   Number of 32-bit words to transfer.
     * @return Always true.
     */
    bool SetCB(dma_cb_t *cbp, uint32_t dma_flag, uint32_t src, uint32_t dst, uint32_t repeat);

    /**
     * @brief Fills a control block that copies user-memory words to a peripheral register.
     *
     * Transfers to the PCM FIFO are paced by its DREQ line; the other destinations are written immediately.
     *
     * @param cbp    Control block to fill.
     * @param index  Index of the first source word in @ref usermem.
     * @param dst    Destination register.
     * @param repeat Number of 32-bit words to transfer.
     * @return Always true.
     */
    bool SetEasyCB(dma_cb_t *cbp, uint32_t index, dma_common_reg dst, uint32_t repeat);
};

/**
 * @brief Circular DMA buffer of samples, each sample made of a fixed number of control blocks and
 *        user-memory registers.
 *
 * The control-block chain is a closed loop: the DMA engine keeps cycling through the ring until it is
 * stopped. The position of the engine is read with GetCurrentSample(); the writer is responsible for the
 * accounting of written and played slots.
 */
class bufferdma : public dma {
public:
    uint32_t buffersize = 0;       ///< Number of sample slots in the ring.
    uint32_t cbbysample = 0;       ///< Number of control blocks per slot.
    uint32_t registerbysample = 0; ///< Number of user-memory words per slot.
    uint32_t *sampletab = nullptr; ///< First user-memory word of slot 0.

public:
    /**
     * @brief Allocates a ring of @p tbuffersize slots.
     *
     * @param Channel           DMA channel (negative for the board default, see dma::dma()).
     * @param tbuffersize       Number of slots.
     * @param tcbbysample       Control blocks per slot.
     * @param tregisterbysample User-memory words per slot.
     * @param Force             Reset an explicitly selected channel even when it is active.
     *
     * @throws std::runtime_error on channel or allocation failure (see dma::dma()).
     */
    bufferdma(int Channel, uint32_t tbuffersize, uint32_t tcbbysample, uint32_t tregisterbysample, bool Force = false);

    /** @brief Builds the control-block chain; the base implementation does nothing. */
    void SetDmaAlgo();

    /**
     * @brief Returns the slot the DMA engine is executing.
     * @return Slot index in [0, @ref buffersize), or -1 when the channel is stopped or reports an invalid
     *         position.
     */
    int GetCurrentSample();
};
#endif
