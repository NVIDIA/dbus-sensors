/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

#include "LeakDetectSensor.hpp"

#include <unistd.h>

#include <boost/asio/read_until.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <phosphor-logging/lg2.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Enable debug logging
static constexpr bool debug = false;

// Enable leak detector voltage values to be exposed on Dbus interfaces
static constexpr bool leakValueIntf = false;

// TODO: Revisit the max setting to consider IPMI implications.
static constexpr double maxVoltageReading = 2.048;
static constexpr double minVoltageReading = 0.0;

// Scales to Volts
static constexpr double sensorScaleFactor = 0.0005;

// Round value to 3 decimal places
static constexpr double roundFactor = 10000;

LeakDetectSensor::LeakDetectSensor(
                        const std::string& readPath,
                        sdbusplus::asio::object_server& objectServer,
                        std::shared_ptr<sdbusplus::asio::connection>& conn,
                        boost::asio::io_context& io,
                        const std::string& sensorName,
                        std::vector<thresholds::Threshold>&& thresholdsIn,
                        const std::shared_ptr<I2CDevice>& i2cDevice,
                        const float pollRate,
                        PowerState readState,
                        const std::string& configurationPath) :
    Sensor(escapeName(sensorName), std::move(thresholdsIn), configurationPath,
           "LeakDetect", false, false, maxVoltageReading, minVoltageReading,
           conn, readState),
    i2cDevice(i2cDevice),
    objServer(objectServer),
    inputDev(io, readPath, boost::asio::random_access_file::read_only),
    waitTimer(io),
    readPath(readPath),
    sensorPollMs(static_cast<unsigned int>(pollRate * 1000)),
    leakLevel(LeakLevel::NORMAL)
{
    if constexpr (leakValueIntf)
    {
        sensorInterface = objectServer.add_interface(
            "/xyz/openbmc_project/sensors/voltage/" + name,
            "xyz.openbmc_project.Sensor.Value");

        for (const auto& threshold : thresholds)
        {
            std::string interface = thresholds::getInterface(threshold.level);
            thresholdInterfaces[static_cast<size_t>(threshold.level)] =
                objectServer.add_interface(
                    "/xyz/openbmc_project/sensors/voltage/" + name, interface);
        }
        association = objectServer.add_interface(
            "/xyz/openbmc_project/sensors/voltage/" + name,
            association::interface);

        setInitialProperties(sensor_paths::unitVolts);
    }
}

LeakDetectSensor::~LeakDetectSensor()
{
    inputDev.close();
    waitTimer.cancel();

    for (const auto& iface : thresholdInterfaces)
    {
        objServer.remove_interface(iface);
    }
    objServer.remove_interface(sensorInterface);
    objServer.remove_interface(association);
}

// Kicks off a read operation for the underlying value of the sensor based on
// the sysfs location stored in readPath.  The read location was set up while
// initializing inputDev member variable during sensor object construction.
void LeakDetectSensor::setupRead()
{
    std::weak_ptr<LeakDetectSensor> weakRef = weak_from_this();
    inputDev.async_read_some_at(
        0, boost::asio::buffer(readBuf),
        [weakRef](const boost::system::error_code& ec, std::size_t bytesRead) {
        std::shared_ptr<LeakDetectSensor> self = weakRef.lock();
        if (self)
        {
            self->handleResponse(ec, bytesRead);
        }
    });
}

// Overrides the base class checkThresholds to include additiona logic for
// determining current leaking levels.
void LeakDetectSensor::checkThresholds()
{
    // The checkThresholds method will return false if the Critical threshold
    // is crossed.  Logic here will need to be expanded to include more leakage
    // levels in the future.
    if (!thresholds::checkThresholds(this))
    {
        // Once the sensor is in "leakage" state, it will not be able to revert
        // back to a "normal" state, as we consider this to be a critical issue
        // that should be not resolved on its own.
        setLeakLevel(LeakLevel::LEAKAGE);
    }
}

// Restarts a read operation after waiting for a fixed polling period.
void LeakDetectSensor::restartRead()
{
    std::weak_ptr<LeakDetectSensor> weakRef = weak_from_this();
    waitTimer.expires_after(std::chrono::milliseconds(sensorPollMs));
    waitTimer.async_wait([weakRef](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            std::cerr << "LeakDetectSensor read cancelled!\n";
            return; // we're being canceled
        }
        std::shared_ptr<LeakDetectSensor> self = weakRef.lock();
        if (!self)
        {
            std::cerr << "LeakDetectSensor weakRef no self!\n";
            return;
        }
        self->setupRead();
    });
}

// Handles the output of a read operation. If the read yielded no errors, will
// translate the raw value and update the value stored on this sensor object.
// updateValue will also check for threshold crossings and take appropriate
// actions.
void LeakDetectSensor::handleResponse(const boost::system::error_code& err,
                                      size_t bytesRead)
{
    if ((err == boost::system::errc::bad_file_descriptor) ||
        (err == boost::asio::error::misc_errors::not_found))
    {
        std::cerr << "LeakDetectSensor " << name << " is getting destroyed\n";
        return;
    }

    if (!err)
    {
        const char* bufEnd = readBuf.data() + bytesRead;
        std::from_chars_result ret = std::from_chars(readBuf.data(), bufEnd,
                                                     rawValue);
        if (ret.ec != std::errc())
        {
            incrementError();
        }
        else
        {
            double newValue = rawValue * sensorScaleFactor;
            newValue = std::round(newValue * roundFactor) / roundFactor;
            if constexpr (debug)
            {
                std::cout << "Updating " << name << " to " << newValue << "\n";
            }
            updateValue(newValue);
        }
    }
    else
    {
        std::cerr << "Error in response\n";
        incrementError();
    }

    restartRead();
}

// Updates the sensor object's current leak level state, and take all
// appropriate actions related to a state transition.
// TODO: Shutdown logic may be added here based on the new leakage level
void LeakDetectSensor::setLeakLevel(LeakLevel newLeakLevel)
{
    // Only log event if the leak state has transitioned
    if (leakLevel != newLeakLevel)
    {
        leakLevel = newLeakLevel;
        logEvent(leakLevel);
    }
}

// Log an event indicating a leak level state transition.  This is separate
// from the logs that may be added when thresholds of the sensor is crossed.
void LeakDetectSensor::logEvent(LeakLevel leakLevel)
{
    if constexpr (debug)
    {
        std::cout << "Logging event for sensor: " << name << "\n";
    }

    std::string messageId = "ResourceEvent.1.0.ResourceStatusChangedCritical";
    std::string resolution =
            "Power down server immediately and inspect for water leakage.";
    std::string severity = "xyz.openbmc_project.Logging.Entry.Level.Error";
    std::string status = getLeakLevelStatusName(leakLevel);

    std::map<std::string, std::string> addData = {};
    addData["REDFISH_MESSAGE_ID"] = messageId;
    addData["REDFISH_MESSAGE_ARGS"] = name + "," + status;
    addData["xyz.openbmc_project.Logging.Entry.Resolution"] = resolution;

    addEventLog(messageId, severity, addData);
}

std::string LeakDetectSensor::getLeakLevelStatusName(LeakLevel leaklevel)
{
    switch(leaklevel)
    {
        case LeakLevel::NORMAL:
            return "Normal";
        break;
        case LeakLevel::LEAKAGE:
        default:
            return "Critical";
        break;
    }
}
