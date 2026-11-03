/**
 * @file SoapyRPITX_Buffers.hpp
 * @brief Hardware-independent buffering primitives of the rpitx transmit path: a single-producer /
 *        single-consumer sample FIFO and the absolute-position accounting of the circular DMA ring.
 *
 * @copyright Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
 *
 * Project Site: https://github.com/hiperiondev/SoapySDR_rpitx
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING. If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @brief Bounded lock-free FIFO for exactly one producer thread and one consumer thread.
 *
 * The producer only advances the head counter and the consumer only advances the tail counter. Both counters
 * are 64-bit absolute element counts, so the fill level is always head - tail and never ambiguous.
 *
 * The storage is allocated once; the usable capacity can be lowered at any time with setLimit() (for
 * example when the sample rate changes), which only limits further pushes.
 *
 * @tparam T Element type (trivially copyable).
 */
template <typename T>
class SpscFifo {
public:
    /**
     * @brief Creates a FIFO.
     * @param storageSize Number of elements allocated (at least 1); also the initial capacity.
     */
    explicit SpscFifo(size_t storageSize) :
            buffer(storageSize ? storageSize : 1),
            cap(storageSize ? storageSize : 1),
            lim(storageSize ? storageSize : 1) {
    }

    SpscFifo(const SpscFifo &) = delete;
    SpscFifo &operator=(const SpscFifo &) = delete;

    /**
     * @brief Returns the usable capacity.
     * @return Maximum number of stored elements accepted by push().
     */
    size_t capacity() const {
        return lim.load(std::memory_order_relaxed);
    }

    /**
     * @brief Returns the number of allocated elements.
     * @return Storage size, the upper bound of setLimit().
     */
    size_t storage() const {
        return cap;
    }

    /**
     * @brief Sets the usable capacity (any thread).
     *
     * Elements already stored are kept even when they exceed the new capacity.
     *
     * @param limit New capacity, clamped to [1, storage()].
     */
    void setLimit(size_t limit) {
        lim.store(std::min(std::max<size_t>(limit, 1), cap), std::memory_order_relaxed);
    }

    /**
     * @brief Returns the number of stored elements.
     *
     * Exact when called from the producer or the consumer; a snapshot, never larger than storage(), from any
     * other thread.
     *
     * @return Fill level in elements.
     */
    size_t size() const {
        uint64_t t = tail.load(std::memory_order_acquire);
        uint64_t h = head.load(std::memory_order_acquire);
        return h > t ? (size_t) std::min<uint64_t>(h - t, cap) : 0;
    }

    /**
     * @brief Returns the number of elements that can be pushed (producer side).
     * @return Free space in elements.
     */
    size_t space() const {
        size_t used = size();
        size_t l = capacity();
        return used >= l ? 0 : l - used;
    }

    /**
     * @brief Copies up to @p n elements into the FIFO (producer side).
     * @param src Source elements.
     * @param n   Number of elements offered.
     * @return Number of elements stored (less than @p n when the FIFO is full).
     */
    size_t push(const T *src, size_t n) {
        uint64_t h = head.load(std::memory_order_relaxed);
        uint64_t t = tail.load(std::memory_order_acquire);
        size_t used = (size_t) (h - t);
        size_t l = capacity();
        size_t freeSlots = used >= l ? 0 : l - used;
        size_t m = std::min(n, freeSlots);
        for (size_t i = 0; i < m; i++)
            buffer[(size_t) ((h + i) % cap)] = src[i];
        head.store(h + m, std::memory_order_release);
        return m;
    }

    /**
     * @brief Moves up to @p n elements out of the FIFO (consumer side).
     * @param dst Destination buffer.
     * @param n   Maximum number of elements to read.
     * @return Number of elements read.
     */
    size_t pop(T *dst, size_t n) {
        uint64_t t = tail.load(std::memory_order_relaxed);
        uint64_t h = head.load(std::memory_order_acquire);
        size_t avail = (size_t) (h - t);
        size_t m = std::min(n, avail);
        for (size_t i = 0; i < m; i++)
            dst[i] = buffer[(size_t) ((t + i) % cap)];
        tail.store(t + m, std::memory_order_release);
        return m;
    }

    /**
     * @brief Returns the total number of elements ever pushed.
     * @return Absolute head counter.
     */
    uint64_t pushedTotal() const {
        return head.load(std::memory_order_acquire);
    }

