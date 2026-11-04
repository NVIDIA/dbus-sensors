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

#include <phosphor-logging/lg2.hpp>

#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

namespace nvidia
{
namespace info
{

static std::string formatCpuSerialNumber(const std::string& serialNumber)
{
    constexpr size_t wordCount = 4;
    constexpr size_t wordWidth = 8;
    constexpr size_t wordStride = wordWidth + 1;
    constexpr size_t serialLength = wordCount * wordStride - 1;

    // Expect 35 characters: four 8-digit hex words and three hyphens.
    // Preserve the input if its length or format does not match.
    if (serialNumber.size() != serialLength)
    {
        return serialNumber;
    }

    const std::string_view serial{serialNumber};
    std::array<uint32_t, wordCount> words{};

    for (size_t i = 0; i < words.size(); ++i)
    {
        // Extract and parse one 8-digit hex word.
        const auto word = serial.substr(i * wordStride, wordWidth);
        const auto [end, ec] = std::from_chars(
            word.data(), word.data() + word.size(), words[i], 16);

        if (ec != std::errc{} || end != word.data() + word.size())
        {
            return serialNumber;
        }

        // A hyphen must follow each word except the last.
        if (i + 1 < wordCount && serial[(i + 1) * wordStride - 1] != '-')
        {
            return serialNumber;
        }
    }

    // Reverse the word order and byte-swap each word to match SMBIOS Type 4.
    return std::format("{:08X}{:08X}{:08X}{:08X}", std::byteswap(words[3]),
                       std::byteswap(words[2]), std::byteswap(words[1]),
                       std::byteswap(words[0]));
}

void from_json(const Json& j, NvidiaCpu& c)
{
    j.at("Socket").get_to(c.socketNum);
    j.at("Family").get_to(c.family);
    j.at("Id").get_to(c.idStr);
    j.at("CoreCount").get_to(c.coreCount);
    j.at("ThreadCount").get_to(c.threadCount);
    j.at("MaxSpeedInMhz").get_to(c.maxSpeedInMhz);
    j.at("Manufacturer").get_to(c.manufacturer);
    j.at("Model").get_to(c.model);
    j.at("ModelRevision").get_to(c.modelRevision);
    j.at("SerialNumber").get_to(c.serialNumber);
    c.serialNumber = formatCpuSerialNumber(c.serialNumber);
    j.at("Version").get_to(c.version);
    j.at("SKU").get_to(c.sku);
}

void NvidiaCpu::validate()
{
    // Schema already validated shape; just hex-parse idStr into idValue.
    std::string_view sv(idStr);
    if (sv.size() >= 2 && sv[0] == '0' && (sv[1] == 'x' || sv[1] == 'X'))
    {
        sv.remove_prefix(2);
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

void NvidiaCpu::publish(sdbusplus::asio::object_server& objServer,
                        const std::string& cpuPath,
                        const std::string& componentPath,
                        const std::string& boardPath, uint64_t cpuIndex)
{
    using AssocTuple = std::tuple<std::string, std::string, std::string>;
    using AssocList = std::vector<AssocTuple>;

    const std::string cpuName = "CPU_" + std::to_string(cpuIndex);

    // --- cpuPath: 8 interfaces -------------------------------------------
    auto& cpu =
        add(cpuPath, "xyz.openbmc_project.Inventory.Item.Cpu", objServer);
    cpu.register_property("Socket", std::to_string(socketNum));
    cpu.register_property("Family", family);
    cpu.register_property("Id", idValue);
    cpu.register_property("CoreCount", coreCount);
    cpu.register_property("ThreadCount", threadCount);
    cpu.register_property("MaxSpeedInMhz", maxSpeedInMhz);
    cpu.register_property("ModelRevision", modelRevision);
    cpu.register_property("ProcessorType", std::string("CPU"));

    auto& item = add(cpuPath, "xyz.openbmc_project.Inventory.Item", objServer);
    item.register_property("PrettyName", model);
    item.register_property("Present", true);

    auto& asset = add(cpuPath, "xyz.openbmc_project.Inventory.Decorator.Asset",
                      objServer);
    asset.register_property("Manufacturer", manufacturer);
    asset.register_property("Model", model);
    asset.register_property("SerialNumber", serialNumber);
    asset.register_property("Name", cpuName);

    auto& rev = add(cpuPath, "xyz.openbmc_project.Inventory.Decorator.Revision",
                    objServer);
    rev.register_property("Version", version);

    auto& instance = add(
        cpuPath, "xyz.openbmc_project.Inventory.Decorator.Instance", objServer);
    instance.register_property("InstanceNumber", cpuIndex);

    auto& skuIf =
        add(cpuPath, "xyz.openbmc_project.Inventory.Decorator.SKU", objServer);
    skuIf.register_property("SKU", sku);

    auto& assoc =
        add(cpuPath, "xyz.openbmc_project.Association.Definitions", objServer);
    {
        AssocList assocs;
        assocs.emplace_back("chassis", "all_processors", componentPath);
        assoc.register_property("Associations", assocs);
    }

    auto& opStatus =
        add(cpuPath, "xyz.openbmc_project.State.Decorator.OperationalStatus",
            objServer);
    opStatus.register_property("Functional", true);

    // --- componentPath: 7 merged chassis-component interfaces ------------
    auto& chassis = add(
        componentPath, "xyz.openbmc_project.Inventory.Item.Chassis", objServer);
    chassis.register_property(
        "Type", std::string("xyz.openbmc_project.Inventory.Item.Chassis."
                            "ChassisType.Component"));

    auto& chassisItem =
        add(componentPath, "xyz.openbmc_project.Inventory.Item", objServer);
    chassisItem.register_property("PrettyName", std::string(""));
    chassisItem.register_property("Present", true);

    auto& chassisAsset =
        add(componentPath, "xyz.openbmc_project.Inventory.Decorator.Asset",
            objServer);
    chassisAsset.register_property("Manufacturer", manufacturer);
    chassisAsset.register_property("Model", model);
    chassisAsset.register_property("SerialNumber", serialNumber);

    auto& chassisRev =
        add(componentPath, "xyz.openbmc_project.Inventory.Decorator.Revision",
            objServer);
    chassisRev.register_property("Version", version);

    auto& chassisAssoc =
        add(componentPath, "xyz.openbmc_project.Association.Definitions",
            objServer);
    {
        AssocList assocs;
        assocs.emplace_back("parent_chassis", "all_chassis", boardPath);
        chassisAssoc.register_property("Associations", assocs);
    }

    auto& chassisOpStatus =
        add(componentPath,
            "xyz.openbmc_project.State.Decorator.OperationalStatus", objServer);
    chassisOpStatus.register_property("Functional", true);

    auto& chassisInstance =
        add(componentPath, "xyz.openbmc_project.Inventory.Decorator.Instance",
            objServer);
    chassisInstance.register_property("InstanceNumber", cpuIndex);

    initializeAll();

    lg2::info("Published CPU at {P}", "P", cpuPath);
}

} // namespace info
} // namespace nvidia
