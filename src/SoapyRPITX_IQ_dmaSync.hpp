/**
 * @file SoapyRPITX_IQ_dmaSync.hpp
 * @brief IQ transmitter on GPIO4: sample FIFO, real-time feeder thread and the librpitx DMA ring that drives
 *        the PLLC fractional multiplier (frequency), the pad drive strength (amplitude) and the GPIO4
 *        function (carrier on/off).
 *
 * @copyright Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com)
 *
 * Project Site: https://github.com/hiperiondev/SoapySDR_rpitx
 *
 * This is based on other projects:
 * - librpitx (https://github.com/F5OEO/librpitx), Copyright (C) 2018 Evariste COURJAUD F5OEO
 * - SoapyPlutoSDR (https://github.com/pothosware/SoapyPlutoSDR)
 *
 * please contact their authors for more information.
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

#include <stdint.h>

#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "dma.hpp"
#include "gpio.hpp"
#include "dsp.hpp"
#include "SoapyRPITX_Buffers.hpp"
#include "SoapyRPITX_Session.hpp"

/** @brief GPIO used for the RF output (GPCLK0, function ALT0). */
#define RPITX_TX_GPIO 4

/** @brief Lowest supported IQ sample rate in Hz. */
#define RPITX_MIN_SAMPLE_RATE 10000u
/** @brief Highest supported IQ sample rate in Hz. */
#define RPITX_MAX_SAMPLE_RATE 200000u
/** @brief Smallest DMA ring in samples. */
#define RPITX_MIN_RING_SIZE 512u
/** @brief Largest DMA ring in samples (each slot uses 140 bytes of VideoCore memory). */
#define RPITX_MAX_RING_SIZE 32768u
/** @brief Shortest DMA ring duration in milliseconds at the configured sample rate. */
#define RPITX_MIN_RING_MS 10u
/** @brief Duration of the default DMA ring in milliseconds (the default ring has at least 4096 slots). */
#define RPITX_DEFAULT_RING_MS 85u

/**
 * @brief Returns the default DMA ring size for a sample rate.
 * @param sampleRate Sample rate in Hz.
 * @return @ref RPITX_DEFAULT_RING_MS of samples rounded up to a multiple of 512, between 4096 and
 *         @ref RPITX_MAX_RING_SIZE.
 */
uint32_t SoapyRPITX_DefaultRingSize(uint32_t sampleRate);

/**
 * @brief Tells whether a DMA ring is long enough for a sample rate.
 * @param ringSize   Ring size in samples.
 * @param sampleRate Sample rate in Hz.
 * @return True when the ring holds at least @ref RPITX_MIN_RING_MS of samples.
 */
bool SoapyRPITX_RingFitsRate(uint32_t ringSize, uint32_t sampleRate);

/**
 * @brief Frequency-related transmitter parameters; changing any of them reprograms the PLL.
 */
struct SoapyRPITX_TuneConfig {
    double frequency = 144.0e6;  ///< Output frequency in Hz.
    uint32_t sampleRate = 48000; ///< IQ sample rate in Hz (DMA pacing rate).
    double ppm = 0.0;            ///< Manual crystal correction in ppm, added to the NTP estimate (positive when the crystal runs slow).
    bool ppmFromNtp = true;      ///< Use the kernel NTP frequency estimate as the base correction.
    int harmonic = 1;            ///< Harmonic of the clock used for transmission (the PLL runs at frequency / harmonic).
};

/**
 * @brief Full transmitter configuration.
 */
struct SoapyRPITX_TxConfig {
    SoapyRPITX_TuneConfig tune;    ///< Frequency-related parameters.
    uint32_t ringSize = 4096;      ///< DMA ring size in samples.
    uint32_t fifoMs = 250;         ///< Capacity of the software FIFO in milliseconds at the current sample rate.
    uint32_t prerollMs = 100;      ///< Amount of queued signal required before the DMA starts, in milliseconds.
    int dmaChannel = -1;           ///< DMA channel, or -1 for the first free channel of the board.
    bool dmaForce = false;         ///< Reset an explicitly selected DMA channel even when it is active.
    int driveLevel = 7;            ///< Maximum pad drive level 0-7 (2 mA to 16 mA).
    float iqScale = 1.0f;          ///< Gain applied to the IQ magnitude before amplitude quantisation.
    bool constantEnvelope = false; ///< Ignore the IQ magnitude: carrier at full drive whenever a sample is present.
    int rtPriority = 50;           ///< SCHED_FIFO priority of the feeder thread (0 keeps the default scheduler).
    int cpu = -1;                  ///< CPU the feeder thread is pinned to, or -1 for no affinity.
    bool signalCleanup = false;    ///< Turn the carrier off when the process is killed by a signal with the default action.
};

