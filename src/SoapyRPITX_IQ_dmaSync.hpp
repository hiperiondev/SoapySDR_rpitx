/**
 * @file SoapyRPITX_IQ_dmaSync.hpp
 * @brief IQ transmitter on GPIO4: sample FIFO, real-time feeder thread and the librpitx DMA ring that drives
 *        the PLLC fractional multiplier (frequency), the pad drive strength (amplitude), the GPIO4 function
 *        (carrier on/off) and, optionally, a second clock pin (GPIO20) and an envelope DAC (PCM/I2S or PWM)
 *        for Envelope Elimination and Restoration.
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
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "dma.hpp"
#include "gpio.hpp"
#include "dsp.hpp"
#include "SoapyRPITX_Amplitude.hpp"
#include "SoapyRPITX_Buffers.hpp"
#include "SoapyRPITX_Session.hpp"

/** @brief GPIO used for the RF output (GPCLK0, function ALT0). */
#define RPITX_TX_GPIO 4
/** @brief GPIO used as the second clock output of the RF-DAC option (GPCLK0, function ALT5). */
#define RPITX_TX2_GPIO 20

/** @brief Lowest supported IQ sample rate in Hz. */
#define RPITX_MIN_SAMPLE_RATE 10000u
/** @brief Highest supported slot rate (IQ sample rate times slots per sample) in Hz. */
#define RPITX_MAX_SAMPLE_RATE 200000u
/** @brief Largest number of DMA slots per IQ sample. */
#define RPITX_MAX_SLOTS_PER_SAMPLE 4u
/** @brief Smallest DMA ring in slots. */
#define RPITX_MIN_RING_SIZE 512u
/** @brief Largest DMA ring in slots (each slot uses 140 to 180 bytes of VideoCore memory). */
#define RPITX_MAX_RING_SIZE 32768u
/** @brief Shortest DMA ring duration in milliseconds at the configured slot rate. */
#define RPITX_MIN_RING_MS 10u
/** @brief Duration of the default DMA ring in milliseconds (the default ring has at least 4096 slots). */
#define RPITX_DEFAULT_RING_MS 85u

/**
 * @brief Returns the default DMA ring size for a slot rate.
 * @param slotRate Slot rate in Hz (IQ sample rate times slots per sample).
 * @return @ref RPITX_DEFAULT_RING_MS of slots rounded up to a multiple of 512, between 4096 and
 *         @ref RPITX_MAX_RING_SIZE.
 */
uint32_t SoapyRPITX_DefaultRingSize(uint32_t slotRate);

/**
 * @brief Tells whether a DMA ring is long enough for a slot rate.
 * @param ringSize Ring size in slots.
 * @param slotRate Slot rate in Hz.
 * @return True when the ring holds at least @ref RPITX_MIN_RING_MS of slots.
 */
bool SoapyRPITX_RingFitsRate(uint32_t ringSize, uint32_t slotRate);

/**
 * @brief Frequency-related transmitter parameters; changing any of them reprograms the PLL.
 */
struct SoapyRPITX_TuneConfig {
    double frequency = 144.0e6;  ///< Output frequency in Hz.
    uint32_t sampleRate = 48000; ///< IQ sample rate in Hz.
    double ppm = 0.0;            ///< Manual crystal correction in ppm, added to the NTP estimate (positive when the crystal runs slow).
    bool ppmFromNtp = true;      ///< Use the kernel NTP frequency estimate as the base correction.
    int harmonic = 1;            ///< Harmonic of the clock used for transmission (the PLL runs at frequency / harmonic).
};

/** @brief Output of the envelope for Envelope Elimination and Restoration. */
enum class SoapyRPITX_EnvelopeOutput {
    None, ///< The envelope drives the pad strength (and the second clock pin) of the RF output.
    Pcm,  ///< The envelope is a 16-bit I2S stream on GPIO18 (BCLK), GPIO19 (LRCLK) and GPIO21 (DOUT).
    Pwm,  ///< The envelope is the duty of PWM channel 1 on GPIO18 (ALT5) or GPIO12 (ALT0).
};

