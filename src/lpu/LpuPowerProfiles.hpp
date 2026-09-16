// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
#pragma once

#include "LpuCommon.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace lpu::power
{
// Internal D-Bus row: mode name, configured whole-tray allocation (W),
// description.
using PowerProfile = std::tuple<std::string, uint32_t, std::string>;
using PowerProfiles = std::vector<PowerProfile>;
using ProfileSnapshot = std::tuple<std::string, PowerProfiles, std::string>;
inline constexpr std::string_view profileConfigInterface =
    "xyz.openbmc_project.Configuration.LPU_Power_Profile";

struct ProfileCatalog
{
    PowerProfiles profiles;
    std::string status;

    static std::optional<ProfileCatalog> parse(
        const SensorBaseConfigMap& config)
    {
        const auto name = config.find("Name");
        const auto scope = config.find("Scope");
        const auto status = config.find("Status");
        const auto maxQWatts = config.find("MaxQWatts");
        const auto maxPWatts = config.find("MaxPWatts");
        const auto maxQDescription = config.find("MaxQDescription");
        const auto maxPDescription = config.find("MaxPDescription");
        if (name == config.end() || scope == config.end() ||
            status == config.end() || maxQWatts == config.end() ||
            maxPWatts == config.end() || maxQDescription == config.end() ||
            maxPDescription == config.end())
        {
            return std::nullopt;
        }

        const auto* nameValue = std::get_if<std::string>(&name->second);
        const auto* scopeValue = std::get_if<std::string>(&scope->second);
        const auto* statusValue = std::get_if<std::string>(&status->second);
        const auto* maxQWattsValue =
            std::get_if<uint64_t>(&maxQWatts->second);
        const auto* maxPWattsValue =
            std::get_if<uint64_t>(&maxPWatts->second);
        const auto* maxQDescriptionValue =
            std::get_if<std::string>(&maxQDescription->second);
        const auto* maxPDescriptionValue =
            std::get_if<std::string>(&maxPDescription->second);
        if (nameValue == nullptr || *nameValue != "TotalLPU_Power_0" ||
            scopeValue == nullptr || *scopeValue != "WholeTray" ||
            statusValue == nullptr ||
            (*statusValue != "Tentative" && *statusValue != "Final") ||
            maxQWattsValue == nullptr || maxPWattsValue == nullptr ||
            maxQDescriptionValue == nullptr ||
            maxPDescriptionValue == nullptr)
        {
            return std::nullopt;
        }

        if (*maxQWattsValue == 0 || *maxPWattsValue == 0 ||
            *maxQWattsValue > std::numeric_limits<uint32_t>::max() ||
            *maxPWattsValue > std::numeric_limits<uint32_t>::max() ||
            *maxQWattsValue >= *maxPWattsValue ||
            maxQDescriptionValue->empty() || maxPDescriptionValue->empty())
        {
            return std::nullopt;
        }

        ProfileCatalog result;
        result.status = *statusValue;
        result.profiles.emplace_back("MaxQ",
                                     static_cast<uint32_t>(*maxQWattsValue),
                                     *maxQDescriptionValue);
        result.profiles.emplace_back("MaxP",
                                     static_cast<uint32_t>(*maxPWattsValue),
                                     *maxPDescriptionValue);
        return result;
    }

    static std::optional<ProfileCatalog> fromManagedObjects(
        const ManagedObjectType& objects)
    {
        std::optional<ProfileCatalog> result;
        for (const auto& [path, interfaces] : objects)
        {
            (void)path;
            const auto config =
                interfaces.find(std::string(profileConfigInterface));
            if (config == interfaces.end())
            {
                continue;
            }
            // There is one whole-tray catalogue. Multiple matching records are
            // ambiguous and therefore treated as invalid configuration.
            if (result)
            {
                return std::nullopt;
            }
            result = parse(config->second);
            if (!result)
            {
                return std::nullopt;
            }
        }
        return result;
    }
};
} // namespace lpu::power
