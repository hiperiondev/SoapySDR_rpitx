// Copyright (c) 2012, Broadcom Europe Ltd.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
// * Neither the name of the copyright holder nor the
// names of its contributors may be used to endorse or promote products
// derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <system_error>

#include "mailbox.hpp"
#include "util.hpp"

// Request and response codes of the property interface.
#define MBOX_PROCESS_REQUEST 0x00000000u
#define MBOX_RESPONSE_SUCCESS 0x80000000u
#define MBOX_TAG_RESPONSE 0x80000000u

static unsigned system_page_size() {
    long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? (unsigned) ps : 4096u;
}

void *mapmem(unsigned base, unsigned size) {
    int mem_fd;
    unsigned offset = base % system_page_size();
    base = base - offset;
    if ((mem_fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC)) < 0) {
        dbg_printf(0, "can't open /dev/mem: %s\n", std::generic_category().message(errno).c_str());
        return NULL;
    }
    void *mem = mmap(0, size + offset, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, base);
    int mmapErr = errno;
    close(mem_fd);
    dbg_printf(2, "base=0x%x, mem=%p\n", base, mem);
    if (mem == MAP_FAILED) {
        dbg_printf(0, "mmap error at 0x%x (size %u): %s\n", base, size, std::generic_category().message(mmapErr).c_str());
        return NULL;
    }
    return (char *) mem + offset;
}

void *unmapmem(void *addr, unsigned size) {
    if (addr == NULL)
        return NULL;

    unsigned offset = (unsigned) ((uintptr_t) addr % system_page_size());
    if (munmap((char *) addr - offset, size + offset) != 0)
        dbg_printf(0, "munmap error: %s\n", std::generic_category().message(errno).c_str());

    return NULL;
}

// Sends a property message to the VideoCore. Returns 0 only when the ioctl succeeded, the firmware
// accepted the buffer and the (single) tag carries a response.
static int mbox_property(int file_desc, unsigned *p) {
    if (ioctl(file_desc, IOCTL_MBOX_PROPERTY, p) < 0) {
        dbg_printf(0, "mailbox ioctl failed: %s\n", std::generic_category().message(errno).c_str());
        return -1;
    }

    if (dbg_getlevel() >= 2) {
        unsigned size = p[0];
        for (unsigned i = 0; i < size / 4; i++)
            dbg_printf(2, "%04zx: 0x%08x\n", i * sizeof *p, p[i]);
    }

    if (p[1] != MBOX_RESPONSE_SUCCESS || (p[4] & MBOX_TAG_RESPONSE) == 0) {
        dbg_printf(0, "mailbox tag 0x%x refused (response 0x%08x, tag 0x%08x)\n", p[2], p[1], p[4]);
        return -1;
    }
    return 0;
}

// Builds and sends a message holding one tag with @p nvalues request words; the first response word is
// returned in @p result.
static int mbox_tag(int file_desc, unsigned tag, const unsigned *values, unsigned nvalues, unsigned *result) {
    unsigned p[32];
    unsigned i = 0;
    p[i++] = 0; // total size, filled below
    p[i++] = MBOX_PROCESS_REQUEST;
    p[i++] = tag;
    p[i++] = nvalues * 4; // value buffer size
    p[i++] = nvalues * 4; // request size
    for (unsigned v = 0; v < nvalues; v++)
        p[i++] = values[v];
    p[i++] = 0; // end tag
    p[0] = i * sizeof *p;

    if (mbox_property(file_desc, p) != 0)
        return -1;
    *result = p[5];
    return 0;
}

unsigned mem_alloc(int file_desc, unsigned size, unsigned align, unsigned flags) {
    const unsigned v[3] = {size, align, flags};
    unsigned handle = 0;
    if (mbox_tag(file_desc, 0x3000c, v, 3, &handle) != 0)
        return 0;
    return handle;
}

unsigned mem_free(int file_desc, unsigned handle) {
    unsigned status = 1;
    if (mbox_tag(file_desc, 0x3000f, &handle, 1, &status) != 0)
        return 1;
    return status;
}

unsigned mem_lock(int file_desc, unsigned handle) {
    unsigned bus = 0;
    if (mbox_tag(file_desc, 0x3000d, &handle, 1, &bus) != 0)
        return 0;
    return bus;
}

unsigned mem_unlock(int file_desc, unsigned handle) {
    unsigned status = 1;
    if (mbox_tag(file_desc, 0x3000e, &handle, 1, &status) != 0)
        return 1;
    return status;
}

int mbox_open() {
    int file_desc = open(VCIO_DEVICE_FILE_NAME, O_RDWR | O_CLOEXEC);
    if (file_desc < 0) {
        int err = errno;
        dbg_printf(0, "can't open " VCIO_DEVICE_FILE_NAME ": %s\n", std::generic_category().message(err).c_str());
        errno = err;
        return -1;
    }
    dbg_printf(1, "Using mbox device " VCIO_DEVICE_FILE_NAME ".\n");
    return file_desc;
}

void mbox_close(int file_desc) {
    if (file_desc >= 0)
        close(file_desc);
}
