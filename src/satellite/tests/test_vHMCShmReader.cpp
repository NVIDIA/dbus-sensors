/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "vHMCShmLayout.hpp"
#include "vHMCShmReader.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include <gtest/gtest.h>

using vhmc_shm::alignUp;
using vhmc_shm::readyMagic;
using vhmc_shm::SensorDataType;
using vhmc_shm::SensorError;
using vhmc_shm::SensorRecord;
using vhmc_shm::seqlockRead;
using vhmc_shm::seqlockWrite;
using vhmc_shm::ShmHeader;
using vhmc_shm::shouldLogWriterWait;
using vhmc_shm::vHMCShmReader;

namespace
{

constexpr uint32_t testRecordCount = 4;

class ShmFile : public testing::Test
{
  public:
    std::filesystem::path dir;
    std::filesystem::path path;
    int fd = -1;
    pid_t lockPid = -1;
    uint8_t* base = nullptr;
    size_t mapSize = 0;
    ShmHeader* header = nullptr;
    SensorRecord* records = nullptr;

    void SetUp() override
    {
        std::array<char, 32> tmpl = {"./vhmcShmXXXXXX"};
        char* created = mkdtemp(tmpl.data());
        ASSERT_NE(created, nullptr);
        dir = created;
        path = dir / "Sensors.shm";
        createReadyFile(testRecordCount, 1);
    }

