#pragma once

/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Seqlock sensor table shared with the vHMC writer
 * (vhmc-shared-memory-sensors). Layout must stay in lockstep: 64-byte
 * SensorRecord, readyMagic, epoch, and header->count.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>

namespace vhmc_shm
{

inline constexpr const char* shmFilePath = "/tmp/Sensors.shm";
inline constexpr uint32_t readyMagic = 0x52445921; // "RDY!"
inline constexpr size_t cacheLineSize = 64;

inline size_t alignUp(size_t x, size_t a)
{
    return (x + a - 1) & ~(a - 1);
}

using SensorId = uint32_t;

// Writer allocates this many slots. Readers use header->count.
inline constexpr uint32_t sensorCountMax = 512;

enum class SensorDataType : uint32_t
{
    None = 0,
    Double = 1,
    Uint64 = 2,
    Uint32 = 3,
    Raw = 4
};

enum class SensorError : int32_t
{
    Success = 0,
    WriterNotReady = -1,
    WriterDisconnected = -2,
    InvalidSensorId = -3,
    ReadContention = -4,
    SensorNoData = -5,
};

inline const char* sensorErrorToString(SensorError err)
{
    switch (err)
    {
        case SensorError::Success:
            return "Success";
        case SensorError::WriterNotReady:
            return "Writer not ready";
        case SensorError::WriterDisconnected:
            return "Writer disconnected";
        case SensorError::InvalidSensorId:
            return "Invalid sensor ID";
        case SensorError::ReadContention:
            return "Read contention (retries exhausted)";
        case SensorError::SensorNoData:
            return "No data available";
        default:
            return "Unknown error";
    }
}

struct alignas(cacheLineSize) SensorRecord
{
    std::atomic<uint32_t> seqAtom; // even=stable, odd=write in progress
    SensorDataType type;
    uint64_t lastUpdated;          // ms since epoch
    uint8_t available; // 0=invalid, 1=valid; default 0 until first valid write
    uint8_t padAlign[7];
    union
    {
        double dVal;
        uint64_t u64Val;
        uint32_t u32Val;
        uint8_t raw[8];
    } value;
    uint8_t pad[cacheLineSize - sizeof(std::atomic<uint32_t>) -
                sizeof(SensorDataType) - sizeof(uint64_t) - sizeof(uint8_t) -
                sizeof(padAlign) - sizeof(value)];
};

static_assert(alignof(SensorRecord) == cacheLineSize,
              "SensorRecord must be 64B aligned");
static_assert(sizeof(SensorRecord) == cacheLineSize,
              "SensorRecord must be exactly 64B");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "atomic<uint32_t> must be lock-free for shared-memory use");

struct ShmHeader
{
    std::atomic<uint32_t> magicCookie; // 0 until ready, then readyMagic
    std::atomic<uint32_t> epoch;       // bumped on each writer startup
    uint32_t count;                    // number of records
};

inline uint64_t getCurrentTimeMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

template <typename T>
inline void seqlockWrite(SensorRecord* rec, SensorDataType type, const T& val,
                         uint8_t available)
{
    rec->seqAtom.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    rec->type = type;
    rec->lastUpdated = getCurrentTimeMs();
    rec->available = available;

    if constexpr (std::is_same_v<T, double>)
    {
        rec->value.dVal = val;
    }
    else if constexpr (std::is_same_v<T, uint64_t>)
    {
        rec->value.u64Val = val;
    }
    else if constexpr (std::is_same_v<T, uint32_t>)
    {
        rec->value.u32Val = val;
    }
    else
    {
        static_assert(sizeof(T) <= sizeof(rec->value.raw),
                      "T too large for raw[8]");
        std::memcpy(rec->value.raw, &val, sizeof(T));
        if constexpr (sizeof(T) < sizeof(rec->value.raw))
        {
            std::memset(rec->value.raw + sizeof(T), 0,
                        sizeof(rec->value.raw) - sizeof(T));
        }
    }

    std::atomic_thread_fence(std::memory_order_release);
    rec->seqAtom.fetch_add(1, std::memory_order_relaxed);
}

namespace detail
{

inline std::atomic<uint64_t>& contentionCounterImpl()
{
    static std::atomic<uint64_t> count{0};
    return count;
}

} // namespace detail

inline void incrementContentionCount()
{
    detail::contentionCounterImpl().fetch_add(1, std::memory_order_relaxed);
}

inline uint64_t getContentionCount()
{
    return detail::contentionCounterImpl().load(std::memory_order_relaxed);
}

inline void resetContentionCount()
{
    detail::contentionCounterImpl().store(0, std::memory_order_relaxed);
}

inline bool seqlockRead(const SensorRecord* ptr, SensorRecord& outCopy)
{
    uint32_t seq1 = 0;
    uint32_t seq2 = 0;
    int retries = 100;

    do
    {
        if (--retries == 0)
        {
            return false;
        }

        seq1 = ptr->seqAtom.load(std::memory_order_acquire);
        if ((seq1 & 1U) != 0U)
        {
            incrementContentionCount();
            std::this_thread::yield();
            continue;
        }

        std::atomic_thread_fence(std::memory_order_acquire);

        outCopy.type = ptr->type;
        outCopy.lastUpdated = ptr->lastUpdated;
        outCopy.available = ptr->available;
        std::memcpy(&outCopy.value, &ptr->value, sizeof(ptr->value));

        std::atomic_thread_fence(std::memory_order_acquire);

        seq2 = ptr->seqAtom.load(std::memory_order_acquire);
        if (seq1 != seq2)
        {
            incrementContentionCount();
        }
    } while (seq1 != seq2 || ((seq2 & 1U) != 0U));

    outCopy.seqAtom.store(seq2, std::memory_order_relaxed);
    return true;
}

} // namespace vhmc_shm
