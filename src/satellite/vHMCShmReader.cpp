/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "vHMCShmReader.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string>

namespace vhmc_shm
{

namespace
{
bool writerLockHeld(int checkFd)
{
    if (checkFd < 0)
    {
        return false;
    }
    struct flock fl{};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    if (::fcntl(checkFd, F_GETLK, &fl) != 0)
    {
        return false;
    }
    return fl.l_type != F_UNLCK;
}
} // namespace

vHMCShmReader::vHMCShmReader(const std::string& path) : shmPath(path) {}

vHMCShmReader::~vHMCShmReader()
{
    unmap();
}

void vHMCShmReader::unmap()
{
    if (mapBase != nullptr && mapBase != MAP_FAILED)
    {
        munmap(mapBase, mapSize);
        mapBase = nullptr;
    }
    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }
    header = nullptr;
    records = nullptr;
    mapSize = 0;
    mappedEpoch = 0;
}

void vHMCShmReader::maybeLogWriterWait()
{
    if (seenWriter)
    {
        return;
    }

    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    if (waitUnreadySince == std::chrono::steady_clock::time_point{})
    {
        waitUnreadySince = now;
    }

    const std::chrono::seconds waited =
        std::chrono::duration_cast<std::chrono::seconds>(
            now - waitUnreadySince);
    if (!shouldLogWriterWait(waited, lastWaitLog, now))
    {
        return;
    }

    lg2::error(
        "vHMCShmReader: writer has not published a ready header after {SEC} seconds",
        "SEC", waited.count());
    lastWaitLog = now;
    waitLogEmitted = true;
}

bool vHMCShmReader::tryMap()
{
    const char* path = shmPath.empty() ? shmFilePath : shmPath.c_str();

    struct stat pathSt{};
    const bool statOk = ::stat(path, &pathSt) == 0;
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();

    const bool sameFailedFile =
        statOk && pathSt.st_dev == lastFailDev &&
        pathSt.st_ino == lastFailIno && pathSt.st_size == lastFailSize;
    if (sameFailedFile && now < retryMapAfter)
    {
        maybeLogWriterWait();
        return false;
    }
    if (!statOk && lastFailSize == -1 && now < retryMapAfter)
    {
        maybeLogWriterWait();
        return false;
    }

    auto markFailed = [this, &now, statOk, &pathSt]() {
        if (statOk)
        {
            lastFailDev = pathSt.st_dev;
            lastFailIno = pathSt.st_ino;
            lastFailSize = pathSt.st_size;
        }
        else
        {
            lastFailDev = 0;
            lastFailIno = 0;
            lastFailSize = -1;
        }
        retryMapAfter = now + mapRetryInterval;
        maybeLogWriterWait();
        return false;
    };

    if (!statOk)
    {
        if (!fileMissingLogged)
        {
            lg2::error(
                "vHMCShmReader: Shared memory file {PATH} does not exist",
                "PATH", path);
            fileMissingLogged = true;
        }
        return markFailed();
    }

    int newFd = ::open(path, O_RDONLY);
    if (newFd < 0)
    {
        return markFailed();
    }

    struct stat st{};
    if (fstat(newFd, &st) != 0 ||
        st.st_size < static_cast<off_t>(sizeof(ShmHeader)))
    {
        close(newFd);
        return markFailed();
    }

    size_t size = static_cast<size_t>(st.st_size);
    uint8_t* base = static_cast<uint8_t*>(
        mmap(nullptr, size, PROT_READ, MAP_SHARED, newFd, 0));
    if (base == MAP_FAILED)
    {
        close(newFd);
        return markFailed();
    }

    // Writer uses O_TRUNC on the same inode. Do not touch the map until the
    // file is still the size we mapped.
    if (fstat(newFd, &st) != 0 || static_cast<size_t>(st.st_size) != size)
    {
        munmap(base, size);
        close(newFd);
        return markFailed();
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    ShmHeader* hdr = reinterpret_cast<ShmHeader*>(base);
    uint32_t magic = hdr->magicCookie.load(std::memory_order_acquire);
    if (magic != readyMagic)
    {
        munmap(base, size);
        close(newFd);
        return markFailed();
    }

    uint32_t count = hdr->count;
    const size_t recordsStart =
        alignUp(sizeof(ShmHeader), alignof(SensorRecord));
    const size_t recordsInFile =
        (size > recordsStart) ? ((size - recordsStart) / sizeof(SensorRecord))
                              : 0;
    if (count == 0 || count > recordsInFile)
    {
        munmap(base, size);
        close(newFd);
        return markFailed();
    }

    if (!writerLockHeld(newFd))
    {
        munmap(base, size);
        close(newFd);
        return markFailed();
    }

    fileMissingLogged = false;
    retryMapAfter = {};
    lastFailDev = 0;
    lastFailIno = 0;
    lastFailSize = -1;
    if (waitLogEmitted)
    {
        const std::chrono::seconds waited =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - waitUnreadySince);
        lg2::info(
            "vHMCShmReader: writer published a ready header after {SEC} seconds",
            "SEC", waited.count());
    }
    waitUnreadySince = {};
    lastWaitLog = {};
    waitLogEmitted = false;

    fd = newFd;
    mapBase = base;
    mapSize = size;
    header = hdr;
    mappedEpoch = hdr->epoch.load(std::memory_order_acquire);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    records = reinterpret_cast<SensorRecord*>(mapBase + recordsStart);
    seenWriter = true;
    return true;
}