    void TearDown() override
    {
        unmapWriter();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void unmapWriter()
    {
        dropWriterLock();
        if (base != nullptr && base != MAP_FAILED)
        {
            munmap(base, mapSize);
            base = nullptr;
        }
        if (fd >= 0)
        {
            close(fd);
            fd = -1;
        }
        header = nullptr;
        records = nullptr;
        mapSize = 0;
    }

    void dropWriterLock()
    {
        if (lockPid > 0)
        {
            kill(lockPid, SIGKILL);
            waitpid(lockPid, nullptr, 0);
            lockPid = -1;
        }
    }

    void holdWriterLock()
    {
        dropWriterLock();
        int fds[2] = {-1, -1};
        ASSERT_EQ(::pipe(fds), 0);
        lockPid = ::fork();
        ASSERT_GE(lockPid, 0);
        if (lockPid == 0)
        {
            ::close(fds[0]);
            int lockFd = ::open(path.c_str(), O_RDWR);
            if (lockFd < 0)
            {
                ::_exit(1);
            }
            struct flock fl{};
            fl.l_type = F_WRLCK;
            fl.l_whence = SEEK_SET;
            fl.l_start = 0;
            fl.l_len = 0;
            if (::fcntl(lockFd, F_SETLK, &fl) != 0)
            {
                ::_exit(2);
            }
            char ok = 'L';
            if (::write(fds[1], &ok, 1) != 1)
            {
                ::_exit(3);
            }
            ::close(fds[1]);
            ::pause();
            ::_exit(0);
        }
        ::close(fds[1]);
        char ok = 0;
        ASSERT_EQ(::read(fds[0], &ok, 1), 1);
        ::close(fds[0]);
        ASSERT_EQ(ok, 'L');
    }

    void createReadyFile(uint32_t count, uint32_t epoch)
    {
        unmapWriter();

        const size_t recordsStart =
            alignUp(sizeof(ShmHeader), alignof(SensorRecord));
        mapSize = recordsStart + (count * sizeof(SensorRecord));

        fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(ftruncate(fd, static_cast<off_t>(mapSize)), 0);

        base = static_cast<uint8_t*>(
            mmap(nullptr, mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        ASSERT_NE(base, MAP_FAILED);

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        header = reinterpret_cast<ShmHeader*>(base);
        header->magicCookie.store(0, std::memory_order_relaxed);
        header->epoch.store(epoch, std::memory_order_relaxed);
        header->count = count;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        records = reinterpret_cast<SensorRecord*>(base + recordsStart);
        for (uint32_t i = 0; i < count; ++i)
        {
            records[i].seqAtom.store(0, std::memory_order_relaxed);
            records[i].type = SensorDataType::None;
            records[i].lastUpdated = 0;
            records[i].available = 0;
            records[i].value.u64Val = 0;
        }

        header->magicCookie.store(readyMagic, std::memory_order_release);
        holdWriterLock();
    }
};

} // namespace

TEST(vHMCShmLayout, recordIsOneCacheLine)
{
    EXPECT_EQ(sizeof(SensorRecord), 64U);
    EXPECT_EQ(alignof(SensorRecord), 64U);
    EXPECT_EQ(static_cast<uint32_t>(SensorDataType::None), 0U);
    EXPECT_EQ(static_cast<uint32_t>(SensorDataType::Double), 1U);
    EXPECT_EQ(static_cast<uint32_t>(SensorDataType::Uint64), 2U);
    EXPECT_EQ(static_cast<uint32_t>(SensorDataType::Uint32), 3U);
}

TEST(vHMCShmReader, missingFileIsWriterNotReady)
{
    vHMCShmReader reader("/tmp/vhmc_shm_does_not_exist.shm");
    SensorRecord rec{};
    EXPECT_EQ(reader.readSensor(0, rec), SensorError::WriterNotReady);
    EXPECT_EQ(reader.currentEpoch(), 0U);
}

TEST_F(ShmFile, magicNotReadyIsWriterNotReady)
{
    header->magicCookie.store(0, std::memory_order_release);
    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    EXPECT_EQ(reader.readSensor(0, rec), SensorError::WriterNotReady);
}

TEST_F(ShmFile, emptySlotIsSensorNoData)
{
    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    EXPECT_EQ(reader.readSensor(0, rec), SensorError::SensorNoData);
}

TEST_F(ShmFile, typeNoneWithTimestampIsSensorNoData)
{
    records[0].seqAtom.store(0, std::memory_order_relaxed);
    records[0].type = SensorDataType::None;
    records[0].lastUpdated = 1;
    records[0].available = 1;

    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    EXPECT_EQ(reader.readSensor(0, rec), SensorError::SensorNoData);
}

TEST_F(ShmFile, typedSlotWithZeroTimestampIsSensorNoData)
{
    records[0].seqAtom.store(0, std::memory_order_relaxed);
    records[0].type = SensorDataType::Double;
    records[0].lastUpdated = 0;
    records[0].available = 1;
    records[0].value.dVal = 1.0;

    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    EXPECT_EQ(reader.readSensor(0, rec), SensorError::SensorNoData);
}

TEST_F(ShmFile, invalidId)
{
    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    EXPECT_EQ(reader.readSensor(testRecordCount, rec),
              SensorError::InvalidSensorId);
}

TEST_F(ShmFile, emptyFileIsWriterNotReady)
{
    unmapWriter();
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, 0), 0);
    close(fd);
    fd = -1;

    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    EXPECT_EQ(reader.readSensor(0, rec), SensorError::WriterNotReady);
}

TEST_F(ShmFile, smallerThanHeaderIsWriterNotReady)
{
    unmapWriter();
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    const off_t tooSmall = static_cast<off_t>(sizeof(ShmHeader) - 1);
    ASSERT_EQ(ftruncate(fd, tooSmall), 0);
    close(fd);
    fd = -1;

    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    EXPECT_EQ(reader.readSensor(0, rec), SensorError::WriterNotReady);
}

TEST_F(ShmFile, zeroCountIsWriterNotReady)
{
    header->count = 0;
    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    EXPECT_EQ(reader.readSensor(0, rec), SensorError::WriterNotReady);
}

TEST_F(ShmFile, countExceedsFileIsWriterNotReady)
{
    header->count = testRecordCount + 1;
    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    EXPECT_EQ(reader.readSensor(0, rec), SensorError::WriterNotReady);
}

TEST_F(ShmFile, truncatedFileIsWriterNotReady)
{
    unmapWriter();
    const off_t tooSmall = static_cast<off_t>(sizeof(ShmHeader));
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, tooSmall), 0);
    base = static_cast<uint8_t*>(
        mmap(nullptr, static_cast<size_t>(tooSmall), PROT_READ | PROT_WRITE,
             MAP_SHARED, fd, 0));
    ASSERT_NE(base, MAP_FAILED);
    mapSize = static_cast<size_t>(tooSmall);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    header = reinterpret_cast<ShmHeader*>(base);
    header->magicCookie.store(readyMagic, std::memory_order_relaxed);
    header->epoch.store(1, std::memory_order_relaxed);
    header->count = 1;
    unmapWriter();

    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    EXPECT_EQ(reader.readSensor(0, rec), SensorError::WriterNotReady);
}