/**
 * @brief Full transmitter configuration.
 */
struct SoapyRPITX_TxConfig {
    SoapyRPITX_TuneConfig tune;    ///< Frequency-related parameters.
    uint32_t ringSize = 4096;      ///< DMA ring size in slots.
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
    uint32_t slotsPerSample = 1;   ///< DMA slots per IQ sample (1-4); the IQ signal is linearly interpolated between them.
    SoapyRPITX_EnvelopeOutput envelopeOutput = SoapyRPITX_EnvelopeOutput::None; ///< Envelope output.
    int pwmGpio = 18;                                                           ///< PWM envelope pin: 18 (ALT5) or 12 (ALT0).
    int pwmBits = 8;                                                            ///< PWM envelope resolution in bits (6-12): the period is 2^bits PWM clocks.
    bool rfDac = false;                   ///< GPIO20 is a second clock output, summed with GPIO4 by an external weighted network.
    SoapyRPITX_AmplitudeConfig amplitude; ///< Amplitude calibration and processing.
};

/**
 * @brief Checks the parts of a configuration that do not depend on the hardware.
 * @param cfg Configuration.
 * @throws std::runtime_error with the reason when the configuration is invalid.
 */
void SoapyRPITX_CheckTxConfig(const SoapyRPITX_TxConfig &cfg);

/**
 * @brief Snapshot of the transmitter counters.
 */
struct SoapyRPITX_TxStats {
    uint64_t underflows = 0;    ///< Times the FIFO ran empty in the middle of a burst (host too slow).
    uint64_t late = 0;          ///< Times the DMA engine overtook the feeder (feeder thread stalled).
    uint64_t dmaErrors = 0;     ///< Times the DMA engine stopped by itself and was restarted.
    uint64_t samplesSent = 0;   ///< IQ samples taken from the FIFO into the DMA ring (samples replayed after a retune are counted once).
    uint64_t envelopeClips = 0; ///< Slots whose envelope exceeded full scale and was limited.
    uint64_t envelopeLate = 0;  ///< Envelope words that could not be written ahead in time (ring nearly empty).
    size_t fifoLevel = 0;       ///< Samples waiting in the FIFO.
    size_t fifoCapacity = 0;    ///< FIFO capacity in samples.
    bool dmaRunning = false;    ///< True while the DMA engine is running.
};

/**
 * @brief Claims the transmitter hardware and records the pad configuration of GPIO 0-27 and the functions of
 *        the GPIOs used by the transmitter before librpitx changes anything.
 *
 * The claim is exclusive within the process and, through SoapyRPITX_Session, across processes; hardware
 * left running by a terminated process is recovered first. The GPIOs used must not be claimed by a kernel
 * driver, the PCM/I2S controller must not be enabled in the device tree unless the PWM envelope output is
 * used, and the PWM block must not be enabled (PWM driver or analogue audio) when it is used.
 *
 * Declared as the first base of SoapyRPITX_IQ_dmaSync so that this happens before any register is touched
 * and is released after everything else.
 */
class SoapyRPITX_HardwareClaim {
protected:
    /**
     * @param cfg Transmitter configuration (checked with SoapyRPITX_CheckTxConfig()).
     * @throws std::runtime_error when the configuration is invalid, another transmitter object exists in the
     *         process, another process owns the transmitter, a GPIO or the PCM / PWM block is used by a driver,
     *         or the registers cannot be mapped.
     */
    explicit SoapyRPITX_HardwareClaim(const SoapyRPITX_TxConfig &cfg);

    /** @brief Releases the claim and clears the session record. */
    ~SoapyRPITX_HardwareClaim();

    SoapyRPITX_HardwareClaim(const SoapyRPITX_HardwareClaim &) = delete;
    SoapyRPITX_HardwareClaim &operator=(const SoapyRPITX_HardwareClaim &) = delete;