/**
 * @brief Snapshot of the transmitter counters.
 */
struct SoapyRPITX_TxStats {
    uint64_t underflows = 0;  ///< Times the FIFO ran empty in the middle of a burst (host too slow).
    uint64_t late = 0;        ///< Times the DMA engine overtook the feeder (feeder thread stalled).
    uint64_t dmaErrors = 0;   ///< Times the DMA engine stopped by itself and was restarted.
    uint64_t samplesSent = 0; ///< IQ samples written to the DMA ring.
    size_t fifoLevel = 0;     ///< Samples waiting in the FIFO.
    size_t fifoCapacity = 0;  ///< FIFO capacity in samples.
    bool dmaRunning = false;  ///< True while the DMA engine is running.
};

/**
 * @brief Claims the transmitter hardware and records the pad configuration of GPIO 0-27 before librpitx
 *        changes anything.
 *
 * The claim is exclusive within the process and, through SoapyRPITX_Session, across processes; hardware
 * left running by a terminated process is recovered first. The RF output GPIO must not be claimed by a
 * kernel driver and the PCM/I2S controller must not be enabled in the device tree.
 *
 * Declared as the first base of SoapyRPITX_IQ_dmaSync so that this happens before any register is touched
 * and is released after everything else.
 */
class SoapyRPITX_HardwareClaim {
protected:
    /**
     * @throws std::runtime_error when another transmitter object exists in the process, another process owns
     *         the transmitter, the RF output GPIO or the PCM block is used by a driver, or the pad registers
     *         cannot be mapped.
     */
    SoapyRPITX_HardwareClaim();

    /** @brief Releases the claim and clears the session record. */
    ~SoapyRPITX_HardwareClaim();

    SoapyRPITX_HardwareClaim(const SoapyRPITX_HardwareClaim &) = delete;
    SoapyRPITX_HardwareClaim &operator=(const SoapyRPITX_HardwareClaim &) = delete;

    uint32_t originalPads = 0;                   ///< PADS_GPIO_0 value (drive, hysteresis, slew of GPIO 0-27) at construction.
    std::unique_ptr<SoapyRPITX_Session> session; ///< System-wide lock and crash record.
};

/**
 * @brief IQ transmitter built on librpitx.
 *
 * Data path:
 * @code
 * Write() (host thread) --> SpscFifo --> feeder thread (SCHED_FIFO, 1 ms period) --> DMA ring --> PLLC / PAD / GPFSEL0
 * @endcode
 *
 * Each ring slot holds three words written by four DMA control blocks: pad drive strength, PLLC fractional
 * multiplier, GPFSEL0 (GPIO4 ALT0 for carrier on, input for carrier off) and a dummy write to the PCM FIFO that
 * paces the chain at the sample rate. The chain is a closed loop.
 *
 * The feeder thread:
 * - tracks the absolute DMA position (DmaRingTracker) and keeps the ring filled to three quarters from the FIFO;
 * - writes idle (carrier off) slots when the FIFO is empty, so the ring never runs dry;
 * - scrubs completed slots with idle samples, so a stalled feeder results in silence, not in replayed audio;
 * - detects and counts DMA overruns and FIFO starvation, and resynchronises after an overrun;
 * - restarts the DMA engine when it stops by itself;
 * - applies frequency / sample-rate changes requested through Retune(), with the carrier off while the PLL is
 *   reprogrammed, and replays the samples that were queued in the ring.
 *
 * The frequency of each sample is quantised by the PLLC fractional multiplier; the quantisation error is
 * carried to the next sample, so the carrier phase follows the IQ phase without drift.
 *
 * The DMA starts once @ref SoapyRPITX_TxConfig::prerollMs of signal is queued, when a burst end is signalled,
 * or when queued data has been waiting for the pre-roll time.
 *
 * Thread model: Write() and EndBurst() are called from one producer thread; Start(), Stop(), Retune() and the
 * setters from the control thread; GetStats() and IsPllLocked() from any thread.
 *
 * The DMA writes the whole GPFSEL0 register, so the functions of GPIO 0-9 are sampled when Start() is called and
 * must not be changed by other software while the transmitter is active. The pad register written for the
 * amplitude is shared by GPIO 0-27: data samples set their drive strength (keeping their hysteresis), idle
 * samples and Stop() restore the configuration found at construction. The clock tree and the PCM clock are
 * restored when the object is destroyed.
 */