TEST_F(ShmFile, garbageMagicIsWriterNotReady)
{
    header->magicCookie.store(0xDEADBEEF, std::memory_order_release);
    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    EXPECT_EQ(reader.readSensor(0, rec), SensorError::WriterNotReady);
}

TEST_F(ShmFile, magicDroppedAfterMapIsWriterNotReady)
{
    seqlockWrite(&records[0], SensorDataType::Double, 1.0, 1);
    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    ASSERT_EQ(reader.readSensor(0, rec), SensorError::Success);

    header->magicCookie.store(0, std::memory_order_release);
    EXPECT_EQ(reader.readSensor(0, rec), SensorError::WriterNotReady);
}

TEST_F(ShmFile, missingWriterLockIsWriterNotReady)
{
    seqlockWrite(&records[0], SensorDataType::Double, 1.0, 1);
    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    ASSERT_EQ(reader.readSensor(0, rec), SensorError::Success);

    dropWriterLock();
    EXPECT_EQ(header->magicCookie.load(std::memory_order_acquire), readyMagic);
    EXPECT_EQ(reader.readSensor(0, rec), SensorError::WriterNotReady);

    holdWriterLock();
    ASSERT_EQ(reader.readSensor(0, rec), SensorError::Success);
    EXPECT_DOUBLE_EQ(rec.value.dVal, 1.0);
}

TEST(vHMCShmReader, repeatedMissingFileReadsStayNotReady)
{
    vHMCShmReader reader("/tmp/vhmc_shm_does_not_exist.shm");
    SensorRecord rec{};
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_EQ(reader.readSensor(0, rec), SensorError::WriterNotReady);
    }
}

TEST(vHMCShmReader, waitLogPolicy)
{
    using clock = std::chrono::steady_clock;
    const clock::time_point t0{};
    const clock::time_point never{};

    EXPECT_FALSE(shouldLogWriterWait(std::chrono::seconds{0}, never, t0));
    EXPECT_FALSE(shouldLogWriterWait(std::chrono::seconds{59}, never, t0));
    EXPECT_TRUE(shouldLogWriterWait(std::chrono::seconds{60}, never, t0));

    const clock::time_point firstLog = t0 + std::chrono::seconds{60};
    EXPECT_FALSE(shouldLogWriterWait(std::chrono::seconds{659}, firstLog,
                                     t0 + std::chrono::seconds{659}));
    EXPECT_TRUE(shouldLogWriterWait(std::chrono::seconds{660}, firstLog,
                                    t0 + std::chrono::seconds{660}));
}

