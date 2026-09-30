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
#include <optional>
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

template <typename T>
static std::optional<T> getOptional(const Json& j, const char* key)
{
    if (auto it = j.find(key); it != j.end())
    {
        return it->get<T>();
    }
    return std::nullopt;
}

void from_json(const Json& j, NvidiaDimm& d)
{
    j.at("Present").get_to(d.present);
    j.at("MemoryDeviceLocator").get_to(d.locator);
    d.sizeKB = getOptional<uint32_t>(j, "MemorySizeKB");
    d.dataWidth = getOptional<uint16_t>(j, "MemoryDataWidth");
    d.totalWidth = getOptional<uint16_t>(j, "MemoryTotalWidth");
    d.maxSpeed = getOptional<uint16_t>(j, "MaxMemorySpeedInMHz");
    d.configSpeed = getOptional<uint16_t>(j, "MemoryConfiguredSpeedInMhz");
    d.memoryType = getOptional<MemoryType>(j, "MemoryType");
    d.formFactor = getOptional<FormFactor>(j, "FormFactor");
    d.ecc = getOptional<bool>(j, "ECC");
    d.manufacturer = getOptional<std::string>(j, "Manufacturer");
    d.model = getOptional<std::string>(j, "Model");
    d.partNumber = getOptional<std::string>(j, "PartNumber");
    d.serialNumber = getOptional<std::string>(j, "SerialNumber");
    d.sku = getOptional<std::string>(j, "SKU");
    d.memoryMedia = getOptional<MemoryMedia>(j, "MemoryMedia");
}

void NvidiaDimm::validate()
{
    // Schema covers all DIMM constraints; no-op kept for validateEach<>
    // symmetry.
}

void NvidiaDimm::publish(sdbusplus::asio::object_server& objServer,
                         const std::string& dimmPath)
{
    const auto registerIfPresent =
        [](auto& iface, const char* property, const auto& value) {
            if (value.has_value())
            {
                iface.register_property(property, *value);
            }
        };

    const auto memTypeStr = memoryType.transform([](MemoryType value) {
        return std::string(dimmDeviceTypePrefix) + memoryTypeName(value);
    });
    const auto formFactorStr = formFactor.transform([](FormFactor value) {
        return std::string(dimmFormFactorPrefix) + formFactorName(value);
    });
    const auto eccStr = ecc.transform([](bool value) {
        return std::string(dimmEccPrefix) + (value ? "MultiBitECC" : "NoECC");
    });
    const auto memoryMediaStr = memoryMedia.transform([](MemoryMedia value) {
        return std::string(dimmMemoryTechPrefix) + memoryMediaTechName(value);
    });

    auto& dimm =
        add(dimmPath, "xyz.openbmc_project.Inventory.Item.Dimm", objServer);
    registerIfPresent(dimm, "MemorySizeInKB", sizeKB);
    registerIfPresent(dimm, "MemoryDataWidth", dataWidth);
    registerIfPresent(dimm, "MemoryTotalWidth", totalWidth);
    dimm.register_property("MemoryDeviceLocator", locator);
    registerIfPresent(dimm, "MemoryType", memTypeStr);
    registerIfPresent(dimm, "MaxMemorySpeedInMhz", maxSpeed);
    registerIfPresent(dimm, "MemoryConfiguredSpeedInMhz", configSpeed);
    registerIfPresent(dimm, "FormFactor", formFactorStr);
    registerIfPresent(dimm, "ECC", eccStr);
    registerIfPresent(dimm, "MemoryMedia", memoryMediaStr);

    add(dimmPath, "xyz.openbmc_project.Inventory.Connector.Slot", objServer);

    auto& item = add(dimmPath, "xyz.openbmc_project.Inventory.Item", objServer);
    item.register_property("PrettyName", std::string(""));
    item.register_property("Present", present);

    auto& asset = add(dimmPath, "xyz.openbmc_project.Inventory.Decorator.Asset",
                      objServer);
    registerIfPresent(asset, "Manufacturer", manufacturer);
    registerIfPresent(asset, "Model", model);
    registerIfPresent(asset, "PartNumber", partNumber);
    registerIfPresent(asset, "SerialNumber", serialNumber);
    registerIfPresent(asset, "SKU", sku);

    auto& location =
        add(dimmPath, "xyz.openbmc_project.Inventory.Decorator.LocationCode",
            objServer);
    location.register_property("LocationCode", locator);

    auto& locationType =
        add(dimmPath, "xyz.openbmc_project.Inventory.Decorator.Location",
            objServer);
    registerIfPresent(locationType, "LocationType",
                      formFactor.transform([](FormFactor value) {
                          return std::string(locationTypeFor(value));
                      }));

    auto& context =
        add(dimmPath, "xyz.openbmc_project.Inventory.Decorator.LocationContext",
            objServer);
    context.register_property("LocationContext", std::string());
    locationContextIface = lastIface();

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

void NvidiaDimm::setLocationContext(const std::string& locationContext)
{
    if (locationContextIface)
    {
        locationContextIface->set_property("LocationContext", locationContext);
    }
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
