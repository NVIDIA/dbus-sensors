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

#include <phosphor-logging/lg2.hpp>

#include <cstdint>
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
static constexpr std::string_view locationTypeSocket =
    "xyz.openbmc_project.Inventory.Decorator.Location.LocationTypes.Socket";
static constexpr std::string_view locationTypeEmbedded =
    "xyz.openbmc_project.Inventory.Decorator.Location.LocationTypes.Embedded";
static constexpr std::string_view locationTypeUnknown =
    "xyz.openbmc_project.Inventory.Decorator.Location.LocationTypes.Unknown";

// Socketed/pluggable form factors map to Socket; soldered-down parts map
// to Embedded; Unknown form factor maps to Unknown. Adding a new
// FormFactor value will hit the default and fall back to Unknown;
// classify it explicitly here when added.
static std::string_view locationTypeFor(FormFactor f)
{
    switch (f)
    {
        case FormFactor::RDIMM:
        case FormFactor::UDIMM:
        case FormFactor::SO_DIMM:
        case FormFactor::LRDIMM:
        case FormFactor::Mini_RDIMM:
        case FormFactor::Mini_UDIMM:
        case FormFactor::SO_RDIMM_72b:
        case FormFactor::SO_UDIMM_72b:
        case FormFactor::SO_DIMM_16b:
        case FormFactor::SO_DIMM_32b:
        case FormFactor::SOCAMM:
            return locationTypeSocket;
        case FormFactor::Die:
            return locationTypeEmbedded;
        case FormFactor::Unknown:
            return locationTypeUnknown;
    }
    return locationTypeUnknown;
}

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
    // Schema covers all DIMM constraints; no-op kept for validateEach<>
    // symmetry.
}

void NvidiaDimm::publish(sdbusplus::asio::object_server& objServer,
                         const std::string& dimmPath)
{
    const std::string memTypeStr =
        std::string(dimmDeviceTypePrefix) + memoryTypeName(memoryType);
    const std::string formFactorStr =
        std::string(dimmFormFactorPrefix) + formFactorName(formFactor);
    const std::string eccStr =
        std::string(dimmEccPrefix) + (ecc ? "MultiBitECC" : "NoECC");
    const std::string memoryMediaStr =
        std::string(dimmMemoryTechPrefix) + memoryMediaTechName(memoryMedia);

    auto& dimm =
        add(dimmPath, "xyz.openbmc_project.Inventory.Item.Dimm", objServer);
    dimm.register_property("MemorySizeInKB", sizeKB);
    dimm.register_property("MemoryDataWidth", dataWidth);
    dimm.register_property("MemoryTotalWidth", totalWidth);
    dimm.register_property("MemoryDeviceLocator", locator);
    dimm.register_property("MemoryType", memTypeStr);
    dimm.register_property("MaxMemorySpeedInMhz", maxSpeed);
    dimm.register_property("MemoryConfiguredSpeedInMhz", configSpeed);
    dimm.register_property("FormFactor", formFactorStr);
    dimm.register_property("ECC", eccStr);
    dimm.register_property("MemoryMedia", memoryMediaStr);

    add(dimmPath, "xyz.openbmc_project.Inventory.Connector.Slot", objServer);

    auto& item = add(dimmPath, "xyz.openbmc_project.Inventory.Item", objServer);
    item.register_property("PrettyName", std::string(""));
    item.register_property("Present", true);

    auto& asset = add(dimmPath, "xyz.openbmc_project.Inventory.Decorator.Asset",
                      objServer);
    asset.register_property("Manufacturer", manufacturer);
    asset.register_property("Model", model);
    asset.register_property("PartNumber", partNumber);
    asset.register_property("SerialNumber", serialNumber);
    asset.register_property("SKU", sku);

    auto& location =
        add(dimmPath, "xyz.openbmc_project.Inventory.Decorator.LocationCode",
            objServer);
    location.register_property("LocationCode", locator);

    auto& locationType =
        add(dimmPath, "xyz.openbmc_project.Inventory.Decorator.Location",
            objServer);
    locationType.register_property("LocationType",
                                   std::string(locationTypeFor(formFactor)));

    auto& assoc =
        add(dimmPath, "xyz.openbmc_project.Association.Definitions", objServer);
    {
        using AssocTuple = std::tuple<std::string, std::string, std::string>;
        using AssocList = std::vector<AssocTuple>;
        assoc.register_property("Associations", AssocList{});
    }
    assocIface = lastIface();

    auto& opStatus =
        add(dimmPath, "xyz.openbmc_project.State.Decorator.OperationalStatus",
            objServer);
    opStatus.register_property("Functional", true);

    initializeAll();

    lg2::info("Published DIMM at {P}", "P", dimmPath);
}

void NvidiaDimm::attach(const std::string& motherboardPath)
{
    if (!assocIface)
    {
        return;
    }
    using AssocTuple = std::tuple<std::string, std::string, std::string>;
    using AssocList = std::vector<AssocTuple>;
    AssocList assocs;
    assocs.emplace_back("chassis", "memories", motherboardPath);
    assocIface->set_property("Associations", assocs);
}

} // namespace info
} // namespace nvidia
