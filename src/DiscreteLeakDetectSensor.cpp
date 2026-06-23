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
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message/native_types.hpp>
#include <xyz/openbmc_project/Inventory/Item/LeakDetector/common.hpp>
#include <xyz/openbmc_project/Logging/Entry/common.hpp>
#include <xyz/openbmc_project/State/Chassis/common.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/common.hpp>
#include <xyz/openbmc_project/State/LeakDetector/common.hpp>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
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

static constexpr auto leakDetectionPolicyInterface =
    "xyz.openbmc_project.Configuration.LeakDetectionPolicy";

namespace
{
std::string describeLeakShutdownAction(const std::string& criticalType,
                                       double reactionDelaySeconds)
{
    if (criticalType == "None")
    {
        return std::string("no_chassis_shutdown(delay_s=") +
               std::to_string(static_cast<int>(reactionDelaySeconds)) + ")";
    }
    const char* transition = (criticalType == "GracefulShutdown")
                                 ? "graceful_shutdown"
                                 : "force_off";
    return std::string(transition) + "(delay_s=" +
           std::to_string(static_cast<int>(reactionDelaySeconds)) + ")";
}
} // namespace

DiscreteLeakDetectSensor::DiscreteLeakDetectSensor(
    sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    boost::asio::io_context& io, const std::string& sensorType,
    const std::string& sensorSysfsPath, const std::string& sensorName,
    const std::string& configurationPath, float pollRate, uint8_t busId,
    uint8_t address, const std::string& driver) :
    sensorType(sensorType), sysfsPath(sensorSysfsPath), name(sensorName),
    sensorPollMs(static_cast<unsigned int>(pollRate * 1000)), busId(busId),
    address(address), driver(driver), objServer(objectServer), waitTimer(io),
    shutdownTimer(io), dbusConnection(conn), startedShutdownTimer(false)
{
    DiscreteLeakDetectSensor::lastUID++;
    uid = DiscreteLeakDetectSensor::lastUID;
    sdbusplus::object_path inventoryObjPath(
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
        sdbusplus::object_path(configurationPath).parent_path());
    inventoryAssociation->register_property("Associations",
                                            inventoryAssociations);
    if (!inventoryAssociation->initialize())
    {
        std::cerr << "Error initializing association interface for " << name
                  << "\n";
        return;
    }

    sdbusplus::object_path stateObjPath(
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
    leakPolicyMatch_.reset();
    waitTimer.cancel();
    shutdownTimer.cancel();
    ++shutdownTimerGeneration_;
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

int DiscreteLeakDetectSensor::getLeakInfo()
{
    auto leakVal = readLeakValue(sysfsPath + "/" + name);
    LeakLevel oldLeakLevel = leakLevel;

    if (leakVal == 1)
    {
        leakLevel = LeakLevel::NORMAL;
        stateInterface->set_property("DetectorState",
                                     getLeakLevelStatusName(leakLevel));
        opStateInterface->set_property("State",
                                       getLeakLevelStateString(leakLevel));
    }
    else
    {
        leakLevel = LeakLevel::LEAKAGE;
        stateInterface->set_property("DetectorState",
                                     getLeakLevelStatusName(leakLevel));
        opStateInterface->set_property("State",
                                       getLeakLevelStateString(leakLevel));
    }

    if (oldLeakLevel != leakLevel)
    {
        std::cout << "DiscreteLeakDetectSensor " << name
                  << ": Leak value changed from "
                  << getLeakLevelStatusName(oldLeakLevel) << " to "
                  << getLeakLevelStatusName(leakLevel) << "\n";
        if (leakLevel == LeakLevel::NORMAL)
        {
            cancelPendingShutdown();
        }
        else if (leakLevel == LeakLevel::LEAKAGE &&
                 criticalReactionTriggersShutdown())
        {
            startShutdown();
        }
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
    waitTimer.async_wait(
        [this, sensorName = name](const boost::system::error_code& ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                std::cerr << "DiscreteLeakDetectSensor " << sensorName
                          << ": Read operation aborted\n";
                return; // we're being cancelled
            }
            // read timer error
            if (ec)
            {
                std::cerr << "DiscreteLeakDetectSensor " << sensorName
                          << ": timer error\n";
                return;
            }

            int ret = getLeakInfo();
            if (ret < 0)
            {
                std::cerr << "DiscreteLeakDetectSensor " << sensorName
                          << ": getLeakInfo error\n";
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

void DiscreteLeakDetectSensor::cancelPendingShutdown()
{
    shutdownTimer.cancel();
    ++shutdownTimerGeneration_;
    startedShutdownTimer = false;
}

void DiscreteLeakDetectSensor::startShutdown()
{
    if (!criticalReactionTriggersShutdown())
    {
        return;
    }

    shutdownTimer.cancel();
    ++shutdownTimerGeneration_;
    const uint64_t gen = shutdownTimerGeneration_;

    const unsigned int delaySec = effectiveShutdownDelaySeconds();
    if (delaySec != 0U)
    {
        std::cout << "Setting timer for " << delaySec
                  << " second(s) delay before shutdown due to " << name
                  << ".\n";

        startedShutdownTimer = true;
        shutdownTimer.expires_after(std::chrono::seconds(delaySec));
        shutdownTimer.async_wait([this, gen, sensorName = name](
                                     const boost::system::error_code& ec) {
            if (gen != shutdownTimerGeneration_)
            {
                return;
            }
            startedShutdownTimer = false;
            if (ec == boost::asio::error::operation_aborted)
            {
                std::cout << "DiscreteLeakDetectSensor " << sensorName
                          << ": Timer aborted before expiration\n";
                return; // we're being canceled
            }

            if (ec)
            {
                std::cerr << "DiscreteLeakDetectSensor " << sensorName
                          << ": Shutdown Timer callback error: " << ec.message()
                          << "\n";
                return;
            }

            executeShutdown();
        });
    }
    else
    {
        startedShutdownTimer = false;
        if (gen == shutdownTimerGeneration_)
        {
            executeShutdown();
        }
    }
}

void DiscreteLeakDetectSensor::executeShutdown()
{
    if (!criticalReactionTriggersShutdown())
    {
        std::cout << "Shutdown suppressed for " << name
                  << " (policy does not request power off).\n";
        return;
    }

    std::cout << "Executing shutdown requested by " << name << ".\n";

    // Same as bmcweb systems.hpp GracefulShutdown: host RequestedHostTransition
    // Off.
    if (leakPolicyPropsLoaded_ && criticalReactionType_ == "GracefulShutdown")
    {
        std::variant<std::string> transitionHost(
            "xyz.openbmc_project.State.Host.Transition.Off");

        dbusConnection->async_method_call(
            [sensorName = name](const boost::system::error_code& ec) {
                if (ec)
                {
                    std::cerr << "DiscreteLeakDetectSensor " << sensorName
                              << ": Failed to execute graceful shutdown due to "
                              << ec.message() << "\n";
                }
            },
            "xyz.openbmc_project.State.Host",
            "/xyz/openbmc_project/state/host0",
            "org.freedesktop.DBus.Properties", "Set",
            "xyz.openbmc_project.State.Host", "RequestedHostTransition",
            transitionHost);
    }
    else
    {
        std::variant<std::string> transitionChassis =
            state::convertForMessage(state::Chassis::Transition::Off);

        dbusConnection->async_method_call(
            [sensorName = name](const boost::system::error_code& ec) {
                if (ec)
                {
                    std::cerr << "DiscreteLeakDetectSensor " << sensorName
                              << ": Failed to execute chassis shutdown due to "
                              << ec.message() << "\n";
                }
            },
            state::Chassis::interface, "/xyz/openbmc_project/state/chassis0",
            "org.freedesktop.DBus.Properties", "Set", state::Chassis::interface,
            "RequestedPowerTransition", transitionChassis);
    }
    std::cout << "Issued async D-Bus request for shutdown transition (" << name
              << ").\n";
}

void DiscreteLeakDetectSensor::startLeakPolicyDiscovery()
{
    std::weak_ptr<DiscreteLeakDetectSensor> weak = weak_from_this();
    dbusConnection->async_method_call(
        [weak](const boost::system::error_code& ec,
               const GetSubTreeType& subtree) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (ec)
            {
                std::cerr
                    << "DiscreteLeakDetectSensor " << self->name
                    << ": mapper GetSubTree failed for LeakDetectionPolicy ("
                    << ec.message() << ")\n";
                return;
            }
            auto st = std::make_shared<GetSubTreeType>(subtree);
            self->leakPolicyScanIndex(0, st);
        },
        mapper::busName, mapper::path, mapper::interface, mapper::subtree,
        std::string(inventoryPath), 0,
        std::vector<std::string>{leakDetectionPolicyInterface});
}

void DiscreteLeakDetectSensor::leakPolicyScanIndex(
    size_t index, const std::shared_ptr<GetSubTreeType>& subtree)
{
    size_t i = index;
    while (i < subtree->size() && ((*subtree)[i].second.empty()))
    {
        i++;
    }
    if (i >= subtree->size())
    {
        std::cout
            << "DiscreteLeakDetectSensor " << name
            << ": no LeakDetectionPolicy row with LeakDetectorName match\n";
        return;
    }

    const std::string& path = (*subtree)[i].first;
    const std::string& service = (*subtree)[i].second.front().first;

    std::weak_ptr<DiscreteLeakDetectSensor> weak = weak_from_this();
    dbusConnection->async_method_call(
        [weak, i, subtree, path,
         service](const boost::system::error_code& ec,
                  const BasicVariantType& leakDetectorName) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            const std::string* namePtr =
                std::get_if<std::string>(&leakDetectorName);
            if (!ec && namePtr != nullptr && *namePtr == self->name)
            {
                self->bindLeakPolicy(path, service);
                return;
            }
            self->leakPolicyScanIndex(i + 1, subtree);
        },
        service, path, properties::interface, properties::get,
        std::string(leakDetectionPolicyInterface), "LeakDetectorName");
}

void DiscreteLeakDetectSensor::bindLeakPolicy(const std::string& path,
                                              const std::string& service)
{
    cancelPendingShutdown();
    leakPolicyPath_ = path;
    leakPolicyService_ = service;
    leakPolicyPropsLoaded_ = false;
    std::cout << "DiscreteLeakDetectSensor " << name
              << ": bound LeakDetectionPolicy at " << path << "\n";
    loadAllPolicyProperties();
    installLeakPolicyMatch();
}

void DiscreteLeakDetectSensor::loadAllPolicyProperties()
{
    if (!leakPolicyPath_ || leakPolicyService_.empty())
    {
        return;
    }
    std::weak_ptr<DiscreteLeakDetectSensor> weak = weak_from_this();
    dbusConnection->async_method_call(
        [weak](const boost::system::error_code& ec,
               const SensorBaseConfigMap& map) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (ec)
            {
                std::cerr << "DiscreteLeakDetectSensor " << self->name
                          << ": GetAll failed for LeakDetectionPolicy at "
                          << self->leakPolicyPath_.value_or("?") << ": "
                          << ec.message() << "\n";
                return;
            }
            self->applyPolicyConfigMap(map);
        },
        leakPolicyService_, *leakPolicyPath_, properties::interface, "GetAll",
        std::string(leakDetectionPolicyInterface));
}

void DiscreteLeakDetectSensor::applyPolicyConfigMap(
    const SensorBaseConfigMap& map)
{
    const std::string prevCritical = criticalReactionType_;
    const double prevDelay = policyReactionDelaySeconds_;

    auto applyString = [&map](const char* key, std::string& out) {
        auto it = map.find(key);
        if (it == map.end())
        {
            return;
        }
        try
        {
            out = std::visit(VariantToStringVisitor(), it->second);
        }
        catch (const std::invalid_argument&)
        {}
    };

    applyString("CriticalReactionType", criticalReactionType_);
    applyString("WarningReactionType", warningReactionType_);

    auto delayIt = map.find("ReactionDelaySeconds");
    if (delayIt != map.end())
    {
        try
        {
            policyReactionDelaySeconds_ =
                std::visit(VariantToDoubleVisitor(), delayIt->second);
        }
        catch (const std::invalid_argument&)
        {
            policyReactionDelaySeconds_ = 0.0;
        }
    }

    if constexpr (debug)
    {
        std::cout << "DiscreteLeakDetectSensor " << name
                  << ": policy CriticalReactionType=" << criticalReactionType_
                  << " WarningReactionType=" << warningReactionType_
                  << " ReactionDelaySeconds=" << policyReactionDelaySeconds_
                  << "\n";
    }

    std::cout << "DiscreteLeakDetectSensor " << name
              << ": leak shutdown action from policy \""
              << describeLeakShutdownAction(prevCritical, prevDelay)
              << "\" -> \""
              << describeLeakShutdownAction(criticalReactionType_,
                                            policyReactionDelaySeconds_)
              << "\" (CriticalReactionType / ReactionDelaySeconds)\n";

    leakPolicyPropsLoaded_ = true;

    if (leakLevel == LeakLevel::LEAKAGE && criticalReactionTriggersShutdown())
    {
        startShutdown();
    }
    else
    {
        cancelPendingShutdown();
    }
}

void DiscreteLeakDetectSensor::installLeakPolicyMatch()
{
    leakPolicyMatch_.reset();
    if (!leakPolicyPath_)
    {
        return;
    }

    std::string rule = std::string("type='signal',member='PropertiesChanged',"
                                   "path='") +
                       *leakPolicyPath_ +
                       "',interface='org.freedesktop.DBus.Properties',arg0='" +
                       leakDetectionPolicyInterface + "'";

    std::weak_ptr<DiscreteLeakDetectSensor> weak = weak_from_this();
    try
    {
        leakPolicyMatch_ = std::make_unique<sdbusplus::bus::match_t>(
            static_cast<sdbusplus::bus_t&>(*dbusConnection), rule,
            [weak](sdbusplus::message_t& /*m*/) {
                if (auto self = weak.lock())
                {
                    self->loadAllPolicyProperties();
                }
            });
    }
    catch (const std::exception& e)
    {
        std::cerr << "DiscreteLeakDetectSensor " << name
                  << ": failed to install policy match: " << e.what() << "\n";
    }
}

bool DiscreteLeakDetectSensor::criticalReactionTriggersShutdown() const
{
    return leakPolicyPropsLoaded_ && criticalReactionType_ != "None";
}

unsigned int DiscreteLeakDetectSensor::effectiveShutdownDelaySeconds() const
{
    if (!leakPolicyPropsLoaded_)
    {
        return 0U;
    }
    double d = policyReactionDelaySeconds_;
    if (!std::isfinite(d) || d < 0)
    {
        return 0U;
    }
    constexpr double maxU =
        static_cast<double>(std::numeric_limits<unsigned int>::max());
    if (d >= maxU)
    {
        return std::numeric_limits<unsigned int>::max();
    }
    return static_cast<unsigned int>(d);
}