    /**
     * @brief Records the DMA channel and the VideoCore memory as soon as they are allocated, and clears them
     *        (channel -1, handle 0) before they are released.
     * @param channel   DMA channel, or -1 when none is owned.
     * @param memHandle VideoCore memory handle, or 0 when none is owned.
     */
    void RecordDmaAllocation(int channel, unsigned memHandle);

    /**
     * @brief Returns the allocation and release callback passed to the DMA object.
     * @return Callback that forwards to RecordDmaAllocation() of this claim.
     */
    std::function<void(int, unsigned)> AllocationRecorder();

    /** @brief Writes @ref record to the session lock file. */
    void PublishRecord();

    uint32_t originalPads = 0;                               ///< PADS_GPIO_0 value (drive, hysteresis, slew of GPIO 0-27) at construction.
    uint32_t originalGpioFunction = 0;                       ///< GPIO4 function select code at construction.
    std::vector<std::pair<unsigned, uint32_t>> originalPins; ///< Other GPIOs used and their function at construction.
    SoapyRPITX_SessionRecord record;                         ///< Hardware owned so far, as stored in the lock file.
    std::unique_ptr<SoapyRPITX_Session> session;             ///< System-wide lock and crash record.
};

/**
 * @brief IQ transmitter built on librpitx.
 *
 * Data path:
 * @code
 * Write() (host thread) --> SpscFifo --> feeder thread (SCHED_FIFO, 1 ms period) --> DMA ring --> PLLC / PAD / GPFSEL0
 *                                                                                              [/ GPFSEL2] [/ DAC FIFO]
 * @endcode
 *
 * Each ring slot holds the words written by its DMA control blocks: pad drive strength, PLLC fractional
 * multiplier, GPFSEL0 (GPIO4 ALT0 for carrier on, input for carrier off), GPFSEL2 with the RF-DAC option
 * (GPIO20 ALT5 / input) and finally a write to the PCM FIFO or to the PWM FIFO that paces the chain at the slot
 * rate. Without an envelope DAC the pacing write carries a dummy word; with one it carries the envelope code.
 * The chain is a closed loop.
 *
 * With slots per sample K > 1 every IQ sample is linearly interpolated into K slots, so the amplitude
 * quantiser runs K times faster than the IQ signal.
 *
 * Amplitude: in pad mode (no envelope DAC) the delayed envelope selects one of the states of a
 * SoapyRPITX_AmplitudeModel (drive level and clock pins, or carrier off), nearest or with sigma-delta error
 * feedback. In EER mode the clock pins run at the fixed EER drive for every data slot and the delayed
 * envelope is mapped to the DAC code. AM/PM corrections are added to the carrier phase. The envelope is
 * delayed relative to the phase by @ref SoapyRPITX_AmplitudeConfig::envelopeDelay; the latency of the DAC FIFO
 * is compensated by writing the envelope words of a sample into an earlier slot.
 *
 * The feeder thread:
 * - tracks the absolute DMA position (DmaRingTracker) and keeps the ring filled to three quarters from the FIFO;
 * - writes idle (carrier off) slots when the FIFO is empty, so the ring never runs dry;
 * - scrubs completed slots with idle samples, so a stalled feeder results in silence, not in replayed audio;
 * - detects and counts DMA overruns and FIFO starvation, and resynchronises after an overrun;
 * - restarts the DMA engine when it stops by itself;
 * - applies frequency / sample-rate changes requested through Retune(), with the carrier off while the PLL is
 *   reprogrammed, and replays the data samples that were queued in the ring: idle slots before the first data
 *   slot are dropped, idle slots between data slots (FIFO starvation inside a burst) are replayed as idle;
 * - decides when every queued sample has been played and publishes it for WaitDrained().
 *
 * Every DMA start begins with idle slots that fill the pacing FIFO up to its DREQ level, so no data slot is
 * executed faster than the slot rate.
 *
 * The frequency of each slot is quantised by the PLLC fractional multiplier; the quantisation error is
 * carried to the next slots, so the carrier phase follows the IQ phase without drift.
 *
 * The DMA starts once @ref SoapyRPITX_TxConfig::prerollMs of signal is queued, when a burst end is signalled,
 * or when queued data has been waiting for the pre-roll time.
 *
 * Thread model: Write() and EndBurst() are called from one producer thread; Start(), Stop(), Retune() and the
 * setters from the control thread; GetStats() and IsPllLocked() from any thread.
 *
 * The DMA writes the whole GPFSEL0 register (and GPFSEL2 with the RF-DAC option), so the functions of GPIO
 * 0-9 (and 20-29) are sampled when Start() is called and must not be changed by other software while the
 * transmitter is active. The pad register written for the amplitude is shared by GPIO 0-27: data samples set
 * their drive strength (keeping their hysteresis), idle samples and Stop() restore the configuration found
 * at construction. The clock tree, the PCM clock, the PWM block and the envelope pins are restored when the
 * object is destroyed.
 */
