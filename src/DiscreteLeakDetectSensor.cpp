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

// Enable debug logging
static constexpr bool debug = false;

DiscreteLeakDetectSensor::DiscreteLeakDetectSensor(
    sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    boost::asio::io_context& io, const std::string& sensorType,
    const std::string& sensorSysfsPath, const std::string& sensorName,
    const std::string& configurationPath, float pollRate, uint8_t busId,
    uint8_t address, const std::string& driver) :
    sensorType(sensorType),
    sysfsPath(sensorSysfsPath), name(sensorName),
    sensorPollMs(static_cast<unsigned int>(pollRate * 1000)), busId(busId),
    address(address), driver(driver), objServer(objectServer), waitTimer(io),
    dbusConnection(conn), leakLevel(LeakLevel::NORMAL)
{
    sdbusplus::message::object_path inventoryObjPath(
        "/xyz/openbmc_project/inventory/leakdetectors/");
    inventoryObjPath /= name;

    // Expose inventory related leak detector interfaces and properties
    inventoryInterface = objectServer.add_interface(
        inventoryObjPath, "xyz.openbmc_project.Inventory.Item.LeakDetector");
    inventoryInterface->register_property("LeakDetectorType",
                                          std::string("Moisture"));
    if (!inventoryInterface->initialize())
    {
        std::cerr << "Error initializing leakage inventory interface for "
                  << name << "\n";
        return;
    }

    // Add association of the inventory object to the chassis.  This is required
    // for other applications such as bmcweb to determine which chassis this
    // particular Leak Detector belongs to.
    inventoryAssociation = objectServer.add_interface(inventoryObjPath,
                                                      association::interface);
    std::vector<Association> inventoryAssociations;
    inventoryAssociations.emplace_back(
        "chassis", "contained_by",
        sdbusplus::message::object_path(configurationPath).parent_path());
    inventoryAssociation->register_property("Associations",
                                            inventoryAssociations);
    if (!inventoryAssociation->initialize())
    {
        std::cerr << "Error initializing association interface for " << name
                  << "\n";
        return;
    }

    sdbusplus::message::object_path stateObjPath(
        "/xyz/openbmc_project/state/leakdetectors/");
    stateObjPath /= name;

    // Expose leak detector state interfaces and properties
    stateInterface = objectServer.add_interface(
        stateObjPath, "xyz.openbmc_project.State.LeakDetector");
    stateInterface->register_property("DetectorState",
                                      getLeakLevelStatusName(leakLevel));
    if (!stateInterface->initialize())
    {
        std::cerr << "Error initializing leakage state interface for " << name
                  << "\n";
        return;
    }

    // Add association of the state object to the invetory object that describes
    // the leak detector.  Other application such as bmcweb may use this to
    // determine which leak detector the state is describing.
    stateAssociation = objectServer.add_interface(stateObjPath,
                                                  association::interface);
    std::vector<Association> stateAssociations;
    stateAssociations.emplace_back("inventory", "leak_detecting",
                                   inventoryObjPath);
    stateAssociation->register_property("Associations", stateAssociations);
    if (!stateAssociation->initialize())
    {
        std::cerr << "Error initializing association interface for " << name
                  << "\n";
        return;
    }

    monitor();
}

DiscreteLeakDetectSensor::~DiscreteLeakDetectSensor()
{
    waitTimer.cancel();
    objServer.remove_interface(inventoryInterface);
    objServer.remove_interface(inventoryAssociation);
    objServer.remove_interface(stateInterface);
    objServer.remove_interface(stateAssociation);
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
        leakLevel = LeakLevel::NORMAL;
        stateInterface->set_property("DetectorState",
                                     getLeakLevelStatusName(leakLevel));
    }
    else
    {
        leakLevel = LeakLevel::LEAKAGE;
        stateInterface->set_property("DetectorState",
                                     getLeakLevelStatusName(leakLevel));
        createLeakageLogEntry();
    }

    return 0;
}

std::string
    DiscreteLeakDetectSensor::getLeakLevelStatusName(LeakLevel leaklevel)
{
    switch (leaklevel)
    {
        case LeakLevel::NORMAL:
            return "OK";
            break;
        case LeakLevel::LEAKAGE:
        default:
            return "Critical";
            break;
    }
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

inline void DiscreteLeakDetectSensor::createLeakageLogEntry()
{
    if constexpr (debug)
    {
        std::cout << "Logging event for sensor: " << name << "\n";
    }

    std::string messageId = "ResourceEvent.1.0.ResourceStatusChangedCritical";
    std::string resolution =
        "Inspect for water leakage and consider power down switch tray.";
    std::string severity = "xyz.openbmc_project.Logging.Entry.Level.Error";
    std::string status = getLeakLevelStatusName(leakLevel);

    std::map<std::string, std::string> addData = {};
    addData["REDFISH_MESSAGE_ID"] = messageId;
    addData["REDFISH_MESSAGE_ARGS"] = name + "," + status;
    addData["xyz.openbmc_project.Logging.Entry.Resolution"] = resolution;

    addEventLog(messageId, severity, addData);
}
