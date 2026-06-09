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

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Internal header: pure persisted-JSON helpers split out from NvidiaInfo.cpp
// so tests can drive them against a tmpdir instead of /var/lib/nvidia-info.
// The production single caller is NvidiaInfo, which threads its own
// persistedDir member through. No D-Bus dependencies here.
namespace nvidia::info::persistence
{

// Identity of a persisted info file. A module can hold multiple sockets, so
// both fields are needed (module alone would collide across sockets).
struct PersistedId
{
    int32_t processorModuleIndex{0};
    int32_t socket{0};
};

// Builds <dir>/ProcessorModule_<idx>_Socket_<socket>_Info.json. No I/O.
std::filesystem::path persistedPathFor(std::string_view dir, PersistedId id);

// Inverse of persistedPathFor()'s filename portion. The module must be a
// single digit 0..9 (avoids leading-zero aliasing and matches the range
// CreateInfo enforces); the socket is 0..255 with no leading zeros.
// Returns nullopt for any other shape.
std::optional<PersistedId> persistedIdFromFilename(std::string_view filename);

// Atomically writes jsonStr to persistedPathFor(dir, id) via a sibling .tmp
// file + rename, with an fsync between flush and rename. Creates dir (and
// any missing parents) if needed. Returns false on any I/O error; the
// caller is expected to roll back D-Bus state.
bool persistInfoJson(std::string_view dir, PersistedId id,
                     const std::string& jsonStr);

// Best-effort delete; missing file is not an error. Logged failures
// are intentionally non-fatal because callers (recovery, rejection)
// must continue regardless.
void removePersistedFile(std::string_view dir, PersistedId id);

} // namespace nvidia::info::persistence
