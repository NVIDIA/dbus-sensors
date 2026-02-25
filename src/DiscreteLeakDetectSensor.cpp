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

#include "Utils.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/message/native_types.hpp>
#include <xyz/openbmc_project/Inventory/Item/LeakDetector/common.hpp>
#include <xyz/openbmc_project/Logging/Entry/common.hpp>
#include <xyz/openbmc_project/State/Chassis/common.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/common.hpp>
#include <xyz/openbmc_project/State/LeakDetector/common.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace inventoryItem =
    sdbusplus::common::xyz::openbmc_project::inventory::item;
namespace logging = sdbusplus::common::xyz::openbmc_project::logging;
namespace state = sdbusplus::common::xyz::openbmc_project::state;

/* CPLD definitions
1 - no event (leakage not detected)
0 - leakage event (leakage detected)
*/

// Enable debug logging
static constexpr bool debug = false;
unsigned int DiscreteLeakDetectSensor::lastUID = 0;

DiscreteLeakDetectSensor::DiscreteLeakDetectSensor(
    sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    boost::asio::io_context& io, const std::string& sensorType,
    const std::string& sensorSysfsPath, const std::string& sensorName,
    const std::string& configurationPath, float pollRate, uint8_t busId,
    uint8_t address, const std::string& driver, bool shutdownOnLeak,
    const unsigned int shutdownDelaySeconds) :
    sensorType(sensorType), sysfsPath(sensorSysfsPath), name(sensorName),
    sensorPollMs(static_cast<unsigned int>(pollRate * 1000)), busId(busId),
    address(address), driver(driver), objServer(objectServer), waitTimer(io),
    shutdownTimer(io), dbusConnection(conn), shutdownOnLeak(shutdownOnLeak),
    shutdownDelaySeconds(shutdownDelaySeconds),
    didShutdownOnThisOccurrence(false), startedShutdownTimer(false)
{
    DiscreteLeakDetectSensor::lastUID++;
    uid = DiscreteLeakDetectSensor::lastUID;
    sdbusplus::message::object_path inventoryObjPath(
        "/xyz/openbmc_project/inventory/leakdetectors/");
    inventoryObjPath /= name;

    // Expose inventory related leak detector interfaces and properties
    inventoryInterface = objectServer.add_interface(
        inventoryObjPath, inventoryItem::LeakDetector::interface);
    inventoryInterface->register_property(
        "LeakDetectorType",
        inventoryItem::convertForMessage(
            inventoryItem::LeakDetector::LeakDetectorTypeEnum::Moisture));
    if (!inventoryInterface->initialize())
    {
        std::cerr << "Error initializing leakage inventory interface for "
                  << name << "\n";
        return;
    }

    // Add association of the inventory object to the chassis.  This is required
    // for other applications such as bmcweb to determine which chassis this
    // particular Leak Detector belongs to.
    inventoryAssociation =
        objectServer.add_interface(inventoryObjPath, association::interface);
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
    stateInterface = objectServer.add_interface(stateObjPath,
                                                state::LeakDetector::interface);
    stateInterface->register_property("DetectorState",
                                      getLeakLevelStatusName(leakLevel));
    if (!stateInterface->initialize())
    {
        std::cerr << "Error initializing leakage state interface for " << name
                  << "\n";
        return;
    }

    // Expose detector operational state interface and properties
    opStateInterface = objectServer.add_interface(
        stateObjPath, state::decorator::OperationalStatus::interface);
    opStateInterface->register_property("State",
                                        getLeakLevelStateString(leakLevel));
    if (!opStateInterface->initialize())
    {
        std::cerr << "Error initializing operational state interface for "
                  << name << "\n";
        return;
    }

    // Add association of the state object to the invetory object that describes
    // the leak detector.  Other application such as bmcweb may use this to
    // determine which leak detector the state is describing.
    stateAssociation =
        objectServer.add_interface(stateObjPath, association::interface);
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

    // This is asynchronous, ensuring the io_context is available.
    boost::asio::post(waitTimer.get_executor(), [this]() { monitor(); });

    std::cout << "Created DiscreteLeakDetectSensor for " << name << " with uid "
              << uid << "\n";
}