bool vHMCShmReader::ensureMapped()
{
    if (header == nullptr)
    {
        return tryMap();
    }

    const char* path = shmPath.empty() ? shmFilePath : shmPath.c_str();
    struct stat pathSt{};
    if (::stat(path, &pathSt) != 0)
    {
        if (errno == ENOENT)
        {
            unmap();
            return tryMap();
        }
        return true;
    }
    struct stat fdSt{};
    if (::fstat(fd, &fdSt) != 0)
    {
        unmap();
        return tryMap();
    }
    if (pathSt.st_ino != fdSt.st_ino || pathSt.st_dev != fdSt.st_dev)
    {
        unmap();
        return tryMap();
    }
    // Same inode as a writer restart (open O_RDWR|O_TRUNC). Accessing the
    // old map after the file shrinks is SIGBUS; remap before any header load.
    if (fdSt.st_size <= 0 || static_cast<size_t>(fdSt.st_size) != mapSize)
    {
        unmap();
        return tryMap();
    }

    if (header->magicCookie.load(std::memory_order_acquire) != readyMagic)
    {
        return false;
    }
    if (!writerLockHeld(fd))
    {
        return false;
    }
    uint32_t current = header->epoch.load(std::memory_order_acquire);
    if (current != mappedEpoch)
    {
        unmap();
        return tryMap();
    }
    return true;
}

SensorError vHMCShmReader::readSensor(SensorId id, SensorRecord& outRecord)
{
    if (!ensureMapped())
    {
        return SensorError::WriterNotReady;
    }

    if (header == nullptr || records == nullptr)
    {
        return SensorError::WriterNotReady;
    }

    if (id >= header->count)
    {
        return SensorError::InvalidSensorId;
    }

    if (!seqlockRead(&records[id], outRecord))
    {
        return SensorError::ReadContention;
    }

    if (outRecord.type == SensorDataType::None || outRecord.lastUpdated == 0)
    {
        return SensorError::SensorNoData;
    }

    return SensorError::Success;
}

bool vHMCShmReader::hasSeenWriter() const
{
    return seenWriter;
}

uint32_t vHMCShmReader::currentEpoch() const
{
    if (header == nullptr)
    {
        return 0;
    }
    return header->epoch.load(std::memory_order_acquire);
}

} // namespace vhmc_shm
