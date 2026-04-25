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

    // Associate this DIMM with the given motherboard path. Idempotent;
    // must be called after publish().
    void attach(const std::string& motherboardPath);

    uint32_t sizeKB{0};      // "MemorySizeKB" (optional)
    uint16_t dataWidth{0};   // "MemoryDataWidth" (optional)
    uint16_t totalWidth{0};  // "MemoryTotalWidth" (optional)
    std::string locator;     // "MemoryDeviceLocator", non-empty
    uint16_t maxSpeed{0};    // "MaxMemorySpeedInMHz" (optional)
    uint16_t configSpeed{0}; // "MemoryConfiguredSpeedInMhz" (optional)
    MemoryType memoryType{MemoryType::Unknown}; // "MemoryType"
    FormFactor formFactor{FormFactor::Unknown}; // "FormFactor"
    bool ecc{false};                            // "ECC" (optional)
    std::string manufacturer;                   // "Manufacturer", non-empty
    std::string model;                          // "Model" (may be empty)
    std::string partNumber;                     // "PartNumber" (may be empty)
    std::string serialNumber;                   // "SerialNumber" (may be empty)
    std::string sku;                            // "SKU" (may be empty)
    MemoryMedia memoryMedia{MemoryMedia::Unknown}; // "MemoryMedia"

  private:
    // Cached Association.Definitions handle, mutated by attach().
    std::shared_ptr<sdbusplus::asio::dbus_interface> assocIface;
};

void from_json(const Json& j, NvidiaDimm& d);

} // namespace info
} // namespace nvidia