DiscreteLeakDetectSensor::~DiscreteLeakDetectSensor()
{
    waitTimer.cancel();
    shutdownTimer.cancel();
    objServer.remove_interface(inventoryInterface);
    objServer.remove_interface(inventoryAssociation);
    objServer.remove_interface(stateInterface);
    objServer.remove_interface(opStateInterface);
    objServer.remove_interface(stateAssociation);
    std::cout << "Destroyed DiscreteLeakDetectSensor for " << name
              << " with uid " << uid << "\n";
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

bool DiscreteLeakDetectSensor::isAggregatedLeak()
{
    return (name == "leakage_aggr");
}

int DiscreteLeakDetectSensor::getLeakInfo()
{
    std::vector<std::pair<std::string, int>> leakVec;
    auto leakVal = readLeakValue(sysfsPath + "/" + name);
    LeakLevel oldLeakLevel = leakLevel;

    if (leakVal == 1)
    {
        leakLevel = LeakLevel::NORMAL;
        stateInterface->set_property("DetectorState",
                                     getLeakLevelStatusName(leakLevel));
        opStateInterface->set_property("State",
                                       getLeakLevelStateString(leakLevel));
        didShutdownOnThisOccurrence = false;
    }
    else
    {
        leakLevel = LeakLevel::LEAKAGE;
        stateInterface->set_property("DetectorState",
                                     getLeakLevelStatusName(leakLevel));
        opStateInterface->set_property("State",
                                       getLeakLevelStateString(leakLevel));
        if (shutdownOnLeak && !didShutdownOnThisOccurrence &&
            isAggregatedLeak())
        {
            didShutdownOnThisOccurrence = true;
            startShutdown();
        }
    }

    if (oldLeakLevel != leakLevel)
    {
        std::cout << "Leak value changed from "
                  << getLeakLevelStatusName(oldLeakLevel) << " to "
                  << getLeakLevelStatusName(leakLevel) << "\n";
        createLeakageLogEntry();
    }

    return 0;
}

std::string DiscreteLeakDetectSensor::getLeakResourceStatusName(
    LeakLevel leaklevel)
{
    switch (leaklevel)
    {
        case LeakLevel::NORMAL:
            return "ResourceEvent.1.0.ResourceStatusChangedOK";
        case LeakLevel::LEAKAGE:
        default:
            return "ResourceEvent.1.0.ResourceStatusChangedCritical";
    }
}

std::string DiscreteLeakDetectSensor::getLeakResourceResolutionName(
    LeakLevel leaklevel)
{
    std::string resolution = "";
    switch (leaklevel)
    {
        case LeakLevel::NORMAL:
            resolution = "None.";
            break;
        case LeakLevel::LEAKAGE:
        default:
            resolution =
                "Inspect for water leakage and consider power down switch tray.";
            if (startedShutdownTimer &&
                (shutdownTimer.expiry() > std::chrono::steady_clock::now()))
            {
                auto callbackRemainingTime =
                    shutdownTimer.expiry() - std::chrono::steady_clock::now();
                auto remainingSeconds =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        callbackRemainingTime)
                        .count();
                resolution += " System will shutdown in " +
                              std::to_string(remainingSeconds) + " seconds.";
            }
            break;
    }
    return resolution;
}

std::string DiscreteLeakDetectSensor::getLeakResourceSeverityName(
    LeakLevel leaklevel)
{
    switch (leaklevel)
    {
        case LeakLevel::NORMAL:
            return logging::convertForMessage(
                logging::Entry::Level::Informational);
        case LeakLevel::LEAKAGE:
        default:
            return logging::convertForMessage(logging::Entry::Level::Error);
    }
}

std::string DiscreteLeakDetectSensor::getLeakLevelStatusName(
    LeakLevel leaklevel)
{
    switch (leaklevel)
    {
        case LeakLevel::NORMAL:
            return state::convertForMessage(
                state::LeakDetector::DetectorStateEnum::OK);
        case LeakLevel::LEAKAGE:
        default:
            return state::convertForMessage(
                state::LeakDetector::DetectorStateEnum::Critical);
    }
}

std::string DiscreteLeakDetectSensor::getLeakLevelStateString(
    LeakLevel leaklevel)
{
    switch (leaklevel)
    {
        case LeakLevel::NORMAL:
        case LeakLevel::LEAKAGE:
            return state::decorator::convertForMessage(
                state::decorator::OperationalStatus::StateType::Enabled);
    }
    /* For now returning always "Enabled", since we didn't implement,
    the FAULT state yet */
    return state::decorator::convertForMessage(
        state::decorator::OperationalStatus::StateType::Enabled);
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

    std::string messageId = getLeakResourceStatusName(leakLevel);
    std::string resolution = getLeakResourceResolutionName(leakLevel);

    std::string severity = getLeakResourceSeverityName(leakLevel);
    std::string status = getLeakLevelStatusName(leakLevel);

    std::map<std::string, std::string> addData = {};
    addData["REDFISH_MESSAGE_ID"] = messageId;
    addData["REDFISH_MESSAGE_ARGS"] = name + "," + status;
    addData["xyz.openbmc_project.Logging.Entry.Resolution"] = resolution;

    addEventLog(dbusConnection, messageId, severity, addData);
}

void DiscreteLeakDetectSensor::startShutdown()
{
    if (shutdownDelaySeconds != 0U)
    {
        if (startedShutdownTimer)
        {
            /*Timer is already pending. no need to resched */
            return;
        }

        std::cout << "Setting timer for " << shutdownDelaySeconds
                  << " second(s) delay before shutdown due to " << name
                  << ".\n";

        startedShutdownTimer = true;
        shutdownTimer.expires_after(std::chrono::seconds(shutdownDelaySeconds));
        shutdownTimer.async_wait([&](const boost::system::error_code& ec) {
            startedShutdownTimer = false;
            if (ec == boost::asio::error::operation_aborted)
            {
                std::cout << "Timer aborted before expiration \n";
                return; // we're being canceled
            }

            if (ec)
            {
                std::cerr << "Shutdown Timer callback error: " << ec.message()
                          << "\n";
                return;
            }

            executeShutdown();
        });
    }
    else
    {
        startedShutdownTimer = false;
        executeShutdown();
    }
}

void DiscreteLeakDetectSensor::executeShutdown()
{
    std::cout << "Executing shutdown requested by " << name << ".\n";
    std::variant<std::string> transitionChassisOff =
        state::convertForMessage(state::Chassis::Transition::Off);

    dbusConnection->async_method_call(
        [](const boost::system::error_code& ec) {
            if (ec)
            {
                std::cerr << "Failed to execute shutdown due to "
                          << ec.message() << "\n";
                return;
            }
        },
        state::Chassis::interface, "/xyz/openbmc_project/state/chassis0",
        "org.freedesktop.DBus.Properties", "Set", state::Chassis::interface,
        "RequestedPowerTransition", transitionChassisOff);
    std::cout << "executeShutdown done\n";
}