class SoapyRPITX_IQ_dmaSync : private SoapyRPITX_HardwareClaim, public bufferdma, public clkgpio, public pcmgpio {
public:
    /**
     * @brief Allocates the DMA ring, programs the PLL and the PCM pacing clock and fills the ring with idle samples.
     *
     * GPIO4 is not switched to the clock output until data is transmitted.
     *
     * @param cfg Transmitter configuration.
     * @throws std::runtime_error when the hardware is in use, cannot be accessed (not root), the ring is too
     *         short for the sample rate, or the frequency cannot be produced.
     */
    explicit SoapyRPITX_IQ_dmaSync(const SoapyRPITX_TxConfig &cfg);

    /** @brief Stops the transmitter, restores GPIO4, the pads and the clocks, and releases the hardware. */
    ~SoapyRPITX_IQ_dmaSync();

    /** @brief Builds the closed-loop DMA control-block chain. */
    void SetDmaAlgo();

    /**
     * @brief Starts the feeder thread (the DMA itself starts after the pre-roll).
     *
     * Clears the FIFO and the counters of the previous activation.
     */
    void Start();

    /**
     * @brief Stops the feeder thread and the DMA, and turns the carrier off.
     *
     * Samples still queued are discarded. GPIO4 is returned to its original function, or to input when the
     * original function was the clock output.
     */
    void Stop();

    /**
     * @brief Tells whether Start() has been called without a matching Stop().
     * @return True while active.
     */
    bool IsActive() const;

    /**
     * @brief Queues samples for transmission (producer thread).
     *
     * Blocks while the FIFO is full, up to @p timeoutUs.
     *
     * @param samples   Complex samples, full scale = magnitude 1.0.
     * @param n         Number of samples.
     * @param timeoutUs Maximum blocking time in microseconds (0 = do not block).
     * @return Number of samples queued (less than @p n on timeout).
     */
    size_t Write(const std::complex<float> *samples, size_t n, long timeoutUs);

    /** @brief Marks the end of the current burst: queued samples are sent, then the carrier stays off. */
    void EndBurst();

    /**
     * @brief Waits until every queued sample has been played by the DMA.
     * @param timeoutUs Maximum waiting time in microseconds.
     * @return True when drained, false on timeout or when the transmitter is not active.
     */
    bool WaitDrained(long timeoutUs);

    /**
     * @brief Changes the frequency-related parameters.
     *
     * While active the change is performed by the feeder thread: the DMA is stopped, the carrier is turned off,
     * the PLL and the pacing clock are reprogrammed and the DMA restarts with the samples that were queued in
     * the ring, followed by the FIFO.
     *
     * A request not taken by the feeder within 3 s is withdrawn; once taken, the call waits for its result.
     *
     * @param tune New parameters.
     * @return True on success; false when the frequency cannot be produced, the ring is too short for the
     *         sample rate, or the request was withdrawn (the previous configuration stays in effect).
     */
    bool Retune(const SoapyRPITX_TuneConfig &tune);

    /**
     * @brief Sets the maximum pad drive level.
     * @param level Drive level 0-7 (clamped).
     */
    void SetDriveLevel(int level);

    /**
     * @brief Sets the IQ magnitude gain.
     * @param scale Linear gain (negative values are treated as 0).
     */
    void SetIqScale(float scale);

    /**
     * @brief Selects constant-envelope or envelope-following amplitude.
     * @param constant True for constant envelope.
     */
    void SetConstantEnvelope(bool constant);

    /**
     * @brief Returns the current frequency-related parameters.
     * @return Tune configuration in effect.
     */
    SoapyRPITX_TuneConfig GetTune() const;

    /**
     * @brief Returns the counters.
     * @return Snapshot of the counters.
     */
    SoapyRPITX_TxStats GetStats();

    /**
     * @brief Returns the FIFO capacity at the current sample rate.
     * @return Capacity in samples.
     */
    size_t FifoCapacity() const;

