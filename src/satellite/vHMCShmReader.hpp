#pragma once

/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * mmap reader for /tmp/Sensors.shm. Mapping is lazy so satellitesensor
 * can start before the vHMC writer creates a ready header.
 */

#include "vHMCShmLayout.hpp"

#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace vhmc_shm
{

inline constexpr std::chrono::milliseconds mapRetryInterval{500};
inline constexpr std::chrono::seconds writerWaitLogAfter{60};
inline constexpr std::chrono::seconds writerWaitLogEvery{600};

// Wait-log cadence. Tested directly so the reader does not grow a fake clock.
inline bool shouldLogWriterWait(std::chrono::seconds waited,
                                std::chrono::steady_clock::time_point lastLog,
                                std::chrono::steady_clock::time_point now)
{
    if (waited < writerWaitLogAfter)
    {
        return false;
    }
    if (lastLog != std::chrono::steady_clock::time_point{} &&
        now - lastLog < writerWaitLogEvery)
    {
        return false;
    }
    return true;
}

class vHMCShmReader
{
    int fd = -1;
    uint8_t* mapBase = nullptr;
    size_t mapSize = 0;
    ShmHeader* header = nullptr;
    SensorRecord* records = nullptr;
    uint32_t mappedEpoch = 0;
    bool seenWriter = false;
    bool fileMissingLogged = false;
    bool waitLogEmitted = false;
    dev_t lastFailDev = 0;
    ino_t lastFailIno = 0;
    off_t lastFailSize = -1;
    std::chrono::steady_clock::time_point retryMapAfter;
    std::chrono::steady_clock::time_point waitUnreadySince;
    std::chrono::steady_clock::time_point lastWaitLog;
    std::string shmPath;

    void unmap();
    bool tryMap();
    bool ensureMapped();
    void maybeLogWriterWait();

  public:
    explicit vHMCShmReader(const std::string& path = {});
    ~vHMCShmReader();

    vHMCShmReader(const vHMCShmReader&) = delete;
    vHMCShmReader& operator=(const vHMCShmReader&) = delete;
    vHMCShmReader(vHMCShmReader&&) = delete;
    vHMCShmReader& operator=(vHMCShmReader&&) = delete;

    SensorError readSensor(SensorId id, SensorRecord& outRecord);
    bool hasSeenWriter() const;
    uint32_t currentEpoch() const;
};

} // namespace vhmc_shm
