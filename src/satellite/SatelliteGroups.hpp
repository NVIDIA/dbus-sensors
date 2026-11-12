// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA CORPORATION & AFFILIATES

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace satellite
{

/** @brief Offset and name of a sensor of a SatelliteGroup record */
using GroupMember = std::pair<uint64_t, std::string>;

/** @brief Returns one Satellite configuration per sensor of a group.
 *
 *  The group holds the settings its sensors share. A sensor takes its name
 *  and offset from its entry, and, when the group has a StaleOffset, the
 *  stale bit FirstStaleBit plus its rank by offset. */
template <typename ConfigMap>
std::vector<ConfigMap> expandGroup(const ConfigMap& group,
                                   std::vector<GroupMember> members,
                                   size_t firstStaleBit)
{
    std::ranges::sort(members);
    std::vector<ConfigMap> sensors;
    for (size_t rank = 0; rank < members.size(); ++rank)
    {
        ConfigMap cfg = group;
        cfg.erase("FirstStaleBit");
        cfg["Name"] = members[rank].second;
        cfg["OffsetValue"] = members[rank].first;
        if (cfg.count("StaleOffset") != 0)
        {
            cfg["StaleBit"] = static_cast<uint64_t>(firstStaleBit + rank);
        }
        sensors.push_back(std::move(cfg));
    }
    return sensors;
}

} // namespace satellite