class SoapyRPITX_IQ_dmaSync : private SoapyRPITX_HardwareClaim, public bufferdma, public clkgpio, public pcmgpio {
public:
    /**
     * @brief Allocates the DMA ring, programs the PLL and the pacing clock and fills the ring with idle samples.
     *
     * GPIO4 is not switched to the clock output until data is transmitted. The envelope DAC pins are switched
     * to their PCM / PWM function here.
     *
     * @param cfg Transmitter configuration.
     * @throws std::runtime_error when the configuration is invalid, the hardware is in use, cannot be accessed
     *         (not root), the ring is too short for the slot rate, or the frequency or the DAC rate cannot be
     *         produced.
     */
    explicit SoapyRPITX_IQ_dmaSync(const SoapyRPITX_TxConfig &cfg);

    /** @brief Stops the transmitter, restores the GPIOs, the pads and the clocks, and releases the hardware. */
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
     * @brief Stops the feeder thread and the DMA, and turns the carrier and the envelope output off.
     *
     * Samples still queued are discarded. GPIO4 (and GPIO20) are returned to their original function, or to
     * input when the original function was the clock output.
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
     *
     * The feeder thread evaluates the drain condition (FIFO consumed, replay buffer empty, last data slot
     * completed) in one place and publishes the FIFO push count it applies to, so a retune running meanwhile
     * cannot make the queued samples appear played.
     *
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
     * @return True on success; false when the frequency or the DAC rate cannot be produced, the slot rate is out
     *         of range, the ring is too short for the slot rate, or the request was withdrawn (the previous
     *         configuration stays in effect).
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
     * @brief Replaces the amplitude calibration and processing parameters (applied from the next feeder period).
     * @param amplitude New parameters.
     * @return False when the parameters are invalid (nothing is changed).
     */
    bool SetAmplitudeConfig(const SoapyRPITX_AmplitudeConfig &amplitude);

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
     * @return Ring size in slots.
     */
    uint32_t RingSize() const;

    /**
     * @brief Returns the number of DMA slots per IQ sample.
     * @return Slots per sample (1-4).
     */
    uint32_t SlotsPerSample() const;

    /**
     * @brief Returns the IQ sample rate produced by the pacing clock (any thread).
     * @return Rate in Hz (differs from the nominal rate by the divider quantisation), published by the last
     *         successful frequency configuration.
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
     * @brief Stops the DMA, turns the carrier and the envelope output off using only async-signal-safe
     *        register writes.
     *
     * Used from the signal handlers installed with @ref SoapyRPITX_TxConfig::signalCleanup.
     */
    void SignalSafeStop();

private:
    using clock = std::chrono::steady_clock;

    /** @brief Word indexes of a ring slot. */
    enum : uint32_t {
        kWordFrac = 0,  ///< PLLC_FRAC.
        kWordPad = 1,   ///< PADS_GPIO_0.
        kWordFsel0 = 2, ///< GPFSEL0.
    };

