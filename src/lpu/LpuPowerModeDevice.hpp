// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
#pragma once

#include "LpuPowerMode.hpp"

#include <fcntl.h>
#include <lpu850/lpu850-uapi.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace lpu::power
{
// Discover and read the live mode through read-only sysfs. Writes use the
// existing slot-stable character device; never remount sysfs or bypass lpu850.
class DeviceTransport : public Transport
{
  public:
    explicit DeviceTransport(std::filesystem::path root =
                                "/sys/bus/i2c/devices") : root(std::move(root))
    {}

    void refresh() override
    {
        paths.fill({});
        std::error_code ec;
        std::filesystem::directory_iterator it(root, ec);
        if (ec)
        {
            throw Error(Failure::unavailable, "LPU sysfs is unavailable");
        }
        for (; it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            if (ec)
            {
                break;
            }
            const auto dir = it->path() / "lpu850";
            std::ifstream input(dir / "slot_index");
            if (!input.is_open())
            {
                continue;
            }
            unsigned slot = 0;
            std::string extra;
            if (!(input >> slot) || (input >> extra) || slot == 0 ||
                slot > slotCount || !paths[slot - 1].empty())
            {
                throw Error(Failure::unavailable,
                            "Invalid or duplicate LPU slot_index");
            }
            paths[slot - 1] = dir / "power_mode";
        }
        if (ec)
        {
            throw Error(Failure::unavailable, "LPU discovery failed");
        }
        for (size_t slot = 0; slot < slotCount; ++slot)
        {
            if (paths[slot].empty() ||
                !std::filesystem::exists(paths[slot], ec) || ec)
            {
                throw Error(Failure::unavailable,
                            "Missing lpu850 power_mode ABI at slot " +
                                std::to_string(slot + 1));
            }
        }
    }

    Mode read(size_t slot) override
    {
        std::ifstream input(paths.at(slot));
        unsigned value = 0;
        std::string extra;
        if (!(input >> value) || (input >> extra) || value > 2)
        {
            // The kernel checks interface >= 8 on each live read; an old
            // firmware, missing device or transfer failure must fail preflight.
            throw Error(Failure::unavailable,
                        "Cannot read power mode at slot " +
                            std::to_string(slot + 1));
        }
        return static_cast<Mode>(value);
    }

    void write(size_t slot, Mode mode) override
    {
        if (slot >= slotCount || (mode != Mode::maxQ && mode != Mode::maxP))
        {
            throw Error(Failure::invalidArgument,
                        "Invalid LPU power mode/slot");
        }
        const std::string path = "/dev/lpu" + std::to_string(slot + 1) + "_log";
        const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
        {
            fail(path, errno);
        }
        uint8_t requested = static_cast<uint8_t>(mode);
        const int result = ::ioctl(fd, LPU_LOG_SET_POWER_MODE, &requested);
        const int error = errno;
        ::close(fd); // Never read/consume the log stream.
        if (result < 0)
        {
            fail(path, error);
        }
    }

  private:
    [[noreturn]] static void fail(const std::string& path, int error)
    {
        throw Error(
            Failure::writeFailure,
            "Cannot set power mode through " + path + ": " +
                std::error_code(error, std::generic_category()).message());
    }

    std::filesystem::path root;
    std::array<std::filesystem::path, slotCount> paths;
};
} // namespace lpu::power
