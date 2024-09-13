/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES.
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

#include "SynthesizedSensor.hpp"

#include "SensorPaths.hpp"
#include "Thresholds.hpp"
#include "Utils.hpp"
#include "VariantVisitors.hpp"
#include "sensor.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/container/flat_map.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

constexpr const char* synthesizedsensorType = "SummationSensor";
static constexpr bool debug = false;

constexpr const auto monitorTypes{
    std::to_array<const char*>({synthesizedsensorType})};

static std::vector<std::shared_ptr<SynthesizedSensor>> synthSensors;

static void setupSensorMatch(
    std::vector<sdbusplus::bus::match_t>& matches, sdbusplus::bus_t& connection,
    const std::string& type,
    std::function<void(const double&, sdbusplus::message_t&)>&& callback)
{
    std::function<void(sdbusplus::message_t & message)> eventHandler =
        [callback{std::move(callback)}](sdbusplus::message_t& message) {
        std::string objectName;
        boost::container::flat_map<std::string, std::variant<double, int64_t>>
            values;
        message.read(objectName, values);
        auto findValue = values.find("Value");
        if (findValue == values.end())
        {
            return;
        }
        double value = std::visit(VariantToDoubleVisitor(), findValue->second);
        if (std::isnan(value))
        {
            return;
        }

        callback(value, message);
    };
    matches.emplace_back(connection,
                         "type='signal',"
                         "member='PropertiesChanged',interface='org."
                         "freedesktop.DBus.Properties',path_"
                         "namespace='/xyz/openbmc_project/sensors/" +
                             std::string(type) +
                             "',arg0='xyz.openbmc_project.Sensor.Value'",
                         std::move(eventHandler));
}

static constexpr double totalHscMaxReading = 1500;
static constexpr double totalHscMinReading = -10;
SynthesizedSensor::SynthesizedSensor(
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::string& sensorName, const std::string& sensorConfiguration,
    sdbusplus::asio::object_server& objectServer,
    std::vector<thresholds::Threshold>&& thresholdData) :
    Sensor(escapeName(sensorName), std::move(thresholdData),
           sensorConfiguration, synthesizedsensorType, false, false,
           totalHscMaxReading, totalHscMinReading, conn),
    objServer(objectServer)
{
    sensorInterface =
        objectServer.add_interface("/xyz/openbmc_project/sensors/power/" + name,
                                   "xyz.openbmc_project.Sensor.Value");

    for (const auto& threshold : thresholds)
    {
        std::string interface = thresholds::getInterface(threshold.level);
        thresholdInterfaces[static_cast<size_t>(threshold.level)] =
            objectServer.add_interface(
                "/xyz/openbmc_project/sensors/power/" + name, interface);
    }
    association = objectServer.add_interface(
        "/xyz/openbmc_project/sensors/power/" + name, association::interface);
    setInitialProperties(sensor_paths::unitWatts);
}

SynthesizedSensor::~SynthesizedSensor()
{
    for (const auto& iface : thresholdInterfaces)
    {
        objServer.remove_interface(iface);
    }
    objServer.remove_interface(sensorInterface);
    objServer.remove_interface(association);
}

void SynthesizedSensor::setupMatches()
{
    constexpr const auto matchTypes{std::to_array<const char*>({"power"})};

    std::weak_ptr<SynthesizedSensor> weakRef = weak_from_this();
    for (const std::string type : matchTypes)
    {
        setupSensorMatch(matches, *dbusConnection, type,
                         [weakRef, type](const double& value,
                                         sdbusplus::message_t& message) {
            auto self = weakRef.lock();
            if (!self)
            {
                return;
            }
            if (type == "power")
            {
                std::string path = message.get_path();
                for (std::string& sensName : self->sensorOperands)
                {
                    if (path.ends_with(sensName))
                    {
                        self->powerReadings[message.get_path()] = value;
                    }
                }
            }
            self->updateReading();
        });
    }
    dbusConnection->async_method_call(
        [weakRef](boost::system::error_code ec, const GetSubTreeType& subtree) {
        if (ec)
        {
            std::cerr << "Error contacting mapper\n";
            return;
        }
        auto self = weakRef.lock();
        if (!self)
        {
            return;
        }
        for (const auto& [path, matches] : subtree)
        {
            size_t lastSlash = path.rfind('/');
            if (lastSlash == std::string::npos || lastSlash == path.size() ||
                matches.empty())
            {
                continue;
            }
            std::string sensorName = path.substr(lastSlash + 1);
            for (std::string& sensName : self->sensorOperands)
            {
                if (sensorName == sensName)
                {
                    // lambda capture requires a proper variable (not a
                    // structured binding)
                    const std::string& cbPath = path;
                    self->dbusConnection->async_method_call(
                        [weakRef, cbPath](boost::system::error_code ec,
                                          const std::variant<double>& value) {
                        if (ec)
                        {
                            std::cerr << "Error getting value from " << cbPath
                                      << "\n";
                        }
                        auto self = weakRef.lock();
                        if (!self)
                        {
                            return;
                        }
                        double reading = std::visit(VariantToDoubleVisitor(),
                                                    value);
                        if constexpr (debug)
                        {
                            std::cerr << cbPath << "Reading " << reading
                                      << "\n";
                        }
                        self->powerReadings[cbPath] = reading;
                    },
                        matches[0].first, cbPath, properties::interface,
                        properties::get, sensorValueInterface, "Value");
                }
            }
        }
    },
        mapper::busName, mapper::path, mapper::interface, mapper::subtree,
        "/xyz/openbmc_project/sensors/power", 0,
        std::array<const char*, 1>{sensorValueInterface});
}

