/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <gpiod.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

PHOSPHOR_LOG2_USING;

static constexpr const char* busName = "xyz.openbmc_project.Control.Port";
static constexpr const char* objPathBase = "/xyz/openbmc_project/control/port/";
static constexpr const char* ifaceName = "xyz.openbmc_project.Object.Enable";
static constexpr const char* gpioConsumer = "portctrl";
static constexpr const char* stateFileName = "state";
static const std::filesystem::path stateBaseDir = "/var/lib/obmc/portctrl";
static const std::filesystem::path configFile = "/etc/portctrl/config.json";

struct PortConfig
{
    std::string name;
    std::string gpioLineName;
    bool activeLow = false;
};

struct PortInstance
{
    std::string name;
    gpiod::line gpioLine;
    std::filesystem::path stateDirPath;
    std::filesystem::path stateFilePath;
    bool initialState = false;
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface;
};

static int loadConfig(std::vector<PortConfig>& ports)
{
    std::ifstream ifs(configFile);
    if (!ifs)
    {
        error("Failed to open config file {FILE}", "FILE", configFile);
        return -1;
    }

    nlohmann::json portConfigs;
    try
    {
        portConfigs = nlohmann::json::parse(ifs);
    }
    catch (const nlohmann::json::parse_error& e)
    {
        error("Failed to parse config file {FILE}: {ERROR}", "FILE", configFile,
              "ERROR", e.what());
        return -1;
    }

    if (!portConfigs.is_array())
    {
        error("Config file {FILE} must contain a JSON array", "FILE",
              configFile);
        return -1;
    }

    std::unordered_set<std::string> seenNames;
    std::unordered_set<std::string> seenGpioLines;
    for (const auto& entry : portConfigs)
    {
        if (!entry.is_object())
        {
            error("Skipping non-object config entry");
            continue;
        }

        auto nameIt = entry.find("name");
        auto gpioIt = entry.find("gpio_line");
        if (nameIt == entry.end() || !nameIt->is_string() ||
            gpioIt == entry.end() || !gpioIt->is_string())
        {
            error("Skipping config entry with missing or invalid "
                  "name/gpio_line");
            continue;
        }

        PortConfig cfg;
        cfg.name = nameIt->get<std::string>();
        cfg.gpioLineName = gpioIt->get<std::string>();

        auto activeLowIt = entry.find("active_low");
        if (activeLowIt != entry.end() && activeLowIt->is_boolean())
        {
            cfg.activeLow = activeLowIt->get<bool>();
        }

        if (cfg.name.empty() || cfg.gpioLineName.empty())
        {
            error("Skipping config entry with empty name or gpio_line");
            continue;
        }
        if (!seenNames.insert(cfg.name).second)
        {
            error("Duplicate port name {NAME}, skipping", "NAME", cfg.name);
            continue;
        }
        if (!seenGpioLines.insert(cfg.gpioLineName).second)
        {
            error("Duplicate gpio_line {NAME}, skipping", "NAME",
                  cfg.gpioLineName);
            continue;
        }
        ports.push_back(std::move(cfg));
    }

    return 0;
}

static void saveState(const std::filesystem::path& dirPath,
                      const std::filesystem::path& filePath, bool enabled)
{
    try
    {
        std::filesystem::create_directories(dirPath);
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        error("Failed to create state directory {DIR}: {ERROR}", "DIR",
              dirPath.string(), "ERROR", e.what());
        return;
    }

    std::ofstream ofs(filePath);
    if (!ofs)
    {
        error("Failed to open state file {FILE} for writing", "FILE",
              filePath.string());
        return;
    }
    ofs << (enabled ? "true" : "false") << '\n';
    ofs.flush();
    if (ofs.fail())
    {
        error("Failed to write state file {FILE}", "FILE", filePath.string());
    }
}

static bool loadState(const std::filesystem::path& filePath)
{
    std::ifstream ifs(filePath);
    if (!ifs)
    {
        info("State file {FILE} not found, defaulting to disabled", "FILE",
             filePath.string());
        return false;
    }
    std::string val;
    ifs >> val;
    if (ifs.fail())
    {
        error("Failed to read state file {FILE}", "FILE", filePath.string());
        return false;
    }
    return (val == "true");
}