    /**
     * @brief Returns the DMA ring size.
     * @return Ring size in samples.
     */
    uint32_t RingSize() const;

    /**
     * @brief Returns the sample rate produced by the PCM pacing clock.
     * @return Rate in Hz (differs from the nominal rate by the divider quantisation, below 1 ppm).
     */
    double GetActualSampleRate() const;

    /**
     * @brief Tells whether PLLC reports lock.
     * @return True when locked.
     */
    bool IsPllLocked();

    /**
     * @brief Stops the DMA and turns the carrier off; the feeder thread is asked to exit and given up to
     *        50 ms to do so.
     *
     * Used from the process exit handler. The object stays valid: Stop() and the destructor complete the
     * shutdown.
     */
    void EmergencyStop();

    /**
     * @brief Stops the DMA and turns the carrier off using only async-signal-safe register writes.
     *
     * Used from the signal handlers installed with @ref SoapyRPITX_TxConfig::signalCleanup.
     */
    void SignalSafeStop();

private:
    using clock = std::chrono::steady_clock;

    /**
     * @brief Applies a tune configuration to the hardware (DMA must be stopped by the caller).
     * @param tune Configuration to apply.
     * @return True on success.
     */
    bool ApplyTune(const SoapyRPITX_TuneConfig &tune);

    /** @brief Recomputes the idle slot words for the current tune configuration. */
    void ComputeIdleWords();

    /** @brief Fills every ring slot with the idle sample and resets the ring accounting. */
    void ResetRing();

    /** @brief Body of the feeder thread. */
    void FeederLoop();

    /** @brief Applies the scheduling policy and CPU affinity to the calling thread. */
    void SetupFeederThread();

    /**
     * @brief Performs a retune requested through Retune() (feeder thread).
     * @param request New parameters.
     * @return True on success.
     */
    bool FeederRetune(const SoapyRPITX_TuneConfig &request);

    /**
     * @brief Moves samples from the replay buffer and the FIFO into the ring up to the target fill.
     * @param target Target fill level in slots.
     * @return Number of slots written.
     */
    uint32_t PumpData(uint32_t target);

    /**
     * @brief Writes idle samples until the fill level reaches @p target.
     * @param target Target fill level in slots.
     */
    void PadIdle(uint32_t target);

    /** @brief Overwrites the completed slots reported by the tracker with idle samples. */
    void ScrubCompleted();

    /** @brief Restarts after the DMA engine stopped by itself (feeder thread). */
    void RecoverDma();

    /**
     * @brief Writes one IQ sample into a ring slot.
     * @param slot   Ring slot.
     * @param sample IQ sample.
     */
    void WriteDataSlot(uint32_t slot, std::complex<float> sample);

    /**
     * @brief Writes the idle (carrier off) sample into a ring slot.
     * @param slot Ring slot.
     */
    void WriteIdleSlot(uint32_t slot);

    /** @brief Starts the DMA engine at slot 0 and initialises the position tracking. */
    void StartDma();

    /** @brief Stops the DMA engine. */
    void StopDma();

    /** @brief Switches GPIO4 to input and restores the pads of GPIO 0-27 (DMA must be stopped). */
    void CarrierOff();

    /**
     * @brief Returns GPIO4 to its original function (input when it was the clock output) and restores the pad
     *        configuration of GPIO 0-27.
     */
    void RestoreGpio();

    /** @brief Stops the feeder thread and waits for it. */
    void JoinFeeder();

    /** @brief Stores the hardware owned by the object in the session record. */
    void WriteSessionRecord();

    SoapyRPITX_TuneConfig tuneCfg;     ///< Tune configuration in effect.
    SoapyRPITX_TuneConfig pendingTune; ///< Tune configuration requested from the control thread.
    uint32_t fifoMs;                   ///< FIFO capacity in milliseconds.
    uint32_t prerollMs;                ///< Pre-roll in milliseconds.
    int rtPriority;                    ///< Feeder thread SCHED_FIFO priority.
    int cpu;                           ///< Feeder thread CPU affinity.
    bool signalCleanup;                ///< Signal handlers installed by this object.

    std::atomic<int> driveLevel;        ///< Maximum pad drive level 0-7.
    std::atomic<float> iqScale;         ///< IQ magnitude gain.
    std::atomic<bool> constantEnvelope; ///< Constant-envelope amplitude mode.

