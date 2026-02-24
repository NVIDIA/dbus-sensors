/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <phosphor-logging/lg2.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace port_state
{

inline void saveState(const std::filesystem::path& dirPath,
                      const std::filesystem::path& filePath, bool enabled)
{
    try
    {
        std::filesystem::create_directories(dirPath);
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        lg2::error("Failed to create state directory {DIR}: {ERROR}", "DIR",
                   dirPath.string(), "ERROR", e.what());
        return;
    }

    std::ofstream ofs(filePath);
    if (!ofs)
    {
        lg2::error("Failed to open state file {FILE} for writing", "FILE",
                   filePath.string());
        return;
    }
    ofs << (enabled ? "true" : "false") << '\n';
    ofs.flush();
    if (ofs.fail())
    {
        lg2::error("Failed to write state file {FILE}", "FILE",
                   filePath.string());
    }
}

inline bool loadState(const std::filesystem::path& filePath, bool defaultValue)
{
    std::ifstream ifs(filePath);
    if (!ifs)
    {
        lg2::info("State file {FILE} not found, defaulting to {DEFAULT}",
                  "FILE", filePath.string(), "DEFAULT", defaultValue);
        return defaultValue;
    }
    std::string val;
    ifs >> val;
    if (ifs.fail())
    {
        lg2::error("Failed to read state file {FILE}", "FILE",
                   filePath.string());
        return defaultValue;
    }
    return (val == "true");
}

} // namespace port_state
