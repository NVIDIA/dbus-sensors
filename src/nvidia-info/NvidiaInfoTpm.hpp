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

#include "NvidiaInfoEnums.hpp"
#include "NvidiaInfoPublisher.hpp"

#include <sdbusplus/asio/object_server.hpp>

#include <string>

namespace nvidia
{
namespace info
{

// Represents one TPM at .../chassis/motherboard/<terminusName>_tpm<i>.
// Lifecycle: from_json -> validate() -> publish().
class NvidiaTpm : public Publisher
{
  public:
    NvidiaTpm() = default;
    NvidiaTpm(const NvidiaTpm&) = delete;
    NvidiaTpm& operator=(const NvidiaTpm&) = delete;
    NvidiaTpm(NvidiaTpm&&) = default;
    NvidiaTpm& operator=(NvidiaTpm&&) = default;
    ~NvidiaTpm() = default;

    // No-op; kept for validateEach<>() symmetry.
    void validate();

    // Registers the D-Bus interfaces. Call after validate().
    void publish(sdbusplus::asio::object_server& objServer,
                 const std::string& tpmPath);

    // All optional; PrettyName/Model are derived from majorSpecVersion.
    std::string manufacturer;     // "Manufacturer" (optional)
    std::string version;          // "Version" (optional)
    std::string majorSpecVersion; // "MajorSpecVersion" (optional)
};

void from_json(const Json& j, NvidiaTpm& t);

} // namespace info
} // namespace nvidia
