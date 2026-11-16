// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA CORPORATION & AFFILIATES

#include "SatelliteGroups.hpp"

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>

#include <gtest/gtest.h>

namespace
{

using Config =
    std::map<std::string, std::variant<uint64_t, double, std::string>>;

Config makeGroup()
{
    return {{"Type", std::string("SatelliteGroup")},
            {"SensorType", std::string("Power")},
            {"StaleOffset", uint64_t{107}},
            {"FirstStaleBit", uint64_t{2}}};
}

TEST(SatelliteGroups, OneConfigurationPerSensorOrderedByOffset)
{
    auto sensors = satellite::expandGroup(
        makeGroup(), {{79, "GPU_1_Pwr"}, {75, "GPU_0_Pwr"}, {83, "GPU_2_Pwr"}},
        2);
    ASSERT_EQ(sensors.size(), 3U);
    EXPECT_EQ(std::get<std::string>(sensors[0]["Name"]), "GPU_0_Pwr");
    EXPECT_EQ(std::get<uint64_t>(sensors[0]["OffsetValue"]), 75U);
    EXPECT_EQ(std::get<std::string>(sensors[2]["Name"]), "GPU_2_Pwr");
    EXPECT_EQ(std::get<uint64_t>(sensors[2]["OffsetValue"]), 83U);
}

TEST(SatelliteGroups, StaleBitIsTheFirstBitPlusTheRank)
{
    auto sensors = satellite::expandGroup(makeGroup(),
                                          {{79, "B"}, {75, "A"}, {83, "C"}}, 2);
    EXPECT_EQ(std::get<uint64_t>(sensors[0]["StaleBit"]), 2U);
    EXPECT_EQ(std::get<uint64_t>(sensors[1]["StaleBit"]), 3U);
    EXPECT_EQ(std::get<uint64_t>(sensors[2]["StaleBit"]), 4U);
}

TEST(SatelliteGroups, NoStaleBitWithoutAStaleOffset)
{
    auto group = makeGroup();
    group.erase("StaleOffset");
    auto sensors = satellite::expandGroup(group, {{75, "A"}, {79, "B"}}, 0);
    ASSERT_EQ(sensors.size(), 2U);
    EXPECT_EQ(sensors[0].count("StaleBit"), 0U);
}

TEST(SatelliteGroups, KeepsTheSharedSettingsAndDropsFirstStaleBit)
{
    auto sensors = satellite::expandGroup(makeGroup(), {{75, "A"}}, 2);
    ASSERT_EQ(sensors.size(), 1U);
    EXPECT_EQ(std::get<std::string>(sensors[0]["SensorType"]), "Power");
    EXPECT_EQ(std::get<uint64_t>(sensors[0]["StaleOffset"]), 107U);
    EXPECT_EQ(sensors[0].count("FirstStaleBit"), 0U);
}

TEST(SatelliteGroups, StaleBitsMayPassBitSeven)
{
    auto sensors = satellite::expandGroup(makeGroup(), {{1, "A"}, {2, "B"}}, 7);
    ASSERT_EQ(sensors.size(), 2U);
    EXPECT_EQ(std::get<uint64_t>(sensors[1]["StaleBit"]), 8U);
}

TEST(SatelliteGroups, TheStaleBitSaturatesInsteadOfWrapping)
{
    auto sensors =
        satellite::expandGroup(makeGroup(), {{1, "A"}, {2, "B"}}, SIZE_MAX);
    ASSERT_EQ(sensors.size(), 2U);
    EXPECT_EQ(std::get<uint64_t>(sensors[1]["StaleBit"]),
              static_cast<uint64_t>(SIZE_MAX));
}

TEST(SatelliteGroups, ABoardWithoutStaleCheckClearsTheStaleCheck)
{
    auto stale = satellite::resolveStaleCheck(107, 8, false);
    EXPECT_EQ(stale.offset, 0U);
    EXPECT_EQ(stale.bit, 0U);
}

TEST(SatelliteGroups, ACheckedBoardRejectsAStaleBitPastBitSeven)
{
    EXPECT_THROW(satellite::resolveStaleCheck(107, 8, true),
                 std::invalid_argument);
    auto stale = satellite::resolveStaleCheck(107, 7, true);
    EXPECT_EQ(stale.offset, 107U);
    EXPECT_EQ(stale.bit, 7U);
}

TEST(SatelliteGroups, ASensorWithoutAStaleOffsetKeepsNoStaleCheck)
{
    auto stale = satellite::resolveStaleCheck(0, 0, true);
    EXPECT_EQ(stale.offset, 0U);
}

TEST(SatelliteGroups, AnEmptyGroupHasNoSensors)
{
    EXPECT_TRUE(satellite::expandGroup(makeGroup(), {}, 0).empty());
}

} // namespace
