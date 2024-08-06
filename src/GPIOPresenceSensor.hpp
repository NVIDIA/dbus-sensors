/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024. All rights
 * reserved. SPDX-License-Identifier: Apache-2.0
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
#pragma once

#include <boost/asio.hpp>
#include <boost/container/flat_map.hpp>
#include <gpiod.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>

#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace gpio_presence_sensing
{

static const std::chrono::seconds pollRateDefault = std::chrono::seconds(5);

static constexpr const char* service = "xyz.openbmc_project.presence-detect";
static constexpr const char* inventoryObjPath =
    "/xyz/openbmc_project/inventory/item/";
static constexpr const char* inventoryCableObjPath =
    "/xyz/openbmc_project/inventory/system/cable/";

namespace properties
{
static constexpr const char* propertyName = "Name";
static constexpr const char* propertyGpioLine = "GpioLine";
static constexpr const char* propertyPolarity = "Polarity";
static constexpr const char* propertyPresent = "Present";
} // namespace properties

namespace interfaces
{
static constexpr const char* emGPIOCableSensingIfc =
    "xyz.openbmc_project.Configuration.GPIOCableSensing";
static constexpr const char* statusIfc = "xyz.openbmc_project.Inventory.Item";
static constexpr const char* statusCableIfc =
    "xyz.openbmc_project.Inventory.Item.Cable";
} // namespace interfaces

struct Config
{
    std::string name;
    // interface gpio pin.
    std::string gpioLine;
    // GPIO Polarity.
    bool activeLow{};
    // Presence signal.
    bool present{};
};

// Actively listen to the config information from EntityManager and calls the
// callback function once a config is available.
class GPIOPresence : public std::enable_shared_from_this<GPIOPresence>
{
  public:
    explicit GPIOPresence(
        const std::shared_ptr<sdbusplus::asio::connection>& bus) :
        bus(bus),
        timer(bus->get_io_context())
    {}

    // Add a dbus object to the reference list.
    // @params statusIfc: pointer to object status interface.
    // @params objPath: the dbus object path.
    // @params config: EM config
    void addObj(std::unique_ptr<sdbusplus::asio::dbus_interface>& statusIfc,
                const std::string& objPath, const Config& config);

    // Remove a object from the object reference list.
    void removeObj(const std::string& objPath);

    // Check if a object is included in the obj->iface map already.
    bool hasObj(const std::string& objPath);

    void readPresent();

    void startGPIOEventMonitor(boost::asio::io_context& gpioContext);

  private:
    struct ObjIfaces
    {
        std::unique_ptr<sdbusplus::asio::dbus_interface> statusIfc;
        Config config;
    };

    void updatePresence(const std::string& gpioLine, bool state);

    void waitForGPIOEvent(const std::string& name,
                          const std::function<void(bool)>& eventHandler,
                          gpiod::line& line,
                          boost::asio::posix::stream_descriptor& event);

    void addInputLine(const std::string& lineLabel);

    int readLine(const std::string& lineLabel);

    void releaseLine(const std::string& lineLabel);

    // Start to request GPIO events
    bool requestGPIOEvents(const std::string& name,
                           const std::function<void(bool)>& handler,
                           boost::asio::io_context& gpioContext);

    const std::string appName = "presence-detect";

    std::vector<gpiod::line> lines;
    std::vector<std::shared_ptr<boost::asio::posix::stream_descriptor>> linesSD;
    // dbus connection.
    std::shared_ptr<sdbusplus::asio::connection> bus;
    boost::asio::steady_timer timer;
    // Reference to dbus object interfaces.
    std::unordered_map<std::string, ObjIfaces> objIfaces;
    // Reference to gpioLines.
    std::unordered_map<std::string, ::gpiod::line> gpioLines;
};

} // namespace gpio_presence_sensing
