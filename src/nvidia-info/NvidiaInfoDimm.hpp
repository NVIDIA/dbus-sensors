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

#include <sdbusplus/asio/object_server.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace nvidia
{
namespace info
{

// NvidiaDimm owns the 8 D-Bus interfaces that represent a single memory
// module at .../dimm/ProcessorModule_M_Memory_N: Item.Dimm,
// Connector.Slot, Item, Decorator.Asset, Decorator.LocationCode,
// Decorator.Location, Association.Definitions, and
// State.Decorator.OperationalStatus.
//
// Default-constructed by the vector<NvidiaDimm> inside TerminusData,
// populated via from_json, checked by validate(), and only then
// registered on D-Bus by publish(). The destructor removes any
// interfaces publish() created.
class NvidiaDimm
{
  public:
    NvidiaDimm() = default;
    NvidiaDimm(const NvidiaDimm&) = delete;
    NvidiaDimm& operator=(const NvidiaDimm&) = delete;
    NvidiaDimm(NvidiaDimm&&) = default;
    NvidiaDimm& operator=(NvidiaDimm&&) = default;
    ~NvidiaDimm();

    // Throws std::invalid_argument on rejected-enum or required-string
    // violations. Safe to call multiple times.
    void validate();

    // Registers the 8 interfaces on objServer. Caches objServer as a raw
    // pointer so the destructor can remove them. Must be called after
    // validate().
    void publish(sdbusplus::asio::object_server& objServer,
                 const std::string& dimmPath,
                 const std::string& motherboardPath);

    // JSON-populated fields. from_json fills these; validate() enforces
    // the rules described in NvidiaInfoEnums.hpp and below.
    uint32_t sizeKB{0};       // "MemorySizeKB" (optional)
    uint16_t dataWidth{0};    // "MemoryDataWidth" (optional)
    uint16_t totalWidth{0};   // "MemoryTotalWidth" (optional)
    std::string locator;      // "MemoryDeviceLocator", non-empty
    uint16_t maxSpeed{0};     // "MaxMemorySpeedInMHz" (optional)
    uint16_t configSpeed{0};  // "MemoryConfiguredSpeedInMhz" (optional)
    MemoryType memoryType{MemoryType::Unknown};      // "MemoryType"
    FormFactor formFactor{FormFactor::Unknown};      // "FormFactor"
    bool ecc{false};          // "ECC" (optional)
    std::string manufacturer; // "Manufacturer", non-empty
    std::string model;        // "Model" (may be empty)
    std::string partNumber;   // "PartNumber" (may be empty)
    std::string serialNumber; // "SerialNumber" (may be empty)
    std::string sku;          // "SKU" (may be empty)
    MemoryMedia memoryMedia{MemoryMedia::Unknown};   // "MemoryMedia"

  private:
    // Raw pointer — publish() sets it; destructor guards on nullptr.
    // Lifetime is managed by the surrounding NvidiaInfo::objServer
    // shared_ptr, which outlives terminusInfos (and thus this object).
    sdbusplus::asio::object_server* server{nullptr};

    std::shared_ptr<sdbusplus::asio::dbus_interface> dimmIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> slotIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> itemIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> assetIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> locationIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> locationTypeIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> assocIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> opStatusIface;
};

void from_json(const Json& j, NvidiaDimm& d);

} // namespace info
} // namespace nvidia
