// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
#pragma once

#include "LpuPowerMode.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace lpu::power
{
// LPU firmware forgets its selector on every reset. Set by the required meson
// option lpu-power-mode-store, because only the platform knows which storage
// survives its resets: the LP30 vHMC, whose /var is new on every start, points
// it at a BMC eMMC bind mount.
inline constexpr std::string_view defaultStorePath = LPU_POWER_MODE_STORE;

class ModeStore
{
  public:
    explicit ModeStore(std::filesystem::path file = std::filesystem::path(
                           defaultStorePath)) : file(std::move(file))
    {}

    // Nothing stored means no selection was ever made: firmware stays unset.
    // A missing, unreadable or malformed file counts as nothing stored.
    std::optional<Mode> load() const
    {
        std::ifstream input(file);
        std::string value;
        std::string extra;
        if (!(input >> value) || (input >> extra))
        {
            return std::nullopt;
        }
        return modeFromName(value);
    }

    // Replace atomically: a reset mid-save leaves the old or the new
    // selection, never a truncated file. Missing directories are created.
    void save(Mode mode) const
    {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        if (ec)
        {
            fail("save", ec.value());
        }
        const std::string temp = file.string() + ".tmp";
        const int fd = ::open(temp.c_str(),
                              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0)
        {
            fail("save", errno);
        }
        const std::string text = std::string(modeName(mode)) + "\n";
        const bool written = ::write(fd, text.data(), text.size()) ==
                                 static_cast<ssize_t>(text.size()) &&
                             ::fsync(fd) == 0;
        const int error = errno;
        ::close(fd);
        if (!written)
        {
            ::unlink(temp.c_str());
            fail("save", error);
        }
        if (::rename(temp.c_str(), file.c_str()) != 0)
        {
            const int renameError = errno;
            ::unlink(temp.c_str());
            fail("save", renameError);
        }
        syncDirectory("save");
    }

    // Forget the selection, so later resets leave the firmware unset.
    void clear() const
    {
        std::error_code ec;
        const bool removed = std::filesystem::remove(file, ec);
        if (ec)
        {
            fail("remove", ec.value());
        }
        if (removed)
        {
            syncDirectory("remove");
        }
    }

  private:
    // Persist the rename or unlink itself before reporting success.
    void syncDirectory(std::string_view action) const
    {
        const int dir = ::open(file.parent_path().c_str(),
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir < 0)
        {
            fail(action, errno);
        }
        const bool synced = ::fsync(dir) == 0;
        const int error = errno;
        ::close(dir);
        if (!synced)
        {
            fail(action, error);
        }
    }

    [[noreturn]] void fail(std::string_view action, int error) const
    {
        throw Error(
            Failure::writeFailure,
            "Cannot " + std::string(action) + " power mode file " +
                file.string() + ": " +
                std::error_code(error, std::generic_category()).message());
    }

    std::filesystem::path file;
};

// Save before writing the LPUs, and put the file back if either step fails, so
// a later reset never applies a selection the caller was told had failed.
// Every file access runs through onStorage(work), which lets the service keep
// it off its event loop: on a busy eMMC one save can take seconds.
template <typename OnStorage>
void setAndSave(Controller& controller, const ModeStore& store, Mode target,
                const OnStorage& onStorage)
{
    std::optional<Mode> previous;
    std::optional<Error> failure;
    try
    {
        onStorage([&] {
            previous = store.load();
            store.save(target);
        });
        controller.set(target);
    }
    catch (const Error& error)
    {
        failure = error;
    }
    if (!failure)
    {
        return;
    }
    // set() already rolls the LPUs back on failure. The file is rolled back
    // outside the catch block because onStorage may suspend a coroutine.
    try
    {
        onStorage([&] {
            if (store.load() == previous)
            {
                return;
            }
            if (previous)
            {
                store.save(*previous);
            }
            else
            {
                store.clear();
            }
        });
    }
    catch (const Error& rollback)
    {
        throw Error(failure->failure,
                    std::string(failure->what()) + "; " + rollback.what());
    }
    throw Error(*failure);
}
} // namespace lpu::power
