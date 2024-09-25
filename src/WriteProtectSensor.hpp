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
#pragma once

#include <boost/asio.hpp>
#include <boost/container/flat_map.hpp>
#include <gpiod.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace write_protect
{
static constexpr const char* service = "xyz.openbmc_project.WriteProtectSensor";
static constexpr const char* softwareWriteProtectObjPath =
    "/xyz/openbmc_project/software/";
static constexpr const char* storefile = "/var/lib/write_protected";

namespace properties
{
static constexpr const char* propertyName = "Name";
static constexpr const char* propertyGpioLine = "GpioLine";
static constexpr const char* propertyPolarity = "Polarity";
static constexpr const char* propertyWriteProtected = "WriteProtected";
} // namespace properties

namespace interfaces
{
static constexpr const char* emWriteProtectIfc =
    "xyz.openbmc_project.Configuration.WriteProtect";
static constexpr const char* settingsIf =
    "xyz.openbmc_project.Software.Settings";
} // namespace interfaces

struct Config
{
    std::string name;
    std::string gpioLine;
    std::string parentChassisId;
    bool activeLow{};
    bool writeprotected{};
};

class WriteProtect : public std::enable_shared_from_this<WriteProtect>
{
  public:
    explicit WriteProtect(
        const std::shared_ptr<sdbusplus::asio::connection>& bus,
        const std::shared_ptr<sdbusplus::asio::object_server>& objectServer) :
        bus(bus),
        objectServerPtr(objectServer), timer(bus->get_io_context()),
        writeProtectFile(write_protect::storefile)
    {}

    ~WriteProtect()
    {
        for (const auto& obj : objIfaces)
        {
            const auto& config = obj.second.config;
            releaseLine(config.gpioLine);
            removeObj(config.name);
        }
    }

    // Add a dbus object to the reference list.
    void addObj(const std::string& name, const Config& config);

    // Remove a object from the object reference list.
    void removeObj(const std::string& name);

    // Check if a object is included in the obj->iface map already.
    bool hasObj(const std::string& name);

    // Check if a object is empty.
    bool objEmpty();

    // Setup write protect interface
    void setupWriteProtectIf(const std::string& parentChassisId);

  private:
    struct ObjIfaces
    {
        Config config;
    };

    // Read write protect pins for WriteProtected property.
    // the property is true if all pins are protected
    bool readWriteProtect();

    // Set write protect
    bool setWriteProtect(const bool& value);

    // Create write protect interface
    void createWriteProtectIf(const std::string& parentChassisId,
                              const boost::system::error_code& e);

    void addLine(const std::string& lineLabel, bool value);

    void setLine(const std::string& lineLabel, bool value);

    int readLine(const std::string& lineLabel);

    void releaseLine(const std::string& lineLabel);

    // dbus connection.
    std::shared_ptr<sdbusplus::asio::connection> bus;
    // Reference to dbus object server
    std::shared_ptr<sdbusplus::asio::object_server> objectServerPtr;
    // Reference to dbus object interfaces
    std::unordered_map<std::string, ObjIfaces> objIfaces;
    // Reference to gpio line
    std::unordered_map<std::string, ::gpiod::line> gpioLines;
    // Reference to setting intferface
    std::shared_ptr<sdbusplus::asio::dbus_interface> settingsIfPtr;
    // A delay time to cerate IF
    boost::asio::steady_timer timer;
    // WriteProtectFile exists if write protected.
    std::filesystem::path writeProtectFile;
};

} // namespace write_protect
