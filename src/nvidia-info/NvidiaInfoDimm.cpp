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

#include "NvidiaInfoDimm.hpp"

#include "NvidiaInfoEnums.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace nvidia
{
namespace info
{

static constexpr std::string_view dimmEccPrefix =
    "xyz.openbmc_project.Inventory.Item.Dimm.Ecc.";
static constexpr std::string_view dimmFormFactorPrefix =
    "xyz.openbmc_project.Inventory.Item.Dimm.FormFactor.";
static constexpr std::string_view dimmDeviceTypePrefix =
    "xyz.openbmc_project.Inventory.Item.Dimm.DeviceType.";
static constexpr std::string_view dimmMemoryTechPrefix =
    "xyz.openbmc_project.Inventory.Item.Dimm.MemoryTech.";
static constexpr std::string_view locationTypeSlot =
    "xyz.openbmc_project.Inventory.Decorator.Location.LocationTypes.Slot";

void from_json(const Json& j, NvidiaDimm& d)
{
    d.sizeKB = j.value("MemorySizeKB", 0U);
    d.dataWidth = j.value("MemoryDataWidth", static_cast<uint16_t>(0));
    d.totalWidth = j.value("MemoryTotalWidth", static_cast<uint16_t>(0));
    j.at("MemoryDeviceLocator").get_to(d.locator);
    d.maxSpeed = j.value("MaxMemorySpeedInMHz", static_cast<uint16_t>(0));
    d.configSpeed =
        j.value("MemoryConfiguredSpeedInMhz", static_cast<uint16_t>(0));
    j.at("MemoryType").get_to(d.memoryType);
    j.at("FormFactor").get_to(d.formFactor);
    d.ecc = j.value("ECC", false);
    j.at("Manufacturer").get_to(d.manufacturer);
    d.model = j.value("Model", std::string());
    d.partNumber = j.value("PartNumber", std::string());
    d.serialNumber = j.value("SerialNumber", std::string());
    d.sku = j.value("SKU", std::string());
    j.at("MemoryMedia").get_to(d.memoryMedia);
}

void NvidiaDimm::validate()
{
    if (locator.empty())
    {
        throw std::invalid_argument(
            "MemoryDeviceLocator must be non-empty");
    }
    if (manufacturer.empty())
    {
        throw std::invalid_argument("Manufacturer must be non-empty");
    }
    if (memoryType == MemoryType::Unknown)
    {
        throw std::invalid_argument(
            "MemoryType must be a recognized DeviceType value");
    }
    if (formFactor == FormFactor::Unknown)
    {
        throw std::invalid_argument(
            "FormFactor must be a recognized value");
    }
    if (memoryMedia == MemoryMedia::Unknown)
    {
        throw std::invalid_argument(
            "MemoryMedia must be \"DRAM\", \"NAND\", or \"Intel3DXPoint\"");
    }
}

NvidiaDimm::~NvidiaDimm()
{
    if (server == nullptr)
    {
        return;
    }
    for (auto* iface :
         {&dimmIface, &slotIface, &itemIface, &assetIface, &locationIface,
          &locationTypeIface, &assocIface, &opStatusIface})
    {
        if (*iface)
        {
            server->remove_interface(*iface);
        }
    }
}

void NvidiaDimm::publish(sdbusplus::asio::object_server& objServer,
                         const std::string& dimmPath,
                         const std::string& motherboardPath)
{
    server = &objServer;

    const std::string memTypeStr =
        std::string(dimmDeviceTypePrefix) + memoryTypeName(memoryType);
    const std::string formFactorStr =
        std::string(dimmFormFactorPrefix) + formFactorName(formFactor);
    const std::string eccStr =
        std::string(dimmEccPrefix) + (ecc ? "MultiBitECC" : "NoECC");
    const std::string memoryMediaStr =
        std::string(dimmMemoryTechPrefix) + memoryMediaTechName(memoryMedia);

    dimmIface = objServer.add_interface(
        dimmPath, "xyz.openbmc_project.Inventory.Item.Dimm");
    slotIface = objServer.add_interface(
        dimmPath, "xyz.openbmc_project.Inventory.Connector.Slot");
    itemIface = objServer.add_interface(
        dimmPath, "xyz.openbmc_project.Inventory.Item");
    assetIface = objServer.add_interface(
        dimmPath, "xyz.openbmc_project.Inventory.Decorator.Asset");
    locationIface = objServer.add_interface(
        dimmPath, "xyz.openbmc_project.Inventory.Decorator.LocationCode");
    locationTypeIface = objServer.add_interface(
        dimmPath, "xyz.openbmc_project.Inventory.Decorator.Location");
    assocIface = objServer.add_interface(
        dimmPath, "xyz.openbmc_project.Association.Definitions");
    opStatusIface = objServer.add_interface(
        dimmPath, "xyz.openbmc_project.State.Decorator.OperationalStatus");

    dimmIface->register_property("MemorySizeInKB", sizeKB);
    dimmIface->register_property("MemoryDataWidth", dataWidth);
    dimmIface->register_property("MemoryTotalWidth", totalWidth);
    dimmIface->register_property("MemoryDeviceLocator", locator);
    dimmIface->register_property("MemoryType", memTypeStr);
    dimmIface->register_property("MaxMemorySpeedInMhz", maxSpeed);
    dimmIface->register_property("MemoryConfiguredSpeedInMhz", configSpeed);
    dimmIface->register_property("FormFactor", formFactorStr);
    dimmIface->register_property("ECC", eccStr);
    dimmIface->register_property("MemoryMedia", memoryMediaStr);

    itemIface->register_property("PrettyName", std::string(""));
    itemIface->register_property("Present", true);

    assetIface->register_property("Manufacturer", manufacturer);
    assetIface->register_property("Model", model);
    assetIface->register_property("PartNumber", partNumber);
    assetIface->register_property("SerialNumber", serialNumber);
    assetIface->register_property("SKU", sku);

    locationIface->register_property("LocationCode", locator);

    std::string locationType;
    if (formFactor == FormFactor::SOCAMM)
    {
        locationType = std::string(locationTypeSlot);
    }
    locationTypeIface->register_property("LocationType", locationType);

    {
        using AssocTuple = std::tuple<std::string, std::string, std::string>;
        using AssocList = std::vector<AssocTuple>;
        AssocList assocs;
        assocs.emplace_back("chassis", "memories", motherboardPath);
        assocIface->register_property("Associations", assocs);
    }

    opStatusIface->register_property("Functional", true);

    dimmIface->initialize();
    slotIface->initialize();
    itemIface->initialize();
    assetIface->initialize();
    locationIface->initialize();
    locationTypeIface->initialize();
    assocIface->initialize();
    opStatusIface->initialize();
}

} // namespace info
} // namespace nvidia
