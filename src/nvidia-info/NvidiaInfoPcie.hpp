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

// Represents one PCIe slot at .../<terminusName>_pcieslot<i>. Lifecycle:
// from_json -> validate() -> publish(); Publisher base unregisters on
// destruction.
class NvidiaPcie : public Publisher
{
  public:
    NvidiaPcie() = default;
    NvidiaPcie(const NvidiaPcie&) = delete;
    NvidiaPcie& operator=(const NvidiaPcie&) = delete;
    NvidiaPcie(NvidiaPcie&&) = default;
    NvidiaPcie& operator=(NvidiaPcie&&) = default;
    ~NvidiaPcie() = default;

    bool isPresent() const
    {
        return present;
    }

    void validate();

    // Registers the D-Bus interfaces. Associations are empty until attach().
    void publish(sdbusplus::asio::object_server& objServer,
                 const std::string& pciePath, uint64_t moduleIndex);

    // Associate this slot with the given processor-module path. Idempotent;
    // must be called after publish().
    void attach(const std::string& processorModulePath);

    // SlotType empty-string is rejected by the schema; unknown strings
    // decode to OEM via the enum's fallback.
    SlotType slotType{SlotType::OEM};   // "SlotType", OEM fallback
    std::string locationCode;           // "LocationCode", non-empty
    uint32_t generation{0};             // "Generation", 0-6
    uint32_t lanes{0};                  // "Lanes", 0-64
    uint32_t maxLinkSpeed{0};           // "MaxLinkSpeed", 0-6
    uint32_t maxLinkWidth{0};           // "MaxLinkWidth", 0-64
    bool present{false};                // "Present" (optional)
    bool hotPluggable{false};           // "HotPluggable" (optional)
    uint32_t segmentControllerIndex{0}; // "SegmentControllerIndex"
    std::string portType;               // "PortType" (optional)
    std::string portProtocol;           // "PortProtocol" (optional)
    uint32_t rootPort{0};               // "RootPort" (optional)

  private:
    // Cached Association.Definitions handle, mutated by attach().
    std::shared_ptr<sdbusplus::asio::dbus_interface> assocIface;
};

void from_json(const Json& j, NvidiaPcie& c);

} // namespace info
} // namespace nvidia
