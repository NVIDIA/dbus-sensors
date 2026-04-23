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

#include "NvidiaInfoCpu.hpp"

#include "NvidiaInfoEnums.hpp"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace nvidia
{
namespace info
{

void from_json(const Json& j, NvidiaCpu& c)
{
    j.at("Socket").get_to(c.socketNum);
    j.at("Family").get_to(c.family);
    // Schema permits either "Id" or "ID" for the hex processor id string.
    if (j.contains("Id"))
    {
        j.at("Id").get_to(c.idStr);
    }
    else
    {
        j.at("ID").get_to(c.idStr);
    }
    j.at("CoreCount").get_to(c.coreCount);
    j.at("ThreadCount").get_to(c.threadCount);
    j.at("MaxSpeedInMhz").get_to(c.maxSpeedInMhz);
    j.at("Manufacturer").get_to(c.manufacturer);
    j.at("Model").get_to(c.model);
    j.at("ModelRevision").get_to(c.modelRevision);
    j.at("SerialNumber").get_to(c.serialNumber);
    j.at("Version").get_to(c.version);
    j.at("SKU").get_to(c.sku);
}

void NvidiaCpu::validate()
{
    if (socketNum > 255)
    {
        throw std::invalid_argument("Socket must be 0-255");
    }
    if (coreCount == 0)
    {
        throw std::invalid_argument("CoreCount must be > 0");
    }
    if (threadCount == 0)
    {
        throw std::invalid_argument("ThreadCount must be > 0");
    }
    if (maxSpeedInMhz == 0)
    {
        throw std::invalid_argument("MaxSpeedInMhz must be > 0");
    }
    if (manufacturer.empty())
    {
        throw std::invalid_argument("Manufacturer must be non-empty");
    }
    if (model.empty())
    {
        throw std::invalid_argument("Model must be non-empty");
    }
    if (modelRevision.empty())
    {
        throw std::invalid_argument("ModelRevision must be non-empty");
    }
    if (serialNumber.empty())
    {
        throw std::invalid_argument("SerialNumber must be non-empty");
    }
    if (version.empty())
    {
        throw std::invalid_argument("Version must be non-empty");
    }

    // "Id" accepts an optional 0x/0X prefix followed by 1-16 hex digits.
    std::string_view sv(idStr);
    if (sv.size() >= 2 && sv[0] == '0' && (sv[1] == 'x' || sv[1] == 'X'))
    {
        sv.remove_prefix(2);
    }
    if (sv.empty() || sv.size() > 16)
    {
        throw std::invalid_argument(
            "Id must be 1-16 hex digits with optional 0x/0X prefix");
    }
    for (unsigned char ch : sv)
    {
        if (std::isxdigit(ch) == 0)
        {
            throw std::invalid_argument(
                "Id must contain only hex digits (0-9, a-f, A-F)");
        }
    }
    uint64_t parsed = 0;
    const auto [ptr, ec] =
        std::from_chars(sv.data(), sv.data() + sv.size(), parsed, 16);
    if (ec != std::errc{} || ptr != sv.data() + sv.size())
    {
        throw std::invalid_argument("Id hex parse failed");
    }
    idValue = parsed;
}

NvidiaCpu::~NvidiaCpu()
{
    if (server == nullptr)
    {
        return;
    }
    for (auto* iface :
         {&cpuIface, &itemIface, &assetIface, &revIface, &instanceIface,
          &skuIface, &assocIface, &opStatusIface, &chassisIface,
          &chassisItemIface, &chassisAssetIface, &chassisRevIface,
          &chassisAssocIface, &chassisOpStatusIface, &chassisInstanceIface})
    {
        if (*iface)
        {
            server->remove_interface(*iface);
        }
    }
}

void NvidiaCpu::publish(sdbusplus::asio::object_server& objServer,
                        const std::string& cpuPath,
                        const std::string& componentPath,
                        const std::string& boardPath, uint64_t cpuIndex)
{
    server = &objServer;

    using AssocTuple = std::tuple<std::string, std::string, std::string>;
    using AssocList = std::vector<AssocTuple>;

    const std::string cpuName = "CPU_" + std::to_string(cpuIndex);

    // --- cpuPath: 8 interfaces -------------------------------------------
    cpuIface = objServer.add_interface(
        cpuPath, "xyz.openbmc_project.Inventory.Item.Cpu");
    itemIface =
        objServer.add_interface(cpuPath, "xyz.openbmc_project.Inventory.Item");
    assetIface = objServer.add_interface(
        cpuPath, "xyz.openbmc_project.Inventory.Decorator.Asset");
    revIface = objServer.add_interface(
        cpuPath, "xyz.openbmc_project.Inventory.Decorator.Revision");
    instanceIface = objServer.add_interface(
        cpuPath, "xyz.openbmc_project.Inventory.Decorator.Instance");
    skuIface = objServer.add_interface(
        cpuPath, "xyz.openbmc_project.Inventory.Decorator.SKU");
    assocIface = objServer.add_interface(
        cpuPath, "xyz.openbmc_project.Association.Definitions");
    opStatusIface = objServer.add_interface(
        cpuPath, "xyz.openbmc_project.State.Decorator.OperationalStatus");

    cpuIface->register_property("Socket", std::to_string(socketNum));
    cpuIface->register_property("Family", family);
    cpuIface->register_property("Id", idValue);
    cpuIface->register_property("CoreCount", coreCount);
    cpuIface->register_property("ThreadCount", threadCount);
    cpuIface->register_property("MaxSpeedInMhz", maxSpeedInMhz);
    cpuIface->register_property("ModelRevision", modelRevision);
    cpuIface->register_property("ProcessorType", std::string("CPU"));

    itemIface->register_property("PrettyName", model);
    itemIface->register_property("Present", true);

    assetIface->register_property("Manufacturer", manufacturer);
    assetIface->register_property("Model", model);
    assetIface->register_property("SerialNumber", serialNumber);
    assetIface->register_property("Name", cpuName);

    revIface->register_property("Version", version);

    instanceIface->register_property("InstanceNumber", cpuIndex);

    skuIface->register_property("SKU", sku);

    {
        AssocList assocs;
        assocs.emplace_back("chassis", "all_processors", componentPath);
        assocIface->register_property("Associations", assocs);
    }

    opStatusIface->register_property("Functional", true);

    cpuIface->initialize();
    itemIface->initialize();
    assetIface->initialize();
    revIface->initialize();
    instanceIface->initialize();
    skuIface->initialize();
    assocIface->initialize();
    opStatusIface->initialize();

    // --- componentPath: 7 merged chassis-component interfaces ------------
    chassisIface = objServer.add_interface(
        componentPath, "xyz.openbmc_project.Inventory.Item.Chassis");
    chassisItemIface = objServer.add_interface(
        componentPath, "xyz.openbmc_project.Inventory.Item");
    chassisAssetIface = objServer.add_interface(
        componentPath, "xyz.openbmc_project.Inventory.Decorator.Asset");
    chassisRevIface = objServer.add_interface(
        componentPath, "xyz.openbmc_project.Inventory.Decorator.Revision");
    chassisAssocIface = objServer.add_interface(
        componentPath, "xyz.openbmc_project.Association.Definitions");
    chassisOpStatusIface = objServer.add_interface(
        componentPath, "xyz.openbmc_project.State.Decorator.OperationalStatus");
    chassisInstanceIface = objServer.add_interface(
        componentPath, "xyz.openbmc_project.Inventory.Decorator.Instance");

    chassisIface->register_property(
        "Type", std::string("xyz.openbmc_project.Inventory.Item.Chassis."
                            "ChassisType.Component"));

    chassisItemIface->register_property("PrettyName", std::string(""));
    chassisItemIface->register_property("Present", true);

    chassisAssetIface->register_property("Manufacturer", manufacturer);
    chassisAssetIface->register_property("Model", model);
    chassisAssetIface->register_property("SerialNumber", serialNumber);

    chassisRevIface->register_property("Version", version);

    {
        AssocList assocs;
        assocs.emplace_back("parent_chassis", "all_chassis", boardPath);
        chassisAssocIface->register_property("Associations", assocs);
    }

    chassisOpStatusIface->register_property("Functional", true);

    chassisInstanceIface->register_property("InstanceNumber", cpuIndex);

    chassisIface->initialize();
    chassisItemIface->initialize();
    chassisAssetIface->initialize();
    chassisRevIface->initialize();
    chassisAssocIface->initialize();
    chassisOpStatusIface->initialize();
    chassisInstanceIface->initialize();
}

} // namespace info
} // namespace nvidia
