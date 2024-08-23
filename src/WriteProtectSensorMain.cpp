/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
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

#include "Utils.hpp"
#include "WriteProtectSensor.hpp"

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace write_protect
{
std::unique_ptr<sdbusplus::bus::match::match> ifcAdded;
std::unique_ptr<sdbusplus::bus::match::match> ifcRemoved;
using OnInterfaceAddedCallback = std::function<void(const Config&)>;
using OnInterfaceRemovedCallback = std::function<void(std::string_view)>;

Config getConfig(const SensorBaseConfigMap& properties,
                 const std::string& chassisId)
{
    auto name = loadVariant<std::string>(properties, properties::propertyName);
    auto gpioLine = loadVariant<std::string>(properties,
                                             properties::propertyGpioLine);
    auto gpioPolarity = loadVariant<std::string>(properties,
                                                 properties::propertyPolarity);
    bool activeLow = false;
    if (gpioPolarity == "active_low")
    {
        activeLow = true;
    }

    return {name, gpioLine, chassisId, activeLow, false};
}

template <typename Callback>
void getEmWriteProtectIf(const ManagedObjectType& managedObjs,
                         Callback&& callback)
{
    for (const auto& obj : managedObjs)
    {
        const auto& item = obj.second;
        auto found = item.find(interfaces::emWriteProtectIfc);
        if (found != item.end())
        {
            Config config;
            std::filesystem::path emObjPath = obj.first.str;
            try
            {
                config = getConfig(found->second,
                                   emObjPath.parent_path().filename());
                std::forward<Callback>(callback)(config);
            }
            catch (std::exception& e)
            {
                std::cerr << "Incomplete config found: " << e.what()
                          << " obj = " << obj.first.str << std::endl;
            }
        }
    }
}

template <typename Callback>
void catchSignal(sdbusplus::message::message& msg, Callback&& callback)
{
    sdbusplus::message::object_path objPath;
    SensorData ifcAndProperties;
    msg.read(objPath, ifcAndProperties);
    auto found = ifcAndProperties.find(interfaces::emWriteProtectIfc);
    if (found != ifcAndProperties.end())
    {
        Config config;
        std::filesystem::path emObjPath = objPath.str;
        try
        {
            config = getConfig(found->second,
                               emObjPath.parent_path().filename());
            std::forward<Callback>(callback)(config);
        }
        catch (std::exception& e)
        {
            std::cerr << "Incomplete config found: " << e.what()
                      << " obj = " << objPath.str << std::endl;
        }
    }
}

void setupInterfaceAdded(sdbusplus::asio::connection* conn,
                         OnInterfaceAddedCallback&& callbackIn)
{
    if (conn == nullptr)
    {
        throw std::runtime_error("Undefined dbus connection");
    }

    std::function<void(sdbusplus::message::message & msg)> handler =
        [callbackIn](sdbusplus::message::message& msg) {
        catchSignal(msg, callbackIn);
    };

    conn->async_method_call(
        [callback{std::move(callbackIn)}](
            const boost::system::error_code& ec,
            const ManagedObjectType& managedObjs) {
        if (ec)
        {
            std::cerr
                << "Failed to retrieve Entity Manager WriteProtect Interface: "
                << ec << std::endl;
            return;
        }
        getEmWriteProtectIf(managedObjs, callback);
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

void setupInterfaceRemoved(sdbusplus::asio::connection* conn,
                           OnInterfaceRemovedCallback&& callbackIn)
{
    if (conn == nullptr)
    {
        throw std::runtime_error("Undefined dbus connection");
    }

    std::function<void(sdbusplus::message::message & msg)> handler =
        [callback{std::move(callbackIn)}](sdbusplus::message::message msg) {
        sdbusplus::message::object_path objPath;
        msg.read(objPath);
        callback(objPath.filename());
    };

    ifcRemoved = std::make_unique<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*conn),
        sdbusplus::bus::match::rules::interfacesRemoved() +
            sdbusplus::bus::match::rules::sender(
                "xyz.openbmc_project.EntityManager"),
        handler);
}

void addSoftwareObject(
    const std::shared_ptr<write_protect::WriteProtect>& writeprotector,
    const write_protect::Config& config)
{
    if (writeprotector->objEmpty())
    {
        writeprotector->setupWriteProtectIf(config.parentChassisId);
    }
    if (writeprotector->hasObj(config.name))
    {
        writeprotector->removeObj(config.name);
    }
    writeprotector->addObj(config.name, config);
}
} // namespace write_protect

int main()
{
    boost::asio::io_context io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    systemBus->request_name(write_protect::service);
    auto objectServer =
        std::make_shared<sdbusplus::asio::object_server>(systemBus);

    std::shared_ptr<write_protect::WriteProtect> writeprotector =
        std::make_shared<write_protect::WriteProtect>(systemBus, objectServer);

    write_protect::setupInterfaceAdded(
        systemBus.get(),
        [&writeprotector](const write_protect::Config& config) {
        write_protect::addSoftwareObject(writeprotector, config);
    });

    write_protect::setupInterfaceRemoved(
        systemBus.get(), [&writeprotector](std::string_view name) {
        writeprotector->removeObj(std::string(name));
    });

    io.run();
}
