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

#include <gpiod.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

PHOSPHOR_LOG2_USING;

static constexpr std::string_view busName =
    "xyz.openbmc_project.Control.USB.Port";
static constexpr std::string_view objPath =
    "/xyz/openbmc_project/control/port/USB_0";
static constexpr std::string_view ifaceName =
    "xyz.openbmc_project.Object.Enable";
static constexpr std::string_view stateDir = "/var/lib/obmc/usb-port-ctrl";
static constexpr std::string_view stateFile =
    "/var/lib/obmc/usb-port-ctrl/state";
static constexpr std::string_view configFile = "/etc/usbportctrl/config.json";

static gpiod::line gpioLine;

struct GpioConfig
{
    std::string lineName;
    bool activeLow = false;
};

static GpioConfig loadConfig()
{
    GpioConfig config;
    std::string path(configFile);
    std::ifstream ifs(path);
    if (!ifs)
    {
        error("Failed to open config file {FILE}", "FILE", configFile);
        return config;
    }

    try
    {
        auto gpioConfig = nlohmann::json::parse(ifs);
        config.lineName = gpioConfig.value("gpio_line", "");
        config.activeLow = gpioConfig.value("active_low", false);
    }
    catch (const nlohmann::json::exception& e)
    {
        error("Failed to parse config file {FILE}: {ERROR}", "FILE", configFile,
              "ERROR", e.what());
    }

    return config;
}

static void saveState(bool enabled)
{
    try
    {
        std::filesystem::create_directories(std::string(stateDir));
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        error("Failed to create state directory {DIR}: {ERROR}", "DIR",
              stateDir, "ERROR", e.what());
        return;
    }

    std::string stateFilePath(stateFile);
    std::ofstream ofs(stateFilePath);
    if (!ofs)
    {
        error("Failed to open state file {FILE} for writing", "FILE",
              stateFile);
        return;
    }
    ofs << (enabled ? "true" : "false") << std::endl;
    ofs.flush();
    if (ofs.fail())
    {
        error("Failed to write state file {FILE}", "FILE", stateFile);
    }
}

static bool loadState()
{
    std::string stateFilePath(stateFile);
    std::ifstream ifs(stateFilePath);
    if (!ifs)
    {
        info("State file {FILE} not found, defaulting to disabled", "FILE",
             stateFile);
        return false;
    }
    std::string val;
    ifs >> val;
    if (ifs.fail())
    {
        error("Failed to read state file {FILE}", "FILE", stateFile);
        return false;
    }
    return (val == "true");
}

static bool applyState(bool enabled)
{
    try
    {
        gpioLine.set_value(enabled ? 1 : 0);
    }
    catch (const std::exception& e)
    {
        error("Failed to set GPIO value: {ERROR}", "ERROR", e.what());
        return false;
    }
    return true;
}

static bool setEnabled(const bool newVal, bool& storedVal)
{
    if (!applyState(newVal))
    {
        return false;
    }
    saveState(newVal);
    storedVal = newVal;
    return true;
}

/*
 * Boot ordering and fail-closed design:
 *
 * The USB-C host port is disabled at every stage of boot:
 *  1. PCA9555 outputs default to HIGH on power-up (active-low = disabled)
 *  2. bmc_ready.sh sets USB_MUX_EN-O HIGH as defense-in-depth
 *  3. This daemon starts after bmc-boot-complete, claims the GPIO, and
 *     restores the persisted Redfish state
 *
 * The port is never accessible unless explicitly enabled via Redfish.
 */
int main(int /* argc */, char* /* argv */[])
{
    GpioConfig config = loadConfig();
    if (config.lineName.empty())
    {
        error("gpio_line not specified in config file {FILE}", "FILE",
              configFile);
        return 1;
    }

    gpioLine = gpiod::find_line(config.lineName);
    if (!gpioLine)
    {
        error("GPIO line {NAME} not found", "NAME", config.lineName);
        return 1;
    }

    bool initialState = loadState();

    try
    {
        gpioLine.request(
            {"usbportctrl", gpiod::line_request::DIRECTION_OUTPUT,
             config.activeLow ? gpiod::line_request::FLAG_ACTIVE_LOW : 0},
            initialState ? 1 : 0);
    }
    catch (const std::exception& e)
    {
        error("Failed to request GPIO line {NAME}: {ERROR}", "NAME",
              config.lineName, "ERROR", e.what());
        return 1;
    }

    boost::asio::io_context io;
    std::shared_ptr<sdbusplus::asio::connection> conn =
        std::make_shared<sdbusplus::asio::connection>(io);

    sdbusplus::asio::object_server server(conn);
    std::shared_ptr<sdbusplus::asio::dbus_interface> ifaceObj =
        server.add_interface(std::string(objPath), std::string(ifaceName));

    ifaceObj->register_property(
        "Enabled", initialState, setEnabled,
        [](const bool& storedVal) { return storedVal; });

    ifaceObj->initialize();
    conn->request_name(busName.data());
    io.run();
    return 0;
}