// The ready cookie is published without resizing the file, so that transition
// is only picked up once the retry backoff expires.
TEST_F(ShmFile, readyCookieWithoutResizeIsPickedUpAfterBackoff)
{
    header->magicCookie.store(0, std::memory_order_release);
    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    ASSERT_EQ(reader.readSensor(0, rec), SensorError::WriterNotReady);

    header->magicCookie.store(readyMagic, std::memory_order_release);
    seqlockWrite(&records[0], SensorDataType::Double, 3.5, 1);
    ASSERT_EQ(reader.readSensor(0, rec), SensorError::WriterNotReady);

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    SensorError err = SensorError::WriterNotReady;
    while (std::chrono::steady_clock::now() < deadline)
    {
        err = reader.readSensor(0, rec);
        if (err == SensorError::Success)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(err, SensorError::Success);
    EXPECT_DOUBLE_EQ(rec.value.dVal, 3.5);
}

TEST_F(ShmFile, truncateInPlaceUnmapsBeforeHeaderAccess)
{
    seqlockWrite(&records[0], SensorDataType::Double, 1.0, 1);
    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    ASSERT_EQ(reader.readSensor(0, rec), SensorError::Success);

    // Drop this process's writer map so the test does not SIGBUS on truncate.
    if (base != nullptr && base != MAP_FAILED)
    {
        munmap(base, mapSize);
        base = nullptr;
        header = nullptr;
        records = nullptr;
    }
    ASSERT_EQ(::ftruncate(fd, 0), 0);
    EXPECT_EQ(reader.readSensor(0, rec), SensorError::WriterNotReady);

    createReadyFile(testRecordCount, 3);
    seqlockWrite(&records[0], SensorDataType::Double, 5.0, 1);
    ASSERT_EQ(reader.readSensor(0, rec), SensorError::Success);
    EXPECT_DOUBLE_EQ(rec.value.dVal, 5.0);
    EXPECT_EQ(reader.currentEpoch(), 3U);
}

TEST_F(ShmFile, unlinkedFileIsWriterNotReady)
{
    seqlockWrite(&records[0], SensorDataType::Double, 1.0, 1);
    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    ASSERT_EQ(reader.readSensor(0, rec), SensorError::Success);

    unmapWriter();
    ASSERT_EQ(::unlink(path.c_str()), 0);
    EXPECT_EQ(reader.readSensor(0, rec), SensorError::WriterNotReady);
}

TEST_F(ShmFile, readsDoubleUint32Uint64)
{
    seqlockWrite(&records[0], SensorDataType::Double, 42.5, 1);
    seqlockWrite(&records[1], SensorDataType::Uint32, uint32_t{7}, 1);
    seqlockWrite(&records[2], SensorDataType::Uint64, uint64_t{99}, 1);

    vHMCShmReader reader(path.string());
    SensorRecord rec{};

    ASSERT_EQ(reader.readSensor(0, rec), SensorError::Success);
    EXPECT_EQ(rec.type, SensorDataType::Double);
    EXPECT_DOUBLE_EQ(rec.value.dVal, 42.5);
    EXPECT_EQ(rec.available, 1);

    ASSERT_EQ(reader.readSensor(1, rec), SensorError::Success);
    EXPECT_EQ(rec.type, SensorDataType::Uint32);
    EXPECT_EQ(rec.value.u32Val, 7U);

    ASSERT_EQ(reader.readSensor(2, rec), SensorError::Success);
    EXPECT_EQ(rec.type, SensorDataType::Uint64);
    EXPECT_EQ(rec.value.u64Val, 99U);

    EXPECT_EQ(reader.currentEpoch(), 1U);
}

TEST_F(ShmFile, oddSeqIsReadContention)
{
    records[0].seqAtom.store(1, std::memory_order_release);
    records[0].type = SensorDataType::Double;
    records[0].lastUpdated = 1;
    records[0].available = 0;

    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    EXPECT_EQ(reader.readSensor(0, rec), SensorError::ReadContention);
}

TEST_F(ShmFile, epochChangeRemaps)
{
    seqlockWrite(&records[0], SensorDataType::Double, 1.0, 1);
    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    ASSERT_EQ(reader.readSensor(0, rec), SensorError::Success);
    EXPECT_EQ(reader.currentEpoch(), 1U);

    seqlockWrite(&records[0], SensorDataType::Double, 2.0, 1);
    header->epoch.store(2, std::memory_order_release);

    ASSERT_EQ(reader.readSensor(0, rec), SensorError::Success);
    EXPECT_EQ(reader.currentEpoch(), 2U);
    EXPECT_DOUBLE_EQ(rec.value.dVal, 2.0);
}

TEST_F(ShmFile, replacedFileRemaps)
{
    seqlockWrite(&records[0], SensorDataType::Double, 1.0, 1);
    vHMCShmReader reader(path.string());
    SensorRecord rec{};
    ASSERT_EQ(reader.readSensor(0, rec), SensorError::Success);
    EXPECT_DOUBLE_EQ(rec.value.dVal, 1.0);

    createReadyFile(testRecordCount, 5);
    seqlockWrite(&records[0], SensorDataType::Double, 9.0, 1);

    ASSERT_EQ(reader.readSensor(0, rec), SensorError::Success);
    EXPECT_EQ(reader.currentEpoch(), 5U);
    EXPECT_DOUBLE_EQ(rec.value.dVal, 9.0);
}
