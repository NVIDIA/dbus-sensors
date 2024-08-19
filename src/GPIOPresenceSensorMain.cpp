/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
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

#include "GPIOPresenceSensor.hpp"
#include "Utils.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gpio_presence_sensing
{
std::unique_ptr<sdbusplus::bus::match::match> ifcAdded;
std::unique_ptr<sdbusplus::bus::match::match> ifcRemoved;

using OnInterfaceAddedCallback =
    std::function<void(std::string_view, std::string_view, const Config&)>;
using OnInterfaceRemovedCallback = std::function<void(std::string_view)>;

// Helper function to convert dbus property to struct
// @param[in] properties: dbus properties
Config getConfig(const SensorBaseConfigMap& properties)
{
    auto name = loadVariant<std::string>(properties, properties::propertyName);
    auto gpioLine = loadVariant<std::string>(properties,
                                             properties::propertyGpioLine);
    return {name, gpioLine, /*present*/ false};
}

// NOLINTBEGIN(cppcoreguidelines-rvalue-reference-param-not-moved)
void setupInterfaceAdded(sdbusplus::asio::connection* conn,
                         OnInterfaceAddedCallback&& cb)
{
    std::function<void(sdbusplus::message::message & msg)> handler =
        [callback = cb](sdbusplus::message::message& msg) {
        sdbusplus::message::object_path objPath;
        SensorData ifcAndProperties;
        msg.read(objPath, ifcAndProperties);
        auto found = ifcAndProperties.find(interfaces::emGPIOCableSensingIfc);
        if (found != ifcAndProperties.end())
        {
            Config config;
            try
            {
                config = getConfig(found->second);
                callback(objPath.str, found->first, config);
            }
            catch (std::exception& e)
            {
                std::cerr << "Incomplete config found: " << e.what()
                          << " obj = " << objPath.str << std::endl;
            }
        }
    };

    // call the user callback for all the device that is already available
    conn->async_method_call(
        [cb](const boost::system::error_code ec,
             const ManagedObjectType& managedObjs) {
        if (ec)
        {
            return;
        }
        for (const auto& obj : managedObjs)
        {
            const auto& item = obj.second;
            auto found = item.find(interfaces::emGPIOCableSensingIfc);
            if (found != item.end())
            {
                Config config;
                try
                {
                    config = getConfig(found->second);
                    cb(obj.first.str, found->first, config);
                }
                catch (std::exception& e)
                {
                    std::cerr << "Incomplete config found: " << e.what()
                              << " obj = " << obj.first.str << std::endl;
                }
            }
        }
    },
        "xyz.openbmc_project.EntityManager", "/xyz/openbmc_project/inventory",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");

    ifcAdded = std::make_unique<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*conn),
        sdbusplus::bus::match::rules::interfacesAdded() +
            sdbusplus::bus::match::rules::sender(
                "xyz.openbmc_project.EntityManager"),
        handler);
}
// NOLINTEND(cppcoreguidelines-rvalue-reference-param-not-moved)

// NOLINTBEGIN(cppcoreguidelines-rvalue-reference-param-not-moved)
void setupInterfaceRemoved(sdbusplus::asio::connection* conn,
                           OnInterfaceRemovedCallback&& cb)
{
    if (conn == nullptr)
    {
        throw std::runtime_error("Undefined dbus connection");
    }
    // Listen to the interface removed event.
    std::function<void(sdbusplus::message::message & msg)> handler =
        [callback = cb](sdbusplus::message::message msg) {
        sdbusplus::message::object_path objPath;
        msg.read(objPath);
        callback(objPath.str);
    };
    ifcRemoved = std::make_unique<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*conn),
        sdbusplus::bus::match::rules::interfacesRemoved() +
            sdbusplus::bus::match::rules::sender(
                "xyz.openbmc_project.EntityManager"),
        handler);
}
// NOLINTEND(cppcoreguidelines-rvalue-reference-param-not-moved)

void addInventoryObject(
    const std::shared_ptr<gpio_presence_sensing::GPIOPresence>& controller,
    sdbusplus::asio::object_server& objectServer,
    const gpio_presence_sensing::Config& config)
{
    sdbusplus::message::object_path inventoryPath(
        gpio_presence_sensing::inventoryObjPath);
    sdbusplus::message::object_path inventoryCableObjPath(
        gpio_presence_sensing::inventoryCableObjPath);
    sdbusplus::message::object_path objPath = inventoryPath / config.name;
    sdbusplus::message::object_path objCablePath = inventoryCableObjPath /
                                                   config.name;
    std::cout << "New config received " << objPath.str << std::endl;
    if (controller->hasObj(objPath.str))
    {
        controller->removeObj(objPath.str);
    }
    // Status
    auto statusIfc = objectServer.add_unique_interface(
        objCablePath, gpio_presence_sensing::interfaces::statusIfc);
    auto cableIfc = objectServer.add_interface(objCablePath,
                                               interfaces::statusCableIfc);
    statusIfc->register_property(
        gpio_presence_sensing::properties::propertyPresent, false);
    statusIfc->initialize();
    cableIfc->initialize();
    controller->addObj(statusIfc, objPath.str, config);
}
} // namespace gpio_presence_sensing

void startMain(
    int delay, const std::shared_ptr<sdbusplus::asio::connection>& bus,
    const std::shared_ptr<gpio_presence_sensing::GPIOPresence>& controller,
    boost::asio::io_context& gpioContext)
{
    static boost::asio::steady_timer timer(bus->get_io_context());
    controller->readPresent();
    timer.cancel();
    timer.expires_after(std::chrono::seconds(delay));
    timer.async_wait(
        [bus, controller, &gpioContext](const boost::system::error_code ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            std::cout << "Delaying update loop" << std::endl;
            return;
        }
        controller->startGPIOEventMonitor(gpioContext);
        std::cout << "Update loop started" << std::endl;
    });
}

int main()
{
    boost::asio::io_context io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    systemBus->request_name(gpio_presence_sensing::service);
    sdbusplus::asio::object_server objectServer(systemBus);

    std::shared_ptr<gpio_presence_sensing::GPIOPresence> controller =
        std::make_shared<gpio_presence_sensing::GPIOPresence>(systemBus);
    gpio_presence_sensing::setupInterfaceAdded(
        systemBus.get(), [&controller, &systemBus, &objectServer,
                          &io](std::string_view, std::string_view,
                               const gpio_presence_sensing::Config& config) {
        gpio_presence_sensing::addInventoryObject(controller, objectServer,
                                                  config);
        startMain(/*delay=*/10, systemBus, controller, io);
    });

    gpio_presence_sensing::setupInterfaceRemoved(
        systemBus.get(), [&controller](std::string_view objPath) {
        controller->removeObj(std::string(objPath));
    });

    io.run();
}