void SynthesizedSensor::updateReading()
{
    double val = 0.0;
    if (calculate(val))
    {
        updateValue(val);
    }
    else
    {
        updateValue(std::numeric_limits<double>::quiet_NaN());
    }
}

bool SynthesizedSensor::calculate(double& val)
{
    constexpr size_t maxErrorPrint = 5;
    static size_t errorPrint = maxErrorPrint;

    double totalPower = 0;
    for (const auto& [path, reading] : powerReadings)
    {
        if (std::isnan(reading))
        {
            continue;
        }
        totalPower += reading;
    }

    if (totalPower == 0)
    {
        if (errorPrint > 0)
        {
            errorPrint--;
            std::cerr << "total power 0\n";
        }
        val = 0;
        return false;
    }

    val = totalPower;
    return true;
}

void SynthesizedSensor::checkThresholds()
{
    thresholds::checkThresholds(this);
}

void createSensor(sdbusplus::asio::object_server& objectServer,
                  std::shared_ptr<SynthesizedSensor>& summationSensor,
                  std::shared_ptr<sdbusplus::asio::connection>& dbusConnection)
{
    if (!dbusConnection)
    {
        std::cerr << "Connection not created\n";
        return;
    }
    auto getter = std::make_shared<GetSensorConfiguration>(
        dbusConnection, [&objectServer, &dbusConnection,
                         &summationSensor](const ManagedObjectType& resp) {
        synthSensors.clear();

        for (const auto& [path, interfaces] : resp)
        {
            summationSensor = nullptr;

            for (const auto& [intf, cfg] : interfaces)
            {
                // Get Summation sensor related info
                if (intf == configInterfaceName(synthesizedsensorType))
                {
                    // parseThresholdsFromConfig traverses all
                    // interfaces for this path, looking
                    // for thresholds.
                    std::vector<thresholds::Threshold> sensorThresholds;
                    parseThresholdsFromConfig(interfaces, sensorThresholds);

                    std::string name = loadVariant<std::string>(cfg, "Name");
                    summationSensor = std::make_shared<SynthesizedSensor>(
                        dbusConnection, name, path.str, objectServer,
                        std::move(sensorThresholds));
                    summationSensor->sensorOperands.clear();

                    summationSensor->sensorOperands =
                        loadVariant<std::vector<std::string>>(cfg,
                                                              "SensorsToSum");
                }
            }
            if (summationSensor)
            {
                synthSensors.push_back(summationSensor);

                summationSensor->setupMatches();
                summationSensor->updateReading();
            }
        }
    });
    getter->getConfiguration(
        std::vector<std::string>(monitorTypes.begin(), monitorTypes.end()));
}

int main()
{
    boost::asio::io_context io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::asio::object_server objectServer(systemBus, true);
    objectServer.add_manager("/xyz/openbmc_project/sensors");
    systemBus->request_name("xyz.openbmc_project.SynthesizedSensor");
    std::shared_ptr<SynthesizedSensor> sensor =
        nullptr; // wait until we find the config
    boost::asio::post(io,
                      [&]() { createSensor(objectServer, sensor, systemBus); });

    boost::asio::steady_timer configTimer(io);

    std::function<void(sdbusplus::message_t&)> eventHandler =
        [&](sdbusplus::message_t&) {
        configTimer.expires_after(std::chrono::seconds(1));
        // create a timer because normally multiple properties change
        configTimer.async_wait([&](const boost::system::error_code& ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                return; // we're being canceled
            }
            createSensor(objectServer, sensor, systemBus);
            if (!sensor)
            {
                std::cout << "Configuration not detected\n";
            }
        });
    };
    std::vector<std::unique_ptr<sdbusplus::bus::match_t>> matches =
        setupPropertiesChangedMatches(*systemBus, monitorTypes, eventHandler);

    setupManufacturingModeMatch(*systemBus);
    io.run();
    return 0;
}
