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

#include "NvidiaInfoPcie.hpp"

#include "NvidiaInfoEnums.hpp"

#include <phosphor-logging/lg2.hpp>

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace nvidia
{
namespace info
{

static constexpr std::string_view pcieGenPrefix =
    "xyz.openbmc_project.Inventory.Item.PCIeSlot.Generations.";
static constexpr std::string_view pcieSlotTypePrefix =
    "xyz.openbmc_project.Inventory.Item.PCIeSlot.SlotTypes.";

namespace
{

// Maps a numeric PCIe generation to its DBus Generations.* suffix.
std::string generationSuffix(uint32_t gen)
{
    switch (gen)
    {
        case 1:
            return "Gen1";
        case 2:
            return "Gen2";
        case 3:
            return "Gen3";
        case 4:
            return "Gen4";
        case 5:
            return "Gen5";
        case 6:
            return "Gen6";
        default:
            break;
    }
    return "Unknown";
}

} // namespace

void from_json(const Json& j, NvidiaPcie& c)
{
    // Schema rejects empty strings; unknown values fall back to OEM.
    c.slotType = j.at("SlotType").get<SlotType>();

    j.at("LocationCode").get_to(c.locationCode);
    j.at("Generation").get_to(c.generation);
    j.at("Lanes").get_to(c.lanes);
    j.at("MaxLinkSpeed").get_to(c.maxLinkSpeed);
    j.at("MaxLinkWidth").get_to(c.maxLinkWidth);

    c.present = j.value("Present", false);
    c.hotPluggable = j.value("HotPluggable", false);
    c.segmentControllerIndex = j.value("SegmentControllerIndex", 0U);
    c.portType = j.value("PortType", std::string());
    c.portProtocol = j.value("PortProtocol", std::string());
    c.rootPort = j.value("RootPort", 0U);
}

void NvidiaPcie::validate()
{
    // Schema covers all PCIe constraints; no-op kept for validateEach<>
    // symmetry.
}

void NvidiaPcie::publish(sdbusplus::asio::object_server& objServer,
                         const std::string& pciePath, uint64_t moduleIndex)
{
    const std::string generationStr =
        std::format("{}{}", pcieGenPrefix, generationSuffix(generation));
    const std::string slotTypeStr =
        std::format("{}{}", pcieSlotTypePrefix, slotTypeName(slotType));

    auto& pcie =
        add(pciePath, "xyz.openbmc_project.Inventory.Item.PCIeSlot", objServer);
    pcie.register_property("Generation", generationStr);
    pcie.register_property("Lanes", lanes);
    pcie.register_property("HotPluggable", hotPluggable);
    pcie.register_property("SlotType", slotTypeStr);
    pcie.register_property("ProcessorModuleIndex", moduleIndex);
    pcie.register_property("MaxLinkSpeed", maxLinkSpeed);
    pcie.register_property("SegmentControllerIndex", segmentControllerIndex);
    pcie.register_property("PortType", portType);
    pcie.register_property("PortProtocol", portProtocol);
    pcie.register_property("RootPort", rootPort);
    pcie.register_property("MaxLinkWidth", maxLinkWidth);

    auto& item = add(pciePath, "xyz.openbmc_project.Inventory.Item", objServer);
    item.register_property("PrettyName", std::string(""));
    item.register_property("Present", true);

    auto& location =
        add(pciePath, "xyz.openbmc_project.Inventory.Decorator.LocationCode",
            objServer);
    location.register_property("LocationCode", locationCode);

    add(pciePath, "xyz.openbmc_project.Inventory.Connector.Embedded",
        objServer);

    auto& assoc =
        add(pciePath, "xyz.openbmc_project.Association.Definitions", objServer);
    {
        using AssocTuple = std::tuple<std::string, std::string, std::string>;
        using AssocList = std::vector<AssocTuple>;
        assoc.register_property("Associations", AssocList{});
    }
    assocIface = lastIface();

    initializeAll();

    lg2::info("Published PCIe slot at {P}", "P", pciePath);
}

void NvidiaPcie::attach(const std::string& processorModulePath)
{
    if (!assocIface)
    {
        return;
    }
    using AssocTuple = std::tuple<std::string, std::string, std::string>;
    using AssocList = std::vector<AssocTuple>;
    AssocList assocs;
    assocs.emplace_back("chassis", "pcie_slots", processorModulePath);
    assocIface->set_property("Associations", assocs);
}

} // namespace info
} // namespace nvidia
