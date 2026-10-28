// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA CORPORATION & AFFILIATES

// virtual-i2c-target: the HMC side of the BMC<->HMC I2C mailbox EEPROMs.
// At start it creates the telemetry and control I2C target EEPROMs (and on
// HGX the FPGA's copy), writes the telemetry layout version at byte 24 and,
// where configured, the HBM DRAM padding. Then, every 0.5 s, it writes the
// TMP75's reading and the 85 C threshold, 8 bytes, at the offset of that
// layout version, and the same 8 bytes at offset 0 of the FPGA's copy.
// hmc-mailbox (phosphor-debug-collector) serves the rest of the EEPROMs.
//
// /usr/share/virtual-i2c-target/virtual-i2c-target.conf, key=value:
// TelemetryBus, ControlBus (required), TelemetryAddress (0x4f),
// ControlAddress (0x45), LayoutVersion (5), TempMirrorBus,
// PadHbmDramTemps (0/1), StartupDelay (s before the first temperature).

#include "SatelliteLayout.hpp"

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace
{

namespace fs = std::filesystem;

constexpr auto configFile =
    "/usr/share/virtual-i2c-target/virtual-i2c-target.conf";

std::string slaveEeprom(unsigned bus, unsigned address)
{
    return std::format("/sys/bus/i2c/devices/{}-10{:02x}/slave-eeprom", bus,
                       address);
}

void eepromWrite(const std::string& path, std::size_t offset,
                 std::span<const uint8_t> data)
{
    const std::string s(data.begin(), data.end());
    std::fstream f{path, std::ios::in | std::ios::out | std::ios::binary};
    f.seekp(static_cast<std::streamoff>(offset));
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
    f.flush();
    if (!f)
    {
        lg2::error("cannot write {COUNT} bytes to {PATH} at {OFFSET}", "COUNT",
                   data.size(), "PATH", path, "OFFSET", offset);
    }
}

// What hmc-temp-sensor.sh did: once the TMP75 is there, set its alarm to
// 85 C, then publish its reading and the threshold, 85000 as an integer.
void updateTemp(std::optional<fs::path>& hwmon, bool& missingLogged,
                const std::string& telemetry, std::size_t offset,
                const std::string& mirror)
{
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(
             hwmon ? fs::path{} : fs::path{"/sys/class/hwmon"}, ec))
    {
        std::string name;
        std::getline(std::ifstream(e.path() / "name"), name);
        if (name.starts_with("tmp75"))
        {
            if (fs::exists(e.path() / "temp1_max"))
            {
                std::ofstream(e.path() / "temp1_max") << 85000;
                hwmon = e.path();
            }
            break;
        }
    }
    if (!hwmon)
    {
        if (!missingLogged)
        {
            lg2::warning("HMC TMP75 not found yet; HMC temperature not "
                         "published");
            missingLogged = true;
        }
        return;
    }
    long milli = 0;
    std::ifstream(*hwmon / "temp1_input") >> milli;
    std::array<uint8_t, 8> temp{0, 0, 0, 0, 0x00, 0x08, 0x4c, 0x01};
    std::ranges::copy(satellite_layout::encodeTemperature(milli), temp.begin());
    eepromWrite(telemetry, offset, temp);
    if (!mirror.empty())
    {
        eepromWrite(mirror, 0, temp);
    }
}

// Creates the target EEPROM <type> at <bus>-10<address> unless it exists.
void createTarget(unsigned bus, unsigned address, std::string_view type)
{
    if (fs::exists(std::format("/sys/bus/i2c/devices/i2c-{}/{}-10{:02x}/name",
                               bus, bus, address)))
    {
        return;
    }
    std::ofstream f(std::format("/sys/bus/i2c/devices/i2c-{}/new_device", bus));
    f << std::format("{} 0x10{:02x}\n", type, address);
    f.flush();
    if (!f)
    {
        lg2::error("cannot create {TYPE} at {BUS}-10{ADDRESS}", "TYPE", type,
                   "BUS", bus, "ADDRESS", std::format("{:02x}", address));
    }
}

// HBM DRAM temperatures are deprecated (DGXOPENBMC-25509): their telemetry
// bytes read 0xff, "data not present", with their stale bits set.
void padHbmDramTemps(const std::string& telemetry)
{
    std::array<uint8_t, 28> gpu1To7{};
    gpu1To7.fill(0xff);
    eepromWrite(telemetry, 0x19, gpu1To7);
    eepromWrite(telemetry, 0x125,
                std::to_array<uint8_t>({0xff, 0xff, 0xff, 0xff}));
    for (auto [offset, mask] : {std::pair<std::size_t, uint8_t>{0x39, 0x7f},
                                std::pair<std::size_t, uint8_t>{0x145, 0x01}})
    {
        std::ifstream in(telemetry, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(offset));
        const int stale = in.get();
        if (stale != std::char_traits<char>::eof())
        {
            eepromWrite(telemetry, offset,
                        std::to_array({static_cast<uint8_t>(stale | mask)}));
        }
    }
}

} // namespace

int main()
{
    const auto start = std::chrono::steady_clock::now();
    std::ifstream conf(configFile);
    auto config = satellite_layout::readConfig(conf);
    if (!config.contains("TelemetryBus") || !config.contains("ControlBus"))
    {
        lg2::error("{FILE} must set TelemetryBus and ControlBus", "FILE",
                   configFile);
        return 1;
    }
    const unsigned telemetryBus = config["TelemetryBus"];
    const unsigned address =
        config.contains("TelemetryAddress") ? config["TelemetryAddress"] : 0x4f;
    const unsigned control =
        config.contains("ControlAddress") ? config["ControlAddress"] : 0x45;
    const int version = config.contains("LayoutVersion")
                            ? static_cast<int>(config["LayoutVersion"])
                            : 5;

    createTarget(telemetryBus, address, "slave-24c512ro");
    createTarget(config["ControlBus"], control, "slave-24c02");
    std::string mirror;
    if (config.contains("TempMirrorBus"))
    {
        createTarget(config["TempMirrorBus"], address, "slave-24c512ro");
        mirror = slaveEeprom(config["TempMirrorBus"], address);
    }

    const std::string telemetry = slaveEeprom(telemetryBus, address);
    if (!fs::exists(telemetry))
    {
        lg2::error("No telemetry EEPROM {PATH}; waiting", "PATH", telemetry);
    }
    while (!fs::exists(telemetry))
    {
        std::this_thread::sleep_for(std::chrono::seconds{1});
    }
    eepromWrite(telemetry, 24, std::to_array({static_cast<uint8_t>(version)}));
    if (config["PadHbmDramTemps"] != 0)
    {
        padHbmDramTemps(telemetry);
    }

    std::this_thread::sleep_until(
        start + std::chrono::seconds{config["StartupDelay"]});

    const std::size_t offset = satellite_layout::temperatureOffset(version);
    std::optional<fs::path> hwmon;
    bool missingLogged = false;
    while (true)
    {
        updateTemp(hwmon, missingLogged, telemetry, offset, mirror);
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
    }
}
