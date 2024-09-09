/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
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

#include "DiscreteLeakDetectSensor.hpp"

#include <unistd.h>

#include <boost/asio/read_until.hpp>

#include <cerrno>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

/* CPLD definitions
1 - no event (leakage not detected)
0 - leakage event (leakage detected)
*/

const std::string messageError{
    "The resource property Leakage Sensor has detected errors of type 'Leakage'."};
const std::string resolution{
    "Inspect for water leakage and consider power down switch tray."};
const std::string resourceErrorDetected{
    "ResourceEvent.1.0.ResourceErrorsDetected"};

DiscreteLeakDetectSensor::DiscreteLeakDetectSensor(
    sdbusplus::bus::bus& bus, sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    boost::asio::io_context& io, const std::string& sensorType,
    const std::string& sensorSysfsPath, const std::string& sensorName,
    float pollRate, uint8_t busId, uint8_t address, const std::string& driver) :
    sensorType(sensorType),
    sysfsPath(sensorSysfsPath), name(sensorName),
    sensorPollMs(static_cast<unsigned int>(pollRate * 1000)), busId(busId),
    address(address), driver(driver), objServer(objectServer), waitTimer(io),
    dbusConnection(conn)
{
    auto path = "/xyz/openbmc_project/sensors/leakage/" +
                escapeName(sensorName);

    try
    {
        leakDetectStateIntf =
            std::make_unique<LeakDetectStateIntf>(bus, path.c_str());
        leakDetectItemIntf = std::make_unique<LeakDetectItemIntf>(bus,
                                                                  path.c_str());
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << std::endl;
    }

    if (sensorType == "FloatSwitch")
    {
        leakDetectItemIntf->leakDetectorType(
            sdbusplus::xyz::openbmc_project::Inventory::Item::server::
                LeakDetector::LeakDetectorTypeEnum::FloatSwitch);
    }
    else
    {
        leakDetectItemIntf->leakDetectorType(
            sdbusplus::xyz::openbmc_project::Inventory::Item::server::
                LeakDetector::LeakDetectorTypeEnum::Moisture);
    }

    monitor();
}

DiscreteLeakDetectSensor::~DiscreteLeakDetectSensor()
{
    waitTimer.cancel();
    objServer.remove_interface(sensorInterface);
}

int DiscreteLeakDetectSensor::readLeakValue(const std::string& filePath)
{
    std::ifstream file(filePath);
    int value = 1;
    if (file.is_open())
    {
        file >> value;
    }
    return value;
}

int DiscreteLeakDetectSensor::getLeakInfo()
{
    std::vector<std::pair<std::string, int>> leakVec;
    auto leakVal = readLeakValue(sysfsPath + "/" + name);

    if (leakVal == 1)
    {
        leakDetectStateIntf->detectorState(
            sdbusplus::xyz::openbmc_project::State::server::LeakDetector::
                DetectorStateEnum::OK);
    }
    else
    {
        leakDetectStateIntf->detectorState(
            sdbusplus::xyz::openbmc_project::State::server::LeakDetector::
                DetectorStateEnum::Critical);

        createLeakageLogEntry(resourceErrorDetected, name, "Leakage Detected",
                              resolution);
    }

    return 0;
}

void DiscreteLeakDetectSensor::monitor()
{
    waitTimer.expires_after(std::chrono::milliseconds(sensorPollMs));
    waitTimer.async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            std::cerr << "Read operation aborted\n";
            return; // we're being cancelled
        }
        // read timer error
        if (ec)
        {
            std::cerr << "timer error\n";
            return;
        }

        int ret = getLeakInfo();
        if (ret < 0)
        {
            std::cerr << "DiscreteLeakDetectSensor::getLeakInfo error";
            std::cerr << "\n";
        }

        // Start read for next leakage status
        monitor();
    });
}

inline void DiscreteLeakDetectSensor::createLeakageLogEntry(
    const std::string& messageID, const std::string& arg0,
    const std::string& arg1, const std::string& resolution,
    const std::string& logNamespace)
{
    using namespace sdbusplus::xyz::openbmc_project::Logging::server;
    using Level =
        sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

    std::map<std::string, std::string> addData;
    addData["REDFISH_MESSAGE_ID"] = messageID;
    Level level = Level::Critical;

    if (messageID == resourceErrorDetected)
    {
        addData["REDFISH_MESSAGE_ARGS"] = (arg0 + "," + arg1);
        level = Level::Critical;
    }
    else
    {
        lg2::error("Message Registry messageID is not recognised", "MESSAGEID",
                   messageID);
        return;
    }

    if (!resolution.empty())
    {
        addData["xyz.openbmc_project.Logging.Entry.Resolution"] = resolution;
    }

    if (!logNamespace.empty())
    {
        addData["namespace"] = logNamespace;
    }

    auto severity =
        sdbusplus::xyz::openbmc_project::Logging::server::convertForMessage(
            level);
    dbusConnection->async_method_call(
        [](boost::system::error_code ec) {
        if (ec)
        {
            lg2::error("error while logging message registry: ",
                       "ERROR_MESSAGE", ec.message());
            return;
        }
    },
        "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
        "xyz.openbmc_project.Logging.Create", "Create", messageID, severity,
        addData);
}
