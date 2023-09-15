/*
 * Copyright 2023 Emiliano Augusto Gonzalez (egonzalez . hiperion @ gmail . com))
 * * Project Site: https://github.com/hiperiondev/SoapySDR_rpitx *
 *
 * This is based on other projects:
 *    librpitx (https://github.com/F5OEO/librpitx)
 *        Copyright (C) 2018  Evariste COURJAUD F5OEO
 *    SoapyPlutoSDR (https://github.com/pothosware/SoapyPlutoSDR)
 *
 *    please contact their authors for more information.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include <vector>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Types.hpp>
#include <SoapySDR/Formats.hpp>

typedef enum rpitxStreamFormat {
    RPITX_SDR_CF32,
    RPITX_SDR_CS16,
} rpitxStreamFormat_t;

extern unsigned int buffer_qty;
extern float libRPITX_Frequency;
extern float libRPITX_PPM;
extern int libRPITX_SampleRate;
extern int libRPITX_IQBurst;
extern int FifoSize;
extern double libRPITX_Gain;
extern bool libRPITX_GainMode;

class tx_streamer {
public:
    tx_streamer(const rpitxStreamFormat format, const SoapySDR::Kwargs &args);
    ~tx_streamer();
    int send(const void *const*buffs, const size_t numElems, int &flags, const long long timeNs, const long timeoutUs);
    int flush();

private:
    int send_buf();
    bool has_direct_copy();

    const rpitxStreamFormat format = RPITX_SDR_CF32;
    size_t buf_size = 0;
    size_t items_in_buf = 0;
    bool direct_copy = false;
};

// A local spin_mutex usable with std::lock_guard
//   for lightweight locking for short periods.
class rpitx_spin_mutex {
public:
    rpitx_spin_mutex() = default;
    rpitx_spin_mutex(const rpitx_spin_mutex&) = delete;
    rpitx_spin_mutex& operator=(const rpitx_spin_mutex&) = delete;
    ~rpitx_spin_mutex() {
        lock_state.clear(std::memory_order_release);
    }
    void lock() {
        while (lock_state.test_and_set(std::memory_order_acquire))
            ;
    }
    void unlock() {
        lock_state.clear(std::memory_order_release);
    }

private:
    std::atomic_flag lock_state = ATOMIC_FLAG_INIT;
};

class SoapyRPITX: public SoapySDR::Device {
public:
    SoapyRPITX(const SoapySDR::Kwargs &args);
    ~SoapyRPITX();

    /*******************************************************************
     * Identification API
     ******************************************************************/
    std::string getDriverKey(void) const;
    std::string getHardwareKey(void) const;
    SoapySDR::Kwargs getHardwareInfo(void) const;

    /*******************************************************************
     * Channels API
     ******************************************************************/
    size_t getNumChannels(const int) const;
    bool getFullDuplex(const int direction, const size_t channel) const;

    /*******************************************************************
     * Stream API
     ******************************************************************/
    std::vector<std::string> getStreamFormats(const int direction, const size_t channel) const;
    std::string getNativeStreamFormat(const int direction, const size_t channel, double &fullScale) const;
    SoapySDR::ArgInfoList getStreamArgsInfo(const int direction, const size_t channel) const;
    SoapySDR::Stream* setupStream(const int direction, const std::string &format, const std::vector<size_t> &channels = std::vector<size_t>(),
            const SoapySDR::Kwargs &args = SoapySDR::Kwargs());
    void closeStream(SoapySDR::Stream *stream);
    size_t getStreamMTU(SoapySDR::Stream *stream) const;
    int activateStream(SoapySDR::Stream *stream, const int flags = 0, const long long timeNs = 0, const size_t numElems = 0);
    int deactivateStream(SoapySDR::Stream *stream, const int flags = 0, const long long timeNs = 0);
    int readStream(SoapySDR::Stream *stream, void *const*buffs, const size_t numElems, int &flags, long long &timeNs, const long timeoutUs = 100000);
    int writeStream(SoapySDR::Stream *stream, const void *const*buffs, const size_t numElems, int &flags, const long long timeNs = 0, const long timeoutUs =
            100000);
    int readStreamStatus(SoapySDR::Stream *stream, size_t &chanMask, int &flags, long long &timeNs, const long timeoutUs);

    /*******************************************************************
     * Sensor API
     ******************************************************************/
    std::vector<std::string> listSensors(void) const;
    SoapySDR::ArgInfo getSensorInfo(const std::string &key) const;
    std::string readSensor(const std::string &key) const;

    /*******************************************************************
     * Settings API
     ******************************************************************/
    SoapySDR::ArgInfoList getSettingInfo(void) const;
    void writeSetting(const std::string &key, const std::string &value);
    std::string readSetting(const std::string &key) const;

    /*******************************************************************
     * Antenna API
     ******************************************************************/
    std::vector<std::string> listAntennas(const int direction, const size_t channel) const;
    void setAntenna(const int direction, const size_t channel, const std::string &name);
    std::string getAntenna(const int direction, const size_t channel) const;

    /*******************************************************************
     * Frontend corrections API
     ******************************************************************/
    bool hasDCOffsetMode(const int direction, const size_t channel) const;

    /*******************************************************************
     * Gain API
     ******************************************************************/
    std::vector<std::string> listGains(const int direction, const size_t channel) const;
    bool hasGainMode(const int direction, const size_t channel) const;
    void setGainMode(const int direction, const size_t channel, const bool automatic);
    bool getGainMode(const int direction, const size_t channel) const;
    void setGain(const int direction, const size_t channel, const double value);
    void setGain(const int direction, const size_t channel, const std::string &name, const double value);
    double getGain(const int direction, const size_t channel, const std::string &name) const;
    SoapySDR::Range getGainRange(const int direction, const size_t channel, const std::string &name) const;

    /*******************************************************************
     * Frequency API
     ******************************************************************/
    void setFrequency(const int direction, const size_t channel, const std::string &name, const double frequency, const SoapySDR::Kwargs &args =
            SoapySDR::Kwargs());
    double getFrequency(const int direction, const size_t channel, const std::string &name) const;
    SoapySDR::ArgInfoList getFrequencyArgsInfo(const int direction, const size_t channel) const;
    std::vector<std::string> listFrequencies(const int direction, const size_t channel) const;
    SoapySDR::RangeList getFrequencyRange(const int direction, const size_t channel, const std::string &name) const;

    /*******************************************************************
     * Sample Rate API
     ******************************************************************/
    void setSampleRate(const int direction, const size_t channel, const double rate);
    double getSampleRate(const int direction, const size_t channel) const;
    std::vector<double> listSampleRates(const int direction, const size_t channel) const;
    void setBandwidth(const int direction, const size_t channel, const double bw);
    double getBandwidth(const int direction, const size_t channel) const;
    std::vector<double> listBandwidths(const int direction, const size_t channel) const;
    SoapySDR::RangeList getSampleRateRange(const int direction, const size_t channel) const;

private:
    bool IsValidRxStreamHandle(SoapySDR::Stream *handle) const;
    bool IsValidTxStreamHandle(SoapySDR::Stream *handle) const;
    bool is_sensor_channel(int channel) const;
    double double_from_buf(const char *buf) const;
    double get_sensor_value(int channel) const;
    std::string id_to_unit(const std::string &id) const;

    bool gainMode = false;
    mutable rpitx_spin_mutex tx_device_mutex;
    bool decimation = false;
    bool interpolation = false;
    std::unique_ptr<tx_streamer> tx_stream;
};
