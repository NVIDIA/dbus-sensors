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
#include <memory>
#include <optional>
#include <string>

namespace nvidia
{
namespace info
{

// Represents one DIMM at .../dimm/ProcessorModule_M_Memory_N. Lifecycle:
// from_json -> validate() -> publish(); Publisher base unregisters on
// destruction.
class NvidiaDimm : public Publisher
{
  public:
    NvidiaDimm() = default;
    NvidiaDimm(const NvidiaDimm&) = delete;
    NvidiaDimm& operator=(const NvidiaDimm&) = delete;
    NvidiaDimm(NvidiaDimm&&) = default;
    NvidiaDimm& operator=(NvidiaDimm&&) = default;
    ~NvidiaDimm() = default;

    void validate();

    // Registers the D-Bus interfaces. Associations are empty until attach().
    void publish(sdbusplus::asio::object_server& objServer,
                 const std::string& dimmPath);

    // Updates the location after the platform topology paths are discovered.
    void setLocationContext(const std::string& locationContext);

    // Associate this DIMM with the given motherboard path. Idempotent;
    // must be called after publish().
    void attach(const std::string& motherboardPath);

    bool present{false};                     // "Present", required
    std::string locator;                     // "MemoryDeviceLocator", non-empty
    std::optional<size_t> sizeKB;            // "MemorySizeKB"
    std::optional<uint16_t> dataWidth;       // "MemoryDataWidth"
    std::optional<uint16_t> totalWidth;      // "MemoryTotalWidth"
    std::optional<uint16_t> maxSpeed;        // "MaxMemorySpeedInMHz"
    std::optional<uint16_t> configSpeed;     // "MemoryConfiguredSpeedInMhz"
    std::optional<MemoryType> memoryType;    // "MemoryType"
    std::optional<FormFactor> formFactor;    // "FormFactor"
    std::optional<bool> ecc;                 // "ECC"
    std::optional<std::string> manufacturer; // "Manufacturer"
    std::optional<std::string> model;        // "Model"
    std::optional<std::string> partNumber;   // "PartNumber"
    std::optional<std::string> serialNumber; // "SerialNumber"
    std::optional<std::string> sku;          // "SKU"
    std::optional<MemoryMedia> memoryMedia;  // "MemoryMedia"

  private:
    // Cached Association.Definitions handle, mutated by attach().
    std::shared_ptr<sdbusplus::asio::dbus_interface> assocIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> locationContextIface;
};

void from_json(const Json& j, NvidiaDimm& d);

} // namespace info
} // namespace nvidia
