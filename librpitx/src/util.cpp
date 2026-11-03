#include "util.hpp"

#include <atomic>

// Written by the control thread, read by every thread that logs.
static std::atomic<int> debug_level{0};

void dbg_setlevel(int Level) {
    debug_level.store(Level, std::memory_order_relaxed);
}

int dbg_getlevel() {
    return debug_level.load(std::memory_order_relaxed);
}

void dbg_printf(int Level, const char *fmt, ...) {
    if (Level <= debug_level.load(std::memory_order_relaxed)) {
        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
    }
}
