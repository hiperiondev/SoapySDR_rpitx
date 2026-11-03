/**
 * @file SoapyRPITX_Profile.hpp
 * @brief Hardware-independent parser of rpitx profile files: settings, default stream arguments and the
 *        initial gain stored as "key=value" lines, as written by rpitx_autotune.py.
 *
 * File format, one entry per line:
 *  - blank lines and lines whose first non-blank character is '#' are ignored;
 *  - "key=value" with a setting key (for example "ppm=1.37" or "amplitude_lut=0.12,0.27,...") is a setting;
 *  - "stream.key=value" (for example "stream.slots_per_sample=2") is a default stream argument;
 *  - "gain=value" is the initial PAD gain (0-7).
 * Spaces around the key and the value are removed; the value may contain '=' and ','. A later line with the
 * same key replaces the earlier one.
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

#include <fstream>
#include <istream>
#include <map>
#include <string>
#include <utility>
#include <vector>

/** @brief Prefix of the default stream arguments in a profile file. */
#define RPITX_PROFILE_STREAM_PREFIX "stream."

/** @brief Key of the initial gain in a profile file. */
#define RPITX_PROFILE_GAIN_KEY "gain"

/**
 * @brief Contents of a profile file.
 */
struct SoapyRPITX_Profile {
    std::vector<std::pair<std::string, std::string>> settings; ///< Settings in file order (one entry per key, the last value wins).
    std::map<std::string, std::string> stream;                 ///< Default stream arguments, without the "stream." prefix.
    bool hasGain = false;                                      ///< A "gain" line was present.
    std::string gain;                                          ///< Value of the "gain" line.
};

/**
 * @brief Removes leading and trailing blanks (spaces, tabs, carriage returns).
 * @param s Text.
 * @return Trimmed text.
 */
inline std::string SoapyRPITX_TrimProfileText(const std::string &s) {
    const char *blanks = " \t\r\n";
    size_t a = s.find_first_not_of(blanks);
    if (a == std::string::npos)
        return std::string();
    size_t b = s.find_last_not_of(blanks);
    return s.substr(a, b - a + 1);
}

/**
 * @brief Parses a profile.
 * @param in    Text stream of the profile.
 * @param out   Receives the entries (cleared first).
 * @param error Receives the description of the first problem ("line N: ...") when the function fails.
 * @return true on success, false for a line without '=' or with an empty key or an empty stream argument name.
 */
inline bool SoapyRPITX_ParseProfile(std::istream &in, SoapyRPITX_Profile &out, std::string &error) {
    out = SoapyRPITX_Profile();
    std::string line;
    size_t number = 0;
    const std::string prefix = RPITX_PROFILE_STREAM_PREFIX;
    while (std::getline(in, line)) {
        number++;
        std::string t = SoapyRPITX_TrimProfileText(line);
        if (t.empty() || t[0] == '#')
            continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            error = "line " + std::to_string(number) + ": expected key=value";
            return false;
        }
        std::string key = SoapyRPITX_TrimProfileText(t.substr(0, eq));
        std::string value = SoapyRPITX_TrimProfileText(t.substr(eq + 1));
        if (key.empty()) {
            error = "line " + std::to_string(number) + ": empty key";
            return false;
        }
        if (key.compare(0, prefix.size(), prefix) == 0) {
            std::string name = key.substr(prefix.size());
            if (name.empty()) {
                error = "line " + std::to_string(number) + ": empty stream argument name";
                return false;
            }
            out.stream[name] = value;
        } else if (key == RPITX_PROFILE_GAIN_KEY) {
            out.hasGain = true;
            out.gain = value;
        } else {
            bool replaced = false;
            for (auto &kv : out.settings) {
                if (kv.first == key) {
                    kv.second = value;
                    replaced = true;
                }
            }
            if (!replaced)
                out.settings.emplace_back(key, value);
        }
    }
    if (in.bad()) {
        error = "read error";
        return false;
    }
    return true;
}

/**
 * @brief Reads and parses a profile file.
 * @param path  File name.
 * @param out   Receives the entries.
 * @param error Receives the description of the problem when the function fails.
 * @return true on success, false when the file cannot be opened or a line is malformed.
 */
inline bool SoapyRPITX_LoadProfile(const std::string &path, SoapyRPITX_Profile &out, std::string &error) {
    std::ifstream f(path);
    if (!f) {
        error = "cannot open the file";
        return false;
    }
    return SoapyRPITX_ParseProfile(f, out, error);
}