    /**
     * @brief Returns the number of user words of a slot for a configuration.
     * @param cfg Configuration.
     * @return 3 to 5 words.
     */
    static uint32_t WordsPerSlot(const SoapyRPITX_TxConfig &cfg);

    /**
     * @brief Returns the number of control blocks of a slot for a configuration.
     * @param cfg Configuration.
     * @return 4 or 5 control blocks.
     */
    static uint32_t CbsPerSlot(const SoapyRPITX_TxConfig &cfg);

    /**
     * @brief Applies a tune configuration to the hardware (DMA must be stopped by the caller).
     *
     * The PLL divider, the pacing clock and the envelope DAC clock are checked before anything is
     * reprogrammed; on such a refusal the crystal corrections of the carrier, PCM and PWM clocks are restored
     * and the previous configuration stays in effect.
     *
     * @param tune Configuration to apply.
     * @return True on success.
     */
    bool ApplyTune(const SoapyRPITX_TuneConfig &tune);

    /** @brief Recomputes the idle slot words for the current tune configuration. */
    void ComputeIdleWords();

    /** @brief Fills every ring slot with the idle sample and resets the ring accounting. */
    void ResetRing();

    /** @brief Builds and publishes the amplitude model from the configuration and the drive level. */
    void PublishModel();

    /** @brief Takes the published amplitude model and recomputes the delay split (feeder thread). */
    void RefreshModel();

    /** @brief Clears the amplitude processing state (start of a burst). */
    void ResetAmplitudeState();

    /** @brief Body of the feeder thread. */
    void FeederLoop();

    /** @brief Applies the scheduling policy and CPU affinity to the calling thread. */
    void SetupFeederThread();

    /**
     * @brief Performs a retune requested through Retune() or a DMA restart (feeder thread).
     *
     * The data slots between the executing slot and the last data slot are kept for replay. With a running
     * engine the position is taken from the engine; with an engine that stopped by itself no lap can have
     * been completed since the last poll, so only the modular distance is used. When the engine reports no
     * valid position the last confirmed position is used, and up to @ref kBadPollLimit feeder periods of
     * samples may then be sent twice.
     *
     * @param request       New parameters.
     * @param engineStopped The DMA engine stopped by itself (restart after an error).
     * @return True on success.
     */
    bool FeederRetune(const SoapyRPITX_TuneConfig &request, bool engineStopped);

    /** @brief Evaluates the drain condition and publishes it for WaitDrained() (feeder thread). */
    void PublishDrainState();

    /**
     * @brief Moves samples from the replay buffer and the FIFO into the ring up to the target fill.
     * @param target Target fill level in slots.
     * @return Number of slots written.
     */
    uint32_t PumpData(uint32_t target);

    /**
     * @brief Writes idle samples until the fill level reaches @p target.
     * @param target Target fill level in slots.
     * @return Number of slots written.
     */
    uint32_t PadIdle(uint32_t target);

    /** @brief Overwrites the completed slots reported by the tracker with idle samples. */
    void ScrubCompleted();

    /** @brief Consecutive polls without a valid DMA position before the engine is considered stopped. */
    static constexpr int kBadPollLimit = 5;

    /** @brief Value of @ref drainedPush while the transmitter is not known to be drained. */
    static constexpr uint64_t kNotDrained = UINT64_MAX;

    /** @brief Restarts after the DMA engine stopped by itself (feeder thread). */
    void RecoverDma();

    /**
     * @brief Computes the absolute position below which envelope words can no longer be written (the DMA may
     *        already have executed those slots).
     */
    void UpdateBehindLimit();

    /**
     * @brief Writes one slot sample into the ring at an absolute position.
     * @param pos    Absolute slot position.
     * @param sample Slot sample (IQ, interpolated when slots per sample > 1).
     */
    void WriteDataSlot(uint64_t pos, std::complex<float> sample);

    /**
     * @brief Writes the idle (carrier off) sample into a ring slot.
     * @param slot Ring slot.
     */
    void WriteIdleSlot(uint32_t slot);

