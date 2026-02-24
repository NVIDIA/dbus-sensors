/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
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

#include "PortStateUtils.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <string>
#include <vector>

PHOSPHOR_LOG2_USING;

static constexpr const char* busName =
    "xyz.openbmc_project.Control.DebugSerial";
static constexpr const char* objPath =
    "/xyz/openbmc_project/control/port/debug_serial0";
static constexpr const char* ifaceName = "xyz.openbmc_project.Object.Enable";
static constexpr const char* portInfoIfaceName =
    "xyz.openbmc_project.Inventory.Decorator.PortInfo";
static constexpr const char* itemIfaceName =
    "xyz.openbmc_project.Inventory.Item";
static constexpr const char* stateDir = "/var/lib/obmc/debugserialctrl";
static constexpr const char* stateFile = "/var/lib/obmc/debugserialctrl/state";

static const std::vector<std::string> consoleUnits = {
    "obmc-console-ttyACM0.socket",
};

static bool controlUnit(sdbusplus::asio::connection& conn, const char* method,
                        const std::string& unit)
{
    try
    {
        auto msg = conn.new_method_call(
            "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager", method);
        msg.append(unit, "replace");
        conn.call(msg);
    }
    catch (const std::exception& e)
    {
        error("Failed to {METHOD} {UNIT}: {ERROR}", "METHOD", method, "UNIT",
              unit, "ERROR", e.what());
        return false;
    }
    return true;
}

static void resetFailedUnit(sdbusplus::asio::connection& conn,
                            const std::string& unit)
{
    try
    {
        auto msg = conn.new_method_call(
            "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager", "ResetFailedUnit");
        msg.append(unit);
        conn.call(msg);
    }
    catch (const std::exception& e)
    {
        debug("ResetFailedUnit {UNIT}: {ERROR}", "UNIT", unit, "ERROR",
              e.what());
    }
}

static bool applyState(sdbusplus::asio::connection& conn, bool enabled)
{
    if (enabled)
    {
        for (const auto& unit : consoleUnits)
        {
            resetFailedUnit(conn, unit);
        }
    }
    const char* method = enabled ? "StartUnit" : "StopUnit";
    for (const auto& unit : consoleUnits)
    {
        if (!controlUnit(conn, method, unit))
        {
            return false;
        }
    }
    return true;
}

static bool setEnabled(sdbusplus::asio::connection& conn, const bool newVal,
                       bool& storedVal)
{
    if (!applyState(conn, newVal))
    {
        return false;
    }
    port_state::saveState(stateDir, stateFile, newVal);
    storedVal = newVal;
    return true;
}

struct DebugSerialPort
{
    DebugSerialPort() = default;
    DebugSerialPort(const DebugSerialPort&) = delete;
    DebugSerialPort& operator=(const DebugSerialPort&) = delete;
    DebugSerialPort(DebugSerialPort&&) = default;
    DebugSerialPort& operator=(DebugSerialPort&&) = default;

    ~DebugSerialPort()
    {
        if (objectServer)
        {
            if (enableIface)
            {
                objectServer->remove_interface(enableIface);
            }
            if (portInfoIface)
            {
                objectServer->remove_interface(portInfoIface);
            }
            if (itemIface)
            {
                objectServer->remove_interface(itemIface);
            }
        }
    }

    std::shared_ptr<sdbusplus::asio::dbus_interface> enableIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> portInfoIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> itemIface;
    sdbusplus::asio::object_server* objectServer = nullptr;
};

/*
 * Boot ordering and fail-open design:
 *
 * The debug serial console is enabled by default at every stage of boot:
 *  1. obmc-console services start normally via systemd
 *  2. This daemon starts after bmc-boot-complete and restores
 *     persisted state
 *  3. If persisted state is false, the daemon stops the console
 *     services
 *
 * The port is always accessible unless explicitly disabled via
 * Redfish.
 */
int main()
{
    bool initialState = port_state::loadState(stateFile, true);

    boost::asio::io_context io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);

    applyState(*systemBus, initialState);

    sdbusplus::asio::object_server objectServer(systemBus, true);
    objectServer.add_manager("/xyz/openbmc_project/control");

    DebugSerialPort port;
    port.objectServer = &objectServer;

    port.enableIface = objectServer.add_interface(std::string(objPath),
                                                  std::string(ifaceName));
    port.enableIface->register_property(
        "Enabled", initialState,
        [&systemBus](const bool newVal, bool& storedVal) {
            return setEnabled(*systemBus, newVal, storedVal);
        },
        [](const bool& storedVal) { return storedVal; });
    port.enableIface->initialize();

    port.portInfoIface = objectServer.add_interface(
        std::string(objPath), std::string(portInfoIfaceName));
    port.portInfoIface->register_property(
        "Protocol", std::string("xyz.openbmc_project.Inventory.Decorator"
                                ".PortInfo.PortProtocol.USB"));
    port.portInfoIface->register_property(
        "Type", std::string("xyz.openbmc_project.Inventory.Decorator"
                            ".PortInfo.PortType.DownstreamPort"));
    port.portInfoIface->initialize();

    port.itemIface = objectServer.add_interface(std::string(objPath),
                                                std::string(itemIfaceName));
    port.itemIface->register_property(
        "PrettyName", std::string("SMM USB-C Debug Serial Port"));
    port.itemIface->register_property("Present", true);
    port.itemIface->initialize();

    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&io](const boost::system::error_code&, int) {
        io.stop();
    });

    systemBus->request_name(busName);
    io.run();
    return 0;
}