    /**
     * @brief Empties the FIFO.
     *
     * Must only be called while the consumer is not running; elements pushed concurrently may remain.
     */
    void clear() {
        tail.store(head.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    std::vector<T> buffer;         ///< Element storage.
    const size_t cap;              ///< Storage size in elements.
    std::atomic<size_t> lim;       ///< Usable capacity in elements.
    std::atomic<uint64_t> head{0}; ///< Elements pushed so far (written by the producer).
    std::atomic<uint64_t> tail{0}; ///< Elements popped so far (written by the consumer).
};

/**
 * @brief Absolute-position bookkeeping for a circular DMA ring that never stops by itself.
 *
 * Slot positions are tracked as 64-bit absolute counts:
 * - @ref readPos()  is the absolute index of the slot the DMA engine is executing;
 * - @ref writePos() is the absolute index of the next slot the writer will fill;
 * - @ref fill()     = writePos - readPos is the number of written slots not yet completed, including the
 *   executing one. A value <= 0 means the engine has overtaken the writer.
 *
 * The engine only reports its position modulo the ring size. update() advances @ref readPos() by the modular
 * distance travelled since the previous update and, when the elapsed time shows that whole laps may have been
 * missed, adds them back from the expected number of consumed samples.
 *
 * Slots behind the engine can be scrubbed (overwritten with an idle sample) so that, if the writer stalls, the
 * engine plays idle samples instead of replaying the previous lap. scrubRange() returns the slots that are safe
 * to scrub: completed slots that have not yet been rewritten with newer data.
 */
class DmaRingTracker {
public:
    /**
     * @brief Creates a tracker.
     * @param ringSize Number of slots of the ring (at least 4).
     */
    explicit DmaRingTracker(uint32_t ringSize) :
            n(ringSize < 4 ? 4 : ringSize) {
    }

    /** @brief Rewinds every position to slot 0 (engine stopped, ring empty). */
    void reset() {
        rd = 0;
        wr = 0;
        scrubbed = 0;
        prevSlot = 0;
    }

    /**
     * @brief Returns the ring size.
     * @return Number of slots.
     */
    uint32_t size() const {
        return n;
    }

    /**
     * @brief Returns the absolute index of the slot being executed.
     * @return Absolute read position.
     */
    uint64_t readPos() const {
        return rd;
    }

    /**
     * @brief Returns the absolute index of the next slot to write.
     * @return Absolute write position.
     */
    uint64_t writePos() const {
        return wr;
    }

    /**
     * @brief Returns the number of written slots not yet completed (including the executing one).
     * @return writePos() - readPos(); zero or negative when the engine has overtaken the writer.
     */
    int64_t fill() const {
        return (int64_t) (wr - rd);
    }

    /**
     * @brief Returns the ring slot of an absolute position.
     * @param absPos Absolute position.
     * @return Slot index in [0, size()).
     */
    uint32_t slotOf(uint64_t absPos) const {
        return (uint32_t) (absPos % n);
    }

    /**
     * @brief Advances the read position from the slot reported by the engine.
     *
     * @param slot            Slot the engine is executing, in [0, size()).
     * @param expectedSamples Number of samples the engine is expected to have consumed since the previous
     *                        update (elapsed time multiplied by the sample rate). Only used to recover laps
     *                        that a modular distance cannot show; it must be accurate to better than half a ring.
     * @return True when the engine has reached or overtaken the write position (fill() <= 0).
     */
    bool update(uint32_t slot, double expectedSamples) {
        slot %= n;
        uint64_t delta = (uint64_t) ((slot + n - prevSlot) % n);
        if (expectedSamples > (double) n / 2) {
            double laps = std::floor((expectedSamples - (double) delta) / (double) n + 0.5);
            if (laps > 0)
                delta += (uint64_t) laps * n;
        }
        rd += delta;
        prevSlot = slot;
        return fill() <= 0;
    }

    /**
     * @brief Returns how many slots can be written to bring fill() up to @p target.
     *
     * The result never lets the writer reach the executing slot: fill() stays at most size() - 2.
     *
     * @param target Desired fill level in slots.
     * @return Number of slots to write (0 when the target is reached or the engine has overtaken the writer).
     */
    uint32_t writable(uint32_t target) const {
        int64_t limit = std::min<int64_t>((int64_t) target, (int64_t) n - 2);
        int64_t f = fill();
        if (f < 0)
            return 0;
        return f >= limit ? 0 : (uint32_t) (limit - f);
    }

    /**
     * @brief Records that @p count slots starting at writePos() have been written.
     * @param count Number of slots written.
     */
    void advanceWrite(uint32_t count) {
        wr += count;
    }

    /**
     * @brief Moves the write position just ahead of the engine after it has overtaken the writer.
     *
     * Every slot before readPos() is then considered scrubbed: the caller must have overwritten all ring slots
     * except the executing one with idle samples.
     *
     * @param guard Number of slots to leave between the executing slot and the new write position (>= 1).
     */
    void resync(uint32_t guard) {
        if (guard < 1)
            guard = 1;
        wr = rd + guard;
        if (scrubbed < rd)
            scrubbed = rd;
    }

    /**
     * @brief Returns the absolute range [begin, end) of completed slots that are safe to scrub.
     *
     * A completed slot p may be scrubbed while p + size() >= writePos(), i.e. before the writer has stored
     * newer data in the same ring slot.
     *
     * @param begin Receives the first absolute position to scrub.
     * @param end   Receives one past the last absolute position to scrub.
     */
    void scrubRange(uint64_t &begin, uint64_t &end) const {
        uint64_t lo = scrubbed;
        if (wr > n && lo < wr - n)
            lo = wr - n;
        begin = lo;
        end = rd > lo ? rd : lo;
    }

    /**
     * @brief Records that every slot before @p end has been scrubbed.
     * @param end One past the last scrubbed absolute position.
     */
    void markScrubbed(uint64_t end) {
        if (end > scrubbed)
            scrubbed = end;
    }

private:
    uint32_t n;            ///< Ring size in slots.
    uint64_t rd = 0;       ///< Absolute position of the executing slot.
    uint64_t wr = 0;       ///< Absolute position of the next slot to write.
    uint64_t scrubbed = 0; ///< Every completed slot before this absolute position has been scrubbed.
    uint32_t prevSlot = 0; ///< Engine slot reported by the previous update().
};
