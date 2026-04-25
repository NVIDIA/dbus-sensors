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

#include <cstdint>
#include <string>

namespace nvidia
{
namespace info
{

// Represents one processor: publishes 8 interfaces on .../cpu/CPU_N and
// 7 on the matching .../component/HGX_CPU_N chassis-component path.
// Lifecycle: from_json populates -> validate() derives -> publish()
// registers on D-Bus. Publisher base unregisters everything on destruction.
class NvidiaCpu : public Publisher
{
  public:
    NvidiaCpu() = default;
    NvidiaCpu(const NvidiaCpu&) = delete;
    NvidiaCpu& operator=(const NvidiaCpu&) = delete;
    NvidiaCpu(NvidiaCpu&&) = default;
    NvidiaCpu& operator=(NvidiaCpu&&) = default;
    ~NvidiaCpu() = default;

    // Parses hex idStr into idValue.
    void validate();

    // Registers the D-Bus interfaces. Call after validate().
    void publish(sdbusplus::asio::object_server& objServer,
                 const std::string& cpuPath, const std::string& componentPath,
                 const std::string& boardPath, uint64_t cpuIndex);

    uint32_t socketNum{0};     // "Socket", 0-255
    std::string family;        // "Family"
    std::string idStr;         // "Id" or "ID" (hex string)
    uint16_t coreCount{0};     // "CoreCount", >0
    uint16_t threadCount{0};   // "ThreadCount", >0
    uint32_t maxSpeedInMhz{0}; // "MaxSpeedInMhz", >0
    std::string manufacturer;  // "Manufacturer", non-empty
    std::string model;         // "Model", non-empty
    std::string modelRevision; // "ModelRevision", non-empty
    std::string serialNumber;  // "SerialNumber", non-empty
    std::string version;       // "Version", non-empty
    std::string sku;           // "SKU" (may be empty)

    // Hex-parsed from idStr by validate(); published as Item.Cpu "Id".
    uint64_t idValue{0};
};

void from_json(const Json& j, NvidiaCpu& c);

} // namespace info
} // namespace nvidia
