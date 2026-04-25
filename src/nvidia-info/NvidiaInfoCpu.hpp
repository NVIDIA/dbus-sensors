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

// NvidiaCpu registers the 15 D-Bus interfaces that represent a single
// processor: 8 on its .../cpu/CPU_N path (Item.Cpu, Item, Asset,
// Revision, Instance, SKU, Association.Definitions, OperationalStatus)
// and 7 on its matching .../component/HGX_CPU_N chassis-component path
// (Item.Chassis, Item, Asset, Revision, Association.Definitions,
// OperationalStatus, Instance).
//
// The object is default-constructed by the vector<NvidiaCpu> inside
// TerminusData, populated via from_json, checked by validate(), and only
// then registered on D-Bus by publish(). The Publisher base owns every
// add()-ed interface and removes them all in its destructor, so dropping
// the containing TerminusData unpublishes the CPU cleanly.
class NvidiaCpu : public Publisher
{
  public:
    // Derivation step: parses idStr (a hex string the JSON schema has
    // already shape-checked) into the uint64_t idValue that publish()
    // registers as the Item.Cpu "Id" property. Safe to call multiple
    // times. Throws std::invalid_argument only on the (post-schema,
    // unreachable-in-practice) case where the hex parse fails.
    void validate();

    // Registers the 15 interfaces on objServer. Must be called after
    // validate(). The Publisher base retains objServer for destruction.
    void publish(sdbusplus::asio::object_server& objServer,
                 const std::string& cpuPath, const std::string& componentPath,
                 const std::string& boardPath, uint64_t cpuIndex);

    // JSON-populated fields. from_json fills these via j.at(key).get_to();
    // validate() enforces the range/non-empty rules below.
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

    // Populated by validate() from idStr (hex-parsed). Passed to D-Bus as
    // the Item.Cpu "Id" property.
    uint64_t idValue{0};
};

void from_json(const Json& j, NvidiaCpu& c);

} // namespace info
} // namespace nvidia