    /** @brief Starts the pacing FIFO and the DMA engine at slot 0 and initialises the position tracking. */
    void StartDma();

    /** @brief Stops the DMA engine and the pacing FIFO (the envelope output goes silent). */
    void StopDma();

    /** @brief Switches the clock pins to input and restores the pads of GPIO 0-27 (DMA must be stopped). */
    void CarrierOff();

    /**
     * @brief Returns the clock pins to their original function (input when it was the clock output) and
     *        restores the pad configuration of GPIO 0-27.
     */
    void RestoreGpio();

    /** @brief Returns the envelope DAC pins to their original function. */
    void RestoreEnvelopePins();

    /** @brief Stops the feeder thread and waits for it. */
    void JoinFeeder();

    /** @brief Updates the record with the saved clocks and the GPIOs and writes it to the session lock file. */
    void WriteSessionRecord();

    SoapyRPITX_TuneConfig tuneCfg;                  ///< Tune configuration in effect.
    SoapyRPITX_TuneConfig pendingTune;              ///< Tune configuration requested from the control thread.
    uint32_t fifoMs;                                ///< FIFO capacity in milliseconds.
    uint32_t prerollMs;                             ///< Pre-roll in milliseconds.
    int rtPriority;                                 ///< Feeder thread SCHED_FIFO priority.
    int cpu;                                        ///< Feeder thread CPU affinity.
    bool signalCleanup;                             ///< Signal handlers installed by this object.
    const uint32_t slotsPerSample;                  ///< DMA slots per IQ sample.
    const SoapyRPITX_EnvelopeOutput envelopeOutput; ///< Envelope output.
    const int pwmGpio;                              ///< PWM envelope pin.
    const uint32_t pwmRange;                        ///< PWM period in PWM clocks (full-scale code).
    const bool rfDac;                               ///< GPIO20 is the second clock output.
    const uint32_t wordsPerSlot;                    ///< User words per slot.
    const int wordFsel2;                            ///< Index of the GPFSEL2 word, or -1.
    const int wordEnv;                              ///< Index of the envelope word, or -1.
    const uint32_t pacingLevel;                     ///< Words kept in the pacing FIFO (its DREQ level).
    uint32_t slotRate = 0;                          ///< Slot rate in Hz (IQ sample rate times slots per sample).

    std::atomic<int> driveLevel;        ///< Maximum pad drive level 0-7.
    std::atomic<float> iqScale;         ///< IQ magnitude gain.
    std::atomic<bool> constantEnvelope; ///< Constant-envelope amplitude mode.

    std::mutex modelMutex;                                      ///< Serialises PublishModel().
    SoapyRPITX_AmplitudeConfig ampCfg;                          ///< Amplitude configuration (modelMutex).
    std::shared_ptr<const SoapyRPITX_AmplitudeModel> published; ///< Latest model (std::atomic_load / std::atomic_store).
    std::shared_ptr<const SoapyRPITX_AmplitudeModel> model;     ///< Model in use (feeder thread only).
    int envAdvance = 0;                                         ///< Slots the envelope words are written ahead of their sample (feeder thread only).
    double envDelaySlots = 0;                                   ///< Software envelope delay in slots (feeder thread only).
    SoapyRPITX_AmplitudeQuantizer quantizer;                    ///< State selection (feeder thread only).
    SoapyRPITX_EnvelopeDelay envHistory;                        ///< Envelope history (feeder thread only).
    double appliedPhase = 0;                                    ///< AM/PM phase of the previous slot, in radians (feeder thread only).
    uint32_t floorCount = 0;                                    ///< Consecutive slots below the envelope floor (feeder thread only).
    std::complex<float> prevIq{0, 0};                           ///< Previous IQ sample, start of the interpolation (feeder thread only).
    uint64_t behindLimit = 0;                                   ///< First absolute position envelope words may still be written to (feeder thread only).
    uint32_t idlePrefix = 0;                                    ///< Idle slots written at the start of the ring before the DMA starts (feeder thread only).

