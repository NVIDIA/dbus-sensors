/*
 * Copyright (c) 2026 NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "NvidiaInfoPersistence.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace nvidia::info::persistence
{

namespace
{

constexpr std::string_view persistedFilenamePrefix = "ProcessorModule_";
constexpr std::string_view persistedFilenameSuffix = "_Info.json";

} // namespace

std::filesystem::path persistedPathFor(std::string_view dir,
                                       int32_t processorModuleIndex)
{
    return std::filesystem::path(dir) /
           std::format("{}{}{}", persistedFilenamePrefix, processorModuleIndex,
                       persistedFilenameSuffix);
}

std::optional<int32_t> moduleIndexFromPersistedFilename(
    std::string_view filename)
{
    if (!filename.starts_with(persistedFilenamePrefix) ||
        !filename.ends_with(persistedFilenameSuffix))
    {
        return std::nullopt;
    }
    const std::string_view digits =
        filename.substr(persistedFilenamePrefix.size(),
                        filename.size() - persistedFilenamePrefix.size() -
                            persistedFilenameSuffix.size());
    if (digits.size() != 1 || digits[0] < '0' || digits[0] > '9')
    {
        return std::nullopt;
    }
    return digits[0] - '0';
}

bool persistInfoJson(std::string_view dir, int32_t processorModuleIndex,
                     const std::string& jsonStr)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    if (ec)
    {
        lg2::error("Failed to create info directory {D}: {E}", "D",
                   std::string(dir), "E", ec.message());
        return false;
    }

    const fs::path finalPath = persistedPathFor(dir, processorModuleIndex);
    const fs::path tempPath = std::format("{}.tmp", finalPath.string());

    try
    {
        std::ofstream out(tempPath.string(),
                          std::ios::binary | std::ios::out | std::ios::trunc);
        if (!out.good())
        {
            lg2::error("Failed to open temp info file {F}", "F",
                       tempPath.string());
            return false;
        }
        out << jsonStr;
        out.flush();
        if (!out.good())
        {
            lg2::error("Failed to write temp info file {F}", "F",
                       tempPath.string());
            fs::remove(tempPath, ec);
            return false;
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Exception while writing temp info file {F}: {E}", "F",
                   tempPath.string(), "E", e.what());
        ec.clear();
        fs::remove(tempPath, ec);
        return false;
    }

    // ofstream::flush() only reaches the kernel buffer; reopen read-only and
    // fsync so the data is on disk before the rename. Best-effort: failures
    // are logged but do not abort the rename.
    if (int fd = ::open(tempPath.c_str(), O_RDONLY | O_CLOEXEC); fd < 0)
    {
        lg2::warning("Failed to open temp info file {F} for fsync: {E}", "F",
                     tempPath.string(), "E", std::strerror(errno));
    }
    else
    {
        if (::fsync(fd) < 0)
        {
            lg2::warning("Failed to fsync temp info file {F}: {E}", "F",
                         tempPath.string(), "E", std::strerror(errno));
        }
        ::close(fd);
    }

    fs::rename(tempPath, finalPath, ec);
    if (ec)
    {
        lg2::error("Failed to rename {T} to {F}: {E}", "T", tempPath.string(),
                   "F", finalPath.string(), "E", ec.message());
        fs::remove(tempPath, ec);
        return false;
    }

    lg2::info("Persisted info JSON to {F}", "F", finalPath.string());
    return true;
}

void removePersistedFile(std::string_view dir, int32_t processorModuleIndex)
{
    namespace fs = std::filesystem;
    const fs::path path = persistedPathFor(dir, processorModuleIndex);
    std::error_code ec;
    fs::remove(path, ec);
    if (ec)
    {
        lg2::warning("Failed to remove stale persisted file {F}: {E}", "F",
                     path.string(), "E", ec.message());
    }
    else
    {
        lg2::info("Removed stale persisted file: {F}", "F", path.string());
    }
}

} // namespace nvidia::info::persistence
