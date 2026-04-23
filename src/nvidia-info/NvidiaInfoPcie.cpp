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

#include <cstdint>
#include <format>
#include <stdexcept>
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

// Maps the numeric PCIe generation to its phosphor-dbus-interfaces
// Generations.* suffix. 0 and out-of-range values (validate() caps at 6)
// map to Unknown.
std::string generationSuffix(uint32_t gen)
{
    switch (gen)
    {
        case 1: return "Gen1";
        case 2: return "Gen2";
        case 3: return "Gen3";
        case 4: return "Gen4";
        case 5: return "Gen5";
        case 6: return "Gen6";
        default: break;
    }
    return "Unknown";
}

} // namespace

void from_json(const Json& j, NvidiaPcie& c)
{
    // SlotType has OEM as its enum fallback, so any unknown string decodes
    // to OEM. An empty string is still rejected here because once decoded
    // it is indistinguishable from a legitimate "OEM".
    std::string rawSlotType;
    j.at("SlotType").get_to(rawSlotType);
    if (rawSlotType.empty())
    {
        throw std::invalid_argument("PCIe SlotType must be non-empty");
    }
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
    if (generation > 6)
    {
        throw std::invalid_argument("PCIe Generation must be 0-6");
    }
    if (lanes > 64)
    {
        throw std::invalid_argument("PCIe Lanes must be 0-64");
    }
    if (maxLinkSpeed > 6)
    {
        throw std::invalid_argument("PCIe MaxLinkSpeed must be 0-6");
    }
    if (maxLinkWidth > 64)
    {
        throw std::invalid_argument("PCIe MaxLinkWidth must be 0-64");
    }
    if (locationCode.empty())
    {
        throw std::invalid_argument("PCIe LocationCode must be non-empty");
    }
}

NvidiaPcie::~NvidiaPcie()
{
    if (server == nullptr)
    {
        return;
    }
    for (auto* iface : {&pcieIface, &itemIface, &locationIface, &embeddedIface,
                        &assocIface})
    {
        if (*iface)
        {
            server->remove_interface(*iface);
        }
    }
}

void NvidiaPcie::publish(sdbusplus::asio::object_server& objServer,
                         const std::string& pciePath,
                         const std::string& processorModulePath,
                         uint64_t moduleIndex)
{
    server = &objServer;

    const std::string generationStr =
        std::format("{}{}", pcieGenPrefix, generationSuffix(generation));
    const std::string slotTypeStr =
        std::format("{}{}", pcieSlotTypePrefix, slotTypeName(slotType));

    pcieIface = objServer.add_interface(
        pciePath, "xyz.openbmc_project.Inventory.Item.PCIeSlot");
    itemIface = objServer.add_interface(
        pciePath, "xyz.openbmc_project.Inventory.Item");
    locationIface = objServer.add_interface(
        pciePath, "xyz.openbmc_project.Inventory.Decorator.LocationCode");
    embeddedIface = objServer.add_interface(
        pciePath, "xyz.openbmc_project.Inventory.Connector.Embedded");
    assocIface = objServer.add_interface(
        pciePath, "xyz.openbmc_project.Association.Definitions");

    pcieIface->register_property("Generation", generationStr);
    pcieIface->register_property("Lanes", lanes);
    pcieIface->register_property("HotPluggable", hotPluggable);
    pcieIface->register_property("SlotType", slotTypeStr);
    pcieIface->register_property("ProcessorModuleIndex", moduleIndex);
    pcieIface->register_property("MaxLinkSpeed", maxLinkSpeed);
    pcieIface->register_property("SegmentControllerIndex",
                                 segmentControllerIndex);
    pcieIface->register_property("PortType", portType);
    pcieIface->register_property("PortProtocol", portProtocol);
    pcieIface->register_property("RootPort", rootPort);
    pcieIface->register_property("MaxLinkWidth", maxLinkWidth);

    itemIface->register_property("PrettyName", std::string(""));
    itemIface->register_property("Present", true);

    locationIface->register_property("LocationCode", locationCode);

    {
        using AssocTuple = std::tuple<std::string, std::string, std::string>;
        using AssocList = std::vector<AssocTuple>;
        AssocList assocs;
        assocs.emplace_back("chassis", "pcie_slots", processorModulePath);
        assocIface->register_property("Associations", assocs);
    }

    pcieIface->initialize();
    itemIface->initialize();
    locationIface->initialize();
    embeddedIface->initialize();
    assocIface->initialize();
}

} // namespace info
} // namespace nvidia