    dsp mydsp;              ///< Phase/amplitude decomposition (feeder thread only).
    double phaseCarry = 0;  ///< Phase error of the PLL quantisation carried to the next slots, in radians (feeder thread only).
    double carryToHz = 0;   ///< Conversion of @ref phaseCarry to a frequency correction (slot rate / 2 pi).
    uint32_t fselIdle = 0;  ///< GPFSEL0 value with GPIO4 as input.
    uint32_t fselOn = 0;    ///< GPFSEL0 value with GPIO4 as GPCLK0 (ALT0).
    uint32_t fsel2Idle = 0; ///< GPFSEL2 value with GPIO20 as input.
    uint32_t fsel2On = 0;   ///< GPFSEL2 value with GPIO20 as GPCLK0 (ALT5).
    uint32_t idleFrac = 0;  ///< PLLC_FRAC word of the idle sample.
    uint32_t idlePad = 0;   ///< PAD word of the idle sample (original pad configuration).
    uint32_t idleEnv = 0;   ///< Envelope word of the idle sample.
    padgpio pads;           ///< Pad control block, used to restore @ref originalPads.
    pwmgpio pwm;            ///< PWM block (envelope output and pacing with the PWM output).

    SpscFifo<std::complex<float>> fifo;      ///< Samples queued by the producer.
    DmaRingTracker tracker;                  ///< Absolute ring positions (feeder thread only).
    std::vector<std::complex<float>> chunk;  ///< Transfer buffer between FIFO and ring (feeder thread only).
    std::vector<std::complex<float>> shadow; ///< Slot sample of every ring slot (zero for idle slots, feeder thread only).
    std::vector<uint8_t> slotIsData;         ///< 1 for ring slots holding a data sample, 0 for idle slots (feeder thread only).
    std::vector<std::complex<float>> replay; ///< Slot samples queued in the ring before a retune, sent first (feeder thread only).
    std::vector<uint8_t> replayIsData;       ///< 1 for data entries of @ref replay, 0 for idle entries (feeder thread only).
    size_t replayPos = 0;                    ///< Next entry of @ref replay (feeder thread only).
    std::atomic<size_t> replayLeft{0};       ///< Entries of @ref replay not yet in the ring.

    std::thread feeder;                             ///< Feeder thread.
    std::atomic<bool> running{false};               ///< Feeder thread run flag.
    std::atomic<bool> feederExited{true};           ///< The feeder thread is not executing its loop.
    std::atomic<bool> active{false};                ///< True between Start() and Stop().
    std::atomic<bool> endBurst{false};              ///< Burst end signalled by the producer.
    std::atomic<bool> dmaRunning{false};            ///< DMA engine running (published by the feeder).
    uint64_t lastDataPos = 0;                       ///< Absolute position after the last data slot (feeder thread only).
    uint64_t consumedTotal = 0;                     ///< FIFO samples written to the ring, on the scale of SpscFifo::pushedTotal() (feeder thread only).
    std::atomic<uint64_t> drainedPush{kNotDrained}; ///< SpscFifo::pushedTotal() up to which every sample has been played, or @ref kNotDrained.
    std::atomic<double> actualSampleRate{0.0};      ///< IQ sample rate produced by the pacing clock (published by ApplyTune()).
    std::atomic<uint64_t> underflows{0};            ///< FIFO starvation events.
    std::atomic<uint64_t> late{0};                  ///< DMA overrun events.
    std::atomic<uint64_t> dmaErrors{0};             ///< DMA restarts after the engine stopped by itself.
    std::atomic<uint64_t> samplesSent{0};           ///< IQ samples taken from the FIFO into the ring.
    std::atomic<uint64_t> envelopeClips{0};         ///< Envelope values limited to full scale.
    std::atomic<uint64_t> envelopeLate{0};          ///< Envelope words dropped because their slot could already be executing.

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
