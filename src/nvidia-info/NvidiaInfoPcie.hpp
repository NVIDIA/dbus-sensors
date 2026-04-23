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

// NvidiaPcie owns the 5 D-Bus interfaces that represent a single PCIe
// slot at .../<terminusName>_pcieslot<i>: Item.PCIeSlot, Item,
// Decorator.LocationCode, Connector.Embedded, and
// Association.Definitions.
//
// Default-constructed by the vector<NvidiaPcie> inside TerminusData,
// populated via from_json, checked by validate(), and only then
// registered on D-Bus by publish(). The destructor removes any
// interfaces publish() created.
class NvidiaPcie
{
  public:
    NvidiaPcie() = default;
    NvidiaPcie(const NvidiaPcie&) = delete;
    NvidiaPcie& operator=(const NvidiaPcie&) = delete;
    NvidiaPcie(NvidiaPcie&&) = default;
    NvidiaPcie& operator=(NvidiaPcie&&) = default;
    ~NvidiaPcie();

    bool isPresent() const { return present; }

    // Throws std::invalid_argument on range or non-empty-string
    // violations. Safe to call multiple times.
    void validate();

    // Registers the 5 interfaces on objServer. Caches objServer as a raw
    // pointer so the destructor can remove them. Must be called after
    // validate().
    void publish(sdbusplus::asio::object_server& objServer,
                 const std::string& pciePath,
                 const std::string& processorModulePath,
                 uint64_t moduleIndex);

    // JSON-populated fields. from_json fills these; validate() enforces
    // the rules below. The SlotType empty-string check happens in
    // from_json because an empty string decodes to SlotType::OEM and is
    // indistinguishable here.
    SlotType slotType{SlotType::OEM};    // "SlotType", OEM fallback
    std::string locationCode;            // "LocationCode", non-empty
    uint32_t generation{0};              // "Generation", 0-6
    uint32_t lanes{0};                   // "Lanes", 0-64
    uint32_t maxLinkSpeed{0};            // "MaxLinkSpeed", 0-6
    uint32_t maxLinkWidth{0};            // "MaxLinkWidth", 0-64
    bool present{false};                 // "Present" (optional)
    bool hotPluggable{false};            // "HotPluggable" (optional)
    uint32_t segmentControllerIndex{0};  // "SegmentControllerIndex"
    std::string portType;                // "PortType" (optional)
    std::string portProtocol;            // "PortProtocol" (optional)
    uint32_t rootPort{0};                // "RootPort" (optional)

  private:
    // Raw pointer — publish() sets it; destructor guards on nullptr.
    // Lifetime is managed by the surrounding NvidiaInfo::objServer
    // shared_ptr, which outlives terminusInfos (and thus this object).
    sdbusplus::asio::object_server* server{nullptr};

    std::shared_ptr<sdbusplus::asio::dbus_interface> pcieIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> itemIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> locationIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> embeddedIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> assocIface;
};

void from_json(const Json& j, NvidiaPcie& c);

} // namespace info
} // namespace nvidia
