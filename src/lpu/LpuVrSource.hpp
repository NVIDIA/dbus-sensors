#pragma once

#include "../Utils.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace lpu_vr_source
{

inline constexpr std::string_view configType = "LPU_VR_Metrics";

inline bool isSelectableConfig(const SensorBaseConfigMap& config)
{
    const auto type = config.find("Type");
    if (type == config.end())
    {
        return false;
    }

    const std::string* typeName = std::get_if<std::string>(&type->second);
    return typeName != nullptr && *typeName == configType;
}

inline std::optional<uint64_t> instanceFromName(std::string_view name)
{
    constexpr std::string_view prefix = "LPU_Module_";
    if (!name.starts_with(prefix))
    {
        return std::nullopt;
    }

    size_t end = prefix.size();
    while (end < name.size() && name[end] >= '0' && name[end] <= '9')
    {
        ++end;
    }
    if (end == prefix.size() || (end < name.size() && name[end] != '_'))
    {
        return std::nullopt;
    }

    uint64_t instance = 0;
    const std::from_chars_result parsed = std::from_chars(
        name.data() + prefix.size(), name.data() + end, instance);
    if (parsed.ec != std::errc{} || parsed.ptr != name.data() + end)
    {
        return std::nullopt;
    }
    return instance;
}

inline std::optional<uint64_t> instanceFromConfig(
    const SensorBaseConfigMap& config)
{
    const auto name = config.find("Name");
    if (name == config.end())
    {
        return std::nullopt;
    }
    const std::string* value = std::get_if<std::string>(&name->second);
    return value == nullptr ? std::nullopt : instanceFromName(*value);
}

inline std::optional<uint64_t> busFromDeviceName(std::string_view device)
{
    constexpr std::string_view suffix = "-0067";
    if (!device.ends_with(suffix) || device.size() == suffix.size())
    {
        return std::nullopt;
    }

    const std::string_view busText =
        device.substr(0, device.size() - suffix.size());
    uint64_t bus = 0;
    const std::from_chars_result parsed =
        std::from_chars(busText.data(), busText.data() + busText.size(), bus);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != busText.data() + busText.size())
    {
        return std::nullopt;
    }
    return bus;
}

using InstanceBusMap = std::map<uint64_t, uint64_t>;

inline std::set<uint64_t> configuredInstances(
    const ManagedObjectType& managedObjects)
{
    std::set<uint64_t> instances;
    for (const auto& [path, interfaces] : managedObjects)
    {
        (void)path;
        for (const auto& [interface, config] : interfaces)
        {
            (void)interface;
            if (isSelectableConfig(config))
            {
                if (const auto instance = instanceFromConfig(config))
                {
                    instances.emplace(*instance);
                }
            }
        }
    }
    return instances;
}

inline InstanceBusMap instanceBuses()
{
    InstanceBusMap buses;
    std::set<uint64_t> conflictingInstances;
    const std::filesystem::path devices("/sys/bus/i2c/devices");
    std::error_code error;
    std::filesystem::directory_iterator entry(devices, error);
    const std::filesystem::directory_iterator end;

    while (!error && entry != end)
    {
        const std::filesystem::path sysfs = entry->path() / "lpu850";
        const std::string device = entry->path().filename().string();
        const std::optional<uint64_t> bus = busFromDeviceName(device);
        if (bus)
        {
            std::ifstream slotFile(sysfs / "slot_index");
            uint64_t slot = 0;
            if ((slotFile >> slot) && slot >= 1U && slot <= 256U)
            {
                // Kernel DT slot indices are 1-based; EM names are 0-based.
                const uint64_t instance = slot - 1U;
                if (!conflictingInstances.contains(instance))
                {
                    const auto [existing, inserted] =
                        buses.emplace(instance, *bus);
                    if (!inserted && existing->second != *bus)
                    {
                        buses.erase(existing);
                        conflictingInstances.emplace(instance);
                    }
                }
            }
        }
        entry.increment(error);
    }
    return buses;
}

} // namespace lpu_vr_source
