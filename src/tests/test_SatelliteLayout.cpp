// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA CORPORATION & AFFILIATES

#include "SatelliteLayout.hpp"

#include <array>
#include <cstdint>
#include <sstream>

#include <gtest/gtest.h>

namespace
{

TEST(SatelliteLayout, HgxConfig)
{
    std::istringstream in("# hgxr\nTelemetryBus=0\nTelemetryAddress=0x54\n"
                          "TempMirrorBus=6\nStartupDelay=5\nnot a key\n");
    auto config = satellite_layout::readConfig(in);
    EXPECT_EQ(config.size(), 4U);
    EXPECT_EQ(config["TelemetryBus"], 0U);
    EXPECT_EQ(config["TelemetryAddress"], 0x54U);
    EXPECT_EQ(config["TempMirrorBus"], 6U);
    EXPECT_EQ(config["StartupDelay"], 5U);
}

TEST(SatelliteLayout, SkipsValuesThatAreNotNumbers)
{
    std::istringstream in("RwfsRecovery=Initramfs\nStartupDelay=5\nBus=\n");
    auto config = satellite_layout::readConfig(in);
    EXPECT_EQ(config.size(), 1U);
    EXPECT_EQ(config["StartupDelay"], 5U);
}

TEST(SatelliteLayout, OffsetFollowsTheLayoutVersion)
{
    EXPECT_EQ(satellite_layout::temperatureOffset(4), 0xEFU);
    EXPECT_EQ(satellite_layout::temperatureOffset(5), 0xF0U);
}

TEST(SatelliteLayout, TemperatureRoundTrips)
{
    for (long milli : {45250L, 0L, -5500L, -40125L, 125999L})
    {
        const std::array<uint8_t, 4> raw =
            satellite_layout::encodeTemperature(milli);
        const double back = satellite_layout::decodeTemperature(raw.data());
        EXPECT_NEAR(back, static_cast<double>(milli) / 1000, 1.0 / 256);
    }
    EXPECT_EQ(satellite_layout::encodeTemperature(45250),
              (std::array<uint8_t, 4>{0x40, 45, 0, 0}));
}

} // namespace
