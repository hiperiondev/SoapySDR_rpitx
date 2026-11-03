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
 * A profile file must be a regular file of at most @ref RPITX_PROFILE_MAX_BYTES bytes that is not writable by
 * group or others; symbolic links are not followed.
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

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <istream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

/** @brief Prefix of the default stream arguments in a profile file. */
#define RPITX_PROFILE_STREAM_PREFIX "stream."

/** @brief Key of the initial gain in a profile file. */
#define RPITX_PROFILE_GAIN_KEY "gain"

/** @brief Largest profile file accepted, in bytes. */
#define RPITX_PROFILE_MAX_BYTES 65536

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
 * @brief Tells whether files and directories of a user can be trusted by a process that may run as root.
 * @param uid Owner.
 * @return true for root and for the effective user of the process.
 */
inline bool SoapyRPITX_TrustedOwner(uid_t uid) {
    return uid == 0 || uid == geteuid();
}

/**
 * @brief Opens a profile file after walking its path from the root without following symbolic links.
 *
 * Every directory of the path must be owned by root or by the effective user and must not be writable by
 * others (a directory with the sticky bit, such as /tmp, is accepted: only the owner of an entry can replace it,
 * and the file's own owner is checked by the caller). The file is opened relative to the last directory, so the
 * directories checked are the ones the file is read from. A relative path is resolved against the current
 * directory, which is checked as well.
 *
 * @param path  File name.
 * @param error Receives the description of the problem when the function fails.
 * @return The file descriptor, or -1.
 */
inline int SoapyRPITX_OpenTrustedFile(const std::string &path, std::string &error) {
    std::string full = path;
    if (full.empty() || full[0] != '/') {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd)) == nullptr) {
            error = "cannot open the file";
            return -1;
        }
        full = std::string(cwd) + "/" + path;
    }

    std::vector<std::string> parts;
    {
        std::istringstream in(full);
        std::string part;
        while (std::getline(in, part, '/')) {
            if (!part.empty() && part != ".")
                parts.push_back(part);
        }
    }
    if (parts.empty()) {
        error = "cannot open the file";
        return -1;
    }

    int dirfd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (size_t i = 0; dirfd >= 0; i++) {
        struct stat ds;
        if (fstat(dirfd, &ds) != 0) {
            close(dirfd);
            error = "cannot open the file";
            return -1;
        }
        if (!SoapyRPITX_TrustedOwner(ds.st_uid)) {
            close(dirfd);
            error = "a directory of the path is owned by another user";
            return -1;
        }
        if ((ds.st_mode & S_IWOTH) != 0 && (ds.st_mode & S_ISVTX) == 0) {
            close(dirfd);
            error = "a directory of the path is writable by others";
            return -1;
        }

        if (i + 1 == parts.size()) {
            int fd = openat(dirfd, parts[i].c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY);
            close(dirfd);
            if (fd < 0)
                error = "cannot open the file";
            return fd;
        }

        int next = openat(dirfd, parts[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(dirfd);
        dirfd = next;
    }
    error = "cannot open a directory of the path (missing, or a symbolic link)";
    return -1;
}

/**
 * @brief Reads and parses a profile file.
 *
 * The driver can run as root, so the file is trusted only when nobody else can change it: it is opened without
 * following symbolic links (in any component of the path) and without blocking, and is refused unless it is a
 * regular file of at most @ref RPITX_PROFILE_MAX_BYTES bytes that is owned by root or by the effective user and
 * that group and others cannot write, in a path whose directories are owned by root or by the effective user and
 * are not writable by others (see @ref SoapyRPITX_OpenTrustedFile()). Devices, pipes and sockets are refused
 * before anything is read from them.
 *
 * @param path  File name.
 * @param out   Receives the entries.
 * @param error Receives the description of the problem when the function fails.
 * @return true on success, false when the file cannot be opened, is not an acceptable file, cannot be read
 *         completely or holds a malformed line.
 */
inline bool SoapyRPITX_LoadProfile(const std::string &path, SoapyRPITX_Profile &out, std::string &error) {
    int fd = SoapyRPITX_OpenTrustedFile(path, error);
    if (fd < 0)
        return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        error = "not a regular file";
        return false;
    }
    if (st.st_size > (off_t) RPITX_PROFILE_MAX_BYTES) {
        close(fd);
        error = "larger than " + std::to_string(RPITX_PROFILE_MAX_BYTES) + " bytes";
        return false;
    }
    if (!SoapyRPITX_TrustedOwner(st.st_uid)) {
        close(fd);
        error = "owned by another user";
        return false;
    }
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        close(fd);
        error = "writable by group or others";
        return false;
    }

    // One byte more than the limit detects a file that grew after fstat().
    std::string text((size_t) RPITX_PROFILE_MAX_BYTES + 1, '\0');
    size_t used = 0;
    while (used < text.size()) {
        ssize_t n = pread(fd, &text[used], text.size() - used, (off_t) used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            close(fd);
            error = "read error";
            return false;
        }
        if (n == 0)
            break;
        used += (size_t) n;
    }
    close(fd);
    if (used > (size_t) RPITX_PROFILE_MAX_BYTES) {
        error = "larger than " + std::to_string(RPITX_PROFILE_MAX_BYTES) + " bytes";
        return false;
    }
    text.resize(used);

    std::istringstream in(text);
    return SoapyRPITX_ParseProfile(in, out, error);
}
