/**
 * @file mailbox.hpp
 * @brief VideoCore mailbox property interface and /dev/mem mapping helpers.
 *
 * @copyright Copyright (c) 2012, Broadcom Europe Ltd. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - Neither the name of the copyright holder nor the
 *   names of its contributors may be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * None of these functions terminate the process: failures are reported through their return values so
 * that a host application loading the library keeps running.
 */

#ifndef DEF_MAILBOX
#define DEF_MAILBOX

#include <linux/ioctl.h>

/** @brief Character-device major number of the VideoCore mailbox driver (used to build the ioctl code). */
#define MAJOR_NUM 100

/** @brief ioctl request that sends a property message to the VideoCore. */
#define IOCTL_MBOX_PROPERTY _IOWR(MAJOR_NUM, 0, char *)

/** @brief Mailbox device node provided by the Raspberry Pi kernel. */
#define VCIO_DEVICE_FILE_NAME "/dev/vcio"

/**
 * @brief Granularity of VideoCore memory allocations, in bytes.
 *
 * This is the allocation unit of the firmware, independent of the page size of the ARM kernel, which
 * mapmem() and unmapmem() obtain with sysconf(_SC_PAGESIZE).
 */
#define VC_PAGE_SIZE (4 * 1024)

extern "C" {

/**
 * @brief Opens the VideoCore mailbox device (@ref VCIO_DEVICE_FILE_NAME) for reading and writing.
 * @return File descriptor, or -1 on failure (errno is set by open()).
 */
int mbox_open();

/**
 * @brief Closes a descriptor returned by mbox_open().
 * @param file_desc Descriptor; negative values are ignored.
 */
void mbox_close(int file_desc);

/**
 * @brief Allocates contiguous VideoCore memory.
 * @param file_desc Mailbox descriptor.
 * @param size      Size in bytes.
 * @param align     Alignment in bytes.
 * @param flags     MEM_FLAG_* allocation flags.
 * @return Memory handle, or 0 when the request failed or was refused by the firmware.
 */
unsigned mem_alloc(int file_desc, unsigned size, unsigned align, unsigned flags);

/**
 * @brief Releases memory allocated by mem_alloc().
 *
 * VideoCore handles are global to the firmware: a handle allocated by another process can be released too.
 *
 * @param file_desc Mailbox descriptor.
 * @param handle    Memory handle.
 * @return 0 on success, non-zero on failure.
 */
unsigned mem_free(int file_desc, unsigned handle);

/**
 * @brief Locks allocated memory in place and returns its bus address.
 * @param file_desc Mailbox descriptor.
 * @param handle    Memory handle.
 * @return Bus address, or 0 on failure.
 */
unsigned mem_lock(int file_desc, unsigned handle);

/**
 * @brief Unlocks memory locked by mem_lock().
 * @param file_desc Mailbox descriptor.
 * @param handle    Memory handle.
 * @return 0 on success, non-zero on failure.
 */
unsigned mem_unlock(int file_desc, unsigned handle);

/**
 * @brief Maps physical memory or registers through /dev/mem.
 *
 * Unaligned bases are supported: the returned pointer addresses @p base itself.
 *
 * @param base Physical address (32-bit; peripherals in the BCM2711 high-peripheral mode are not reachable).
 * @param size Size in bytes.
 * @return Mapped address, or NULL on failure.
 */
void *mapmem(unsigned base, unsigned size);

/**
 * @brief Unmaps memory mapped by mapmem().
 * @param addr Address returned by mapmem(); NULL is ignored.
 * @param size Size passed to mapmem().
 * @return NULL.
 */
void *unmapmem(void *addr, unsigned size);
}

#endif