    dsp mydsp;                     ///< Phase/amplitude decomposition (feeder thread only).
    double phaseCarry = 0;         ///< Phase error of the PLL quantisation carried to the next sample, in radians (feeder thread only).
    double carryToHz = 0;          ///< Conversion of @ref phaseCarry to a frequency correction (rate / 2 pi).
    uint32_t originalGpioFunction; ///< GPIO4 function select code at construction.
    uint32_t fselIdle = 0;         ///< GPFSEL0 value with GPIO4 as input.
    uint32_t fselOn = 0;           ///< GPFSEL0 value with GPIO4 as GPCLK0 (ALT0).
    uint32_t idleFrac = 0;         ///< PLLC_FRAC word of the idle sample.
    uint32_t idlePad = 0;          ///< PAD word of the idle sample (original pad configuration).
    padgpio pads;                  ///< Pad control block, used to restore @ref originalPads.

    SpscFifo<std::complex<float>> fifo;      ///< Samples queued by the producer.
    DmaRingTracker tracker;                  ///< Absolute ring positions (feeder thread only).
    std::vector<std::complex<float>> chunk;  ///< Transfer buffer between FIFO and ring (feeder thread only).
    std::vector<std::complex<float>> shadow; ///< IQ sample of every ring slot (zero for idle slots, feeder thread only).
    std::vector<std::complex<float>> replay; ///< Samples queued in the ring before a retune, sent first (feeder thread only).
    size_t replayPos = 0;                    ///< Next sample of @ref replay (feeder thread only).
    std::atomic<size_t> replayLeft{0};       ///< Samples of @ref replay not yet in the ring.

    std::thread feeder;                     ///< Feeder thread.
    std::atomic<bool> running{false};       ///< Feeder thread run flag.
    std::atomic<bool> feederExited{true};   ///< The feeder thread is not executing its loop.
    std::atomic<bool> active{false};        ///< True between Start() and Stop().
    std::atomic<bool> endBurst{false};      ///< Burst end signalled by the producer.
    std::atomic<bool> dmaRunning{false};    ///< DMA engine running (published by the feeder).
    std::atomic<uint64_t> playedPos{0};     ///< Absolute read position (published by the feeder).
    std::atomic<uint64_t> lastDataPos{0};   ///< Absolute position after the last data slot (published by the feeder).
    std::atomic<uint64_t> consumedTotal{0}; ///< FIFO samples written to the ring, on the scale of SpscFifo::pushedTotal() (published by the feeder).
    std::atomic<uint64_t> underflows{0};    ///< FIFO starvation events.
    std::atomic<uint64_t> late{0};          ///< DMA overrun events.
    std::atomic<uint64_t> dmaErrors{0};     ///< DMA restarts after the engine stopped by itself.
    std::atomic<uint64_t> samplesSent{0};   ///< Data samples written to the ring.

    mutable std::mutex tuneMutex;   ///< Protects @ref tuneCfg, @ref pendingTune and the retune handshake.
    std::condition_variable tuneCv; ///< Signals that a retune request was taken or completed.
    bool tunePending = false;       ///< A retune request is waiting for the feeder.
    uint64_t tuneRequestSeq = 0;    ///< Sequence number of the last request.
    uint64_t tuneTakenSeq = 0;      ///< Sequence number of the last request taken by the feeder.
    uint64_t tuneDoneSeq = 0;       ///< Sequence number of the last request completed by the feeder.
    bool tuneResult = false;        ///< Result of the request @ref tuneDoneSeq.

    std::mutex spaceMutex;                ///< Mutex of @ref spaceCv.
    std::condition_variable spaceCv;      ///< Signalled by the feeder after it frees FIFO space.
    std::atomic<int> waitingProducers{0}; ///< Producers blocked on a full FIFO.

    bool inBurst = false;              ///< Data has been played since the last idle period (feeder thread only).
    clock::time_point firstQueuedTime; ///< Time the first sample of the pending pre-roll was seen (feeder thread only).
    bool firstQueuedValid = false;     ///< @ref firstQueuedTime is valid (feeder thread only).
    clock::time_point lastPoll;        ///< Time of the previous DMA position update (feeder thread only).
    int badPolls = 0;                  ///< Consecutive polls without a valid DMA position (feeder thread only).
};