static bool applyState(gpiod::line& line, bool enabled)
{
    try
    {
        line.set_value(enabled ? 1 : 0);
    }
    catch (const std::exception& e)
    {
        error("Failed to set GPIO value: {ERROR}", "ERROR", e.what());
        return false;
    }
    return true;
}

static bool setPortEnabled(PortInstance& port, const bool newVal,
                           bool& storedVal)
{
    if (!applyState(port.gpioLine, newVal))
    {
        return false;
    }
    saveState(port.stateDirPath, port.stateFilePath, newVal);
    storedVal = newVal;
    return true;
}

static int initPorts(const std::vector<PortConfig>& configs,
                     std::vector<PortInstance>& ports)
{
    ports.reserve(configs.size());

    for (const auto& cfg : configs)
    {
        PortInstance port;
        port.name = cfg.name;
        port.stateDirPath = stateBaseDir / cfg.name;
        port.stateFilePath = port.stateDirPath / stateFileName;

        port.gpioLine = gpiod::find_line(cfg.gpioLineName);
        if (!port.gpioLine)
        {
            error("GPIO line {NAME} not found for port {PORT}", "NAME",
                  cfg.gpioLineName, "PORT", cfg.name);
            return -1;
        }

        port.initialState = loadState(port.stateFilePath);

        try
        {
            port.gpioLine.request(
                {gpioConsumer, gpiod::line_request::DIRECTION_OUTPUT,
                 cfg.activeLow ? gpiod::line_request::FLAG_ACTIVE_LOW : 0},
                port.initialState ? 1 : 0);
        }
        catch (const std::exception& e)
        {
            error("Failed to request GPIO line {NAME} for port {PORT}: {ERROR}",
                  "NAME", cfg.gpioLineName, "PORT", cfg.name, "ERROR",
                  e.what());
            return -1;
        }

        info("Port {PORT}: GPIO {NAME}, initial state {STATE}", "PORT",
             cfg.name, "NAME", cfg.gpioLineName, "STATE", port.initialState);

        ports.push_back(std::move(port));
    }

    return 0;
}

/*
 * Boot ordering and fail-closed design:
 *
 * All controlled ports are disabled at every stage of boot:
 *  1. GPIO expander outputs default to safe state on power-up
 *  2. bmc_ready.sh sets GPIO defaults as defense-in-depth
 *  3. This daemon starts after bmc-boot-complete, claims the GPIOs, and
 *     restores persisted Redfish state per port
 *
 * No port is accessible unless explicitly enabled via Redfish.
 */
int main(int /* argc */, char* /* argv */[])
{
    std::vector<PortConfig> configs;
    if (loadConfig(configs) != 0 || configs.empty())
    {
        error("No valid port configurations found in {FILE}", "FILE",
              configFile);
        return 1;
    }

    std::vector<PortInstance> ports;
    if (initPorts(configs, ports) != 0)
    {
        return 1;
    }

    boost::asio::io_context io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);

    sdbusplus::asio::object_server objectServer(systemBus, true);
    objectServer.add_manager("/xyz/openbmc_project/control");

    for (auto& port : ports)
    {
        std::string objPath = std::string(objPathBase) + port.name;
        port.iface =
            objectServer.add_interface(objPath, std::string(ifaceName));

        // port reference is stable: ports vector is not modified after this
        // loop
        port.iface->register_property(
            "Enabled", port.initialState,
            [&port](const bool newVal, bool& storedVal) {
                return setPortEnabled(port, newVal, storedVal);
            },
            [](const bool& storedVal) { return storedVal; });

        port.iface->initialize();
    }

    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait(
        [&io, &ports, &objectServer](const boost::system::error_code&, int) {
            for (auto& port : ports)
            {
                objectServer.remove_interface(port.iface);
                port.gpioLine.release();
            }
            io.stop();
        });

    systemBus->request_name(busName);
    io.run();
    return 0;
}
