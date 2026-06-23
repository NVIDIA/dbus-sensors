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

#include "MiContext.hpp"
#include "NVMeMiContext.hpp"
#include "NVMeMiManager.hpp"
#include "NVMeMiSensor.hpp"
#include "NVMeMiStatusSensor.hpp"
#include "Thresholds.hpp"
#include "Utils.hpp"
#include "VariantVisitors.hpp"

#ifdef __cplusplus
extern "C"
{
#endif

#include <nvme/mi.h>

#ifdef __cplusplus
}
#endif

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>
#include <tal.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

static NVMEMap nvmeDeviceMap;
static std::unique_ptr<NVMeMiManager> commManager;
// Map to store MCTP endpoint paths by EID for monitoring
static std::map<uint8_t, std::string> eidToMctpPath;
// Flag to prevent overlapping createSensors() execution
static std::atomic<bool> createSensorsInProgress{false};
// Track endpoints that have been explicitly paused (Degraded state)
static std::set<uint8_t> pausedEids;

NVMEMap& getNVMEMap()
{
    return nvmeDeviceMap;
}

using PropertiesMap =
    std::map<std::string,
             std::variant<uint8_t, uint32_t, uint64_t, std::vector<uint8_t>>>;
using EidPropertiesHandler =
    std::function<void(const boost::system::error_code&, const PropertiesMap&)>;
using EidPropertiesCreator = std::function<EidPropertiesHandler(
    uint8_t, const std::string&, const std::string&)>;
using MctpDiscoveryHandler = std::function<void(
    const boost::system::error_code&, const GetSubTreeType&)>;

// Forward declarations
static void discoverMctpEndpoint(
    uint8_t expectedEid,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const std::function<void(uint8_t eid, int net)>& onMctpFound);

// handle MCTP endpoint properties
static EidPropertiesCreator handleEidProperties(
    const std::function<void(uint8_t eid, int net)>& onMctpFound);

// handle MCTP endpoint discovery
template <typename HandlerType>
static MctpDiscoveryHandler handleMctpDiscovery(
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const HandlerType& handleEidProperties, uint8_t expectedEid,
    const char* mctpEndpointInterface);

// Stop polling for an EID
static void stopPollingForEid(uint8_t eid);

// Resume polling for an EID
static void resumePollingForEid(uint8_t eid);

static uint8_t extractAddress(const SensorBaseConfigMap& properties)
{
    auto findSlaveAddr = properties.find("Address");
    if (findSlaveAddr == properties.end())
    {
        return 0;
    }

    return std::visit(VariantToUnsignedIntVisitor(), findSlaveAddr->second);
}

static std::optional<std::string> extractSensorName(
    const std::string& path, const SensorBaseConfigMap& properties)
{
    auto findSensorName = properties.find("Name");
    if (findSensorName == properties.end())
    {
        lg2::error("could not determine configuration name for {PATH}", "PATH",
                   path);
        return std::nullopt;
    }

    return std::get<std::string>(findSensorName->second);
}

// MCTP discovery function to find EID and address
static void discoverMctpEndpoint(
    uint8_t expectedEid,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const std::function<void(uint8_t eid, int net)>& onMctpFound)
{
    const char* mctpEndpointInterface = "xyz.openbmc_project.MCTP.Endpoint";

    auto eidPropertiesHandler = handleEidProperties(onMctpFound);
    auto mctpDiscoveryHandler =
        handleMctpDiscovery(dbusConnection, eidPropertiesHandler, expectedEid,
                            mctpEndpointInterface);

    dbusConnection->async_method_call(
        mctpDiscoveryHandler, mapper::busName, mapper::path, mapper::interface,
        mapper::subtree, "/", 0,
        std::vector<std::string>{mctpEndpointInterface,
                                 "au.com.codeconstruct.MCTP.Endpoint1"});
}

// Helper function to handle MCTP endpoint properties
static EidPropertiesCreator handleEidProperties(
    const std::function<void(uint8_t eid, int net)>& onMctpFound)
{
    return [onMctpFound](uint8_t eid, const std::string& owner,
                         const std::string& path) {
        return [eid, owner, path, onMctpFound](
                   const boost::system::error_code ec,
                   const std::map<std::string,
                                  std::variant<uint8_t, uint32_t, uint64_t,
                                               std::vector<uint8_t>>>&
                       properties) mutable {
            if (ec)
            {
                lg2::error(
                    "Failed to get MCTP endpoint properties: {ERR} eid: {EID}",
                    "ERR", ec.message(), "EID", eid);
                return;
            }

            auto eidIt = properties.find("EID");
            if (eidIt == properties.end())
            {
                return;
            }

            uint8_t currentEid = 0;
            if (std::holds_alternative<uint8_t>(eidIt->second))
            {
                currentEid = std::get<uint8_t>(eidIt->second);
            }
            else
            {
                return;
            }

            if (currentEid != eid)
            {
                return;
            }
            auto netIt = properties.find("NetworkId");
            if (netIt == properties.end())
            {
                return;
            }
            auto net = std::get<uint32_t>(netIt->second);

            auto typeIt = properties.find("SupportedMessageTypes");
            if (typeIt != properties.end())
            {
                if (std::holds_alternative<std::vector<uint8_t>>(
                        typeIt->second))
                {
                    auto type = std::get<std::vector<uint8_t>>(typeIt->second);

                    // Find NVMeMI message type
                    auto it = std::find(type.begin(), type.end(),
                                        NVME_MI_MSGTYPE_NVME & 0x7F);
                    if (it == type.end())
                    {
                        lg2::debug("non-NVMeMI device: {EID}", "EID", eid);
                        return;
                    }
                }
            }

            // Store the MCTP endpoint path for this EID
            eidToMctpPath[eid] = path;

            onMctpFound(eid, net);
        };
    };
}

// Helper function to handle MCTP endpoint discovery
template <typename HandlerType>
static MctpDiscoveryHandler handleMctpDiscovery(
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const HandlerType& handleEidProperties, uint8_t expectedEid,
    const char* mctpEndpointInterface)
{
    return [&dbusConnection, handleEidProperties, expectedEid,
            mctpEndpointInterface](const boost::system::error_code ec,
                                   const GetSubTreeType& ret) {
        if (ec || ret.empty())
        {
            lg2::error("no MCTP endpoints found: {ERR} eid: {EID}", "ERR",
                       ec.message(), "EID", expectedEid);
            return;
        }

        for (const auto& [path, objects] : ret)
        {
            auto interfaces = objects.begin()->second;
            if (std::find(interfaces.begin(), interfaces.end(),
                          mctpEndpointInterface) != interfaces.end())
            {
                auto owner = objects.begin()->first;
                dbusConnection->async_method_call(
                    handleEidProperties(expectedEid, owner, path), owner, path,
                    "org.freedesktop.DBus.Properties", "GetAll",
                    mctpEndpointInterface);
            }
        }
    };
}

static std::shared_ptr<NVMeContext> provideMiContext(
    boost::asio::io_context& io, NVMEMap& map, uint8_t eid)
{
    auto findRoot = map.find(eid);
    if (findRoot != map.end())
    {
        return findRoot->second;
    }

    std::shared_ptr<NVMeContext> context =
        std::make_shared<NVMeMiContext>(io, eid);
    map[eid] = context;

    return context;
}

static void handleSensorConfigurations(
    boost::asio::io_context& io, sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const ManagedObjectType& sensorConfigurations)
{
    for (const auto& [_, nvmeContextPtr] : nvmeDeviceMap)
    {
        if (nvmeContextPtr)
        {
            nvmeContextPtr->close();
        }
    }

    // Clear commManager contexts to release sensor references
    // This allows sensor destructors to run, which removes D-Bus interfaces
    if (commManager)
    {
        for (const auto& [eid, _] : nvmeDeviceMap)
        {
            commManager->removeContext(eid);
        }
    }
    nvmeDeviceMap.clear();

    // Store sensor configurations for later creation after MCTP discovery
    struct SensorConfig
    {
        std::string interfacePath;
        std::string sensorName;
        uint8_t eid;
        std::vector<thresholds::Threshold> thresholds;
        uint8_t pollRate;
    };

    std::vector<SensorConfig> pendingSensors;

    // iterate through all found configurations
    for (const auto& [interfacePath, sensorData] : sensorConfigurations)
    {
        // Check for temperature sensor configuration (Type: "NVME1000")
        auto tempSensorBase =
            sensorData.find(configInterfaceName(NVMeSensor::sensorType));
        if (tempSensorBase != sensorData.end())
        {
            const SensorBaseConfigMap& sensorConfig = tempSensorBase->second;
            std::optional<std::string> sensorName =
                extractSensorName(interfacePath, sensorConfig);
            uint8_t eid = extractAddress(sensorConfig);

            if (!sensorName || eid == 0)
            {
                lg2::error("invalid sensor configuration: {PATH} Eid: {EID}",
                           "PATH", interfacePath, "EID", eid);
                continue;
            }

            std::vector<thresholds::Threshold> sensorThresholds;
            if (!parseThresholdsFromConfig(sensorData, sensorThresholds))
            {
                lg2::error("error populating thresholds for {SENSOR}", "SENSOR",
                           *sensorName);
            }

            // Extract poll rate from sensor configuration
            uint8_t pollRate = static_cast<uint8_t>(
                getPollRate(sensorConfig, 1.0F)); // Default to 1 second

            pendingSensors.emplace_back(interfacePath, *sensorName, eid,
                                        std::move(sensorThresholds), pollRate);
        }

        auto statusSensorBase =
            sensorData.find(configInterfaceName(NVMeStatusSensor::sensorType));
        if (statusSensorBase != sensorData.end())
        {
            const SensorBaseConfigMap& sensorConfig = statusSensorBase->second;
            std::optional<std::string> sensorName =
                extractSensorName(interfacePath, sensorConfig);
            uint8_t eid = extractAddress(sensorConfig);
            if (!sensorName || eid == 0)
            {
                lg2::error("invalid sensor configuration: {PATH} Eid: {EID}",
                           "PATH", interfacePath, "EID", eid);
                continue;
            }

            // Extract poll rate from sensor configuration
            uint8_t pollRate = static_cast<uint8_t>(
                getPollRate(sensorConfig, 1.0F)); // Default to 1 second

            pendingSensors.emplace_back(interfacePath, *sensorName, eid,
                                        std::vector<thresholds::Threshold>{},
                                        pollRate);
        }
    }

    // Group sensor configurations by EID
    std::map<uint8_t, std::vector<SensorConfig>> sensorsByEid;
    for (const auto& sensorConfig : pendingSensors)
    {
        sensorsByEid[sensorConfig.eid].push_back(sensorConfig);
    }

    // Start MCTP discovery for each unique EID
    for (const auto& [eid, sensorConfigs] : sensorsByEid)
    {
        discoverMctpEndpoint(
            eid, dbusConnection,
            [&io, &objectServer, &dbusConnection, sensorConfigs,
             eid](uint8_t discoveredEid, int net) {
                // Check if discovered EID matches expected EID
                if (discoveredEid != eid)
                {
                    lg2::debug(
                        "EID mismatch: expected {EXPECTED}, found {FOUND}",
                        "EXPECTED", eid, "FOUND", discoveredEid);
                    return;
                }

                std::shared_ptr<NVMeContext> context =
                    provideMiContext(io, nvmeDeviceMap, discoveredEid);

                auto nvmeContext =
                    std::static_pointer_cast<NVMeMiContext>(context);

                if (commManager)
                {
                    if (!commManager->addContext(nvmeContext, net,
                                                 discoveredEid))
                    {
                        // If it already exists, we don't want to create
                        // duplicate sensors
                        auto existingContext =
                            nvmeDeviceMap.find(discoveredEid);
                        if (existingContext != nvmeDeviceMap.end() &&
                            existingContext->second != context)
                        {
                            // Context exists but it's different - this is the
                            // duplicate case
                            lg2::debug("Context for eid {EID} already exists",
                                       "EID", discoveredEid);
                        }
                        else
                        {
                            lg2::error(
                                "Failed to add context in NVMeMiManager for eid: {EID}",
                                "EID", discoveredEid);
                        }
                        return;
                    }
                }

                try
                {
                    // Find minimum poll rate from all sensors for this EID
                    uint8_t minPollRate = UINT8_MAX;
                    for (const auto& sensorConfig : sensorConfigs)
                    {
                        if (sensorConfig.pollRate < minPollRate)
                        {
                            minPollRate = sensorConfig.pollRate;
                        }
                    }

                    // Configure context with minimum poll rate
                    nvmeContext->setPollRate(minPollRate);

                    // Create both temperature and status sensors for this EID
                    for (const auto& sensorConfig : sensorConfigs)
                    {
                        if (!sensorConfig.thresholds.empty())
                        {
                            // Create temperature sensor (has thresholds)
                            auto thresholds =
                                sensorConfig.thresholds; // Make a copy
                            std::shared_ptr<NVMeSensor> sensorPtr =
                                std::make_shared<NVMeSensor>(
                                    objectServer, io, dbusConnection,
                                    sensorConfig.sensorName,
                                    std::move(thresholds),
                                    sensorConfig.interfacePath, discoveredEid);

                            context->addSensor<NVMeSensor>(sensorPtr);
                        }
                        else
                        {
                            // Create status sensor (no thresholds)
                            std::shared_ptr<NVMeStatusSensor> statusSensorPtr =
                                std::make_shared<NVMeStatusSensor>(
                                    objectServer, io, dbusConnection,
                                    sensorConfig.sensorName,
                                    sensorConfig.interfacePath, discoveredEid);

                            context->addSensor<NVMeStatusSensor>(
                                statusSensorPtr);
                        }
                    }

                    lg2::debug(
                        "polling nvme devices for eid: {EID} with {COUNT} sensors poll rate: {RATE} seconds",
                        "EID", static_cast<int>(discoveredEid), "COUNT",
                        sensorConfigs.size(), "RATE",
                        static_cast<int>(minPollRate));
                    context->pollNVMeDevices();
                }
                catch (const std::invalid_argument& ex)
                {
                    lg2::error("Failed to add sensors for eid {EID} {ERROR}",
                               "EID", discoveredEid, "ERROR", ex.what());
                }
            });
    }

    lg2::debug(
        "NVMe sensor discovery initiated for {COUNT} sensor configurations.",
        "COUNT", sensorsByEid.size());
}

void createSensors(boost::asio::io_context& io,
                   sdbusplus::asio::object_server& objectServer,
                   std::shared_ptr<sdbusplus::asio::connection>& dbusConnection)
{
    // Set flag at entry
    createSensorsInProgress = true;

    auto getter = std::make_shared<GetSensorConfiguration>(
        dbusConnection, [&io, &objectServer, &dbusConnection](
                            const ManagedObjectType& sensorConfigurations) {
            handleSensorConfigurations(io, objectServer, dbusConnection,
                                       sensorConfigurations);
            // Clear flag when sensor configuration handling completes
            createSensorsInProgress = false;
        });
    getter->getConfiguration(std::vector<std::string_view>{
        NVMeSensor::sensorType, NVMeStatusSensor::sensorType});
}

static void stopPollingForEid(uint8_t eid)
{
    auto findContext = nvmeDeviceMap.find(eid);
    if (findContext != nvmeDeviceMap.end())
    {
        lg2::info(
            "Pausing polling for EID {EID} due to connectivity degradation",
            "EID", eid);

        // Mark this EID as explicitly paused
        pausedEids.insert(eid);

        // Only cancel the timer - keep context and sensors intact
        findContext->second->close();
    }
}

static void resumePollingForEid(uint8_t eid)
{
    // Only resume if this endpoint was explicitly paused
    if (pausedEids.find(eid) == pausedEids.end())
    {
        lg2::debug(
            "Ignoring resume request for EID {EID} - endpoint was not paused",
            "EID", eid);
        return;
    }

    auto findContext = nvmeDeviceMap.find(eid);
    if (findContext != nvmeDeviceMap.end())
    {
        lg2::info("Resuming polling for EID {EID} - connectivity restored",
                  "EID", eid);

        // Remove from paused set
        pausedEids.erase(eid);

        // Restart the polling timer - context and sensors already exist
        findContext->second->pollNVMeDevices();
    }
    else
    {
        // Context not found - this shouldn't normally happen
        lg2::warning("Cannot resume polling for EID {EID} - context not found. "
                     "Endpoint may have been removed.",
                     "EID", eid);
        pausedEids.erase(eid);
    }
}

// Handle MCTP endpoint connectivity property changes
static void handleMctpConnectivityChange(sdbusplus::message_t& message)
{
    if (message.is_method_error())
    {
        lg2::error("PropertiesChanged callback method error");
        return;
    }

    std::string interface;
    std::map<std::string, std::variant<std::string>> changedProperties;
    std::vector<std::string> invalidatedProperties;

    message.read(interface, changedProperties, invalidatedProperties);

    // Check if Connectivity property changed
    auto connectivityIt = changedProperties.find("Connectivity");
    if (connectivityIt == changedProperties.end())
    {
        return;
    }

    std::string connectivity = std::get<std::string>(connectivityIt->second);
    std::string path = message.get_path();

    lg2::info("MCTP endpoint {PATH} Connectivity changed to {STATE}", "PATH",
              path, "STATE", connectivity);

    // Expected Connectivity values: "Available" or "Degraded"
    // Find the EID for this endpoint path
    uint8_t foundEid = 0;
    for (const auto& [eid, storedPath] : eidToMctpPath)
    {
        if (storedPath == path)
        {
            foundEid = eid;
            break;
        }
    }

    if (foundEid == 0)
    {
        // Endpoint not found in our mapping - ignore this change
        // This can happen if we haven't discovered this endpoint yet
        return;
    }

    // Handle connectivity state changes
    // "Degraded" - Device is unresponsive (e.g., during power transition)
    //              Stop polling immediately to avoid communication errors
    // "Available" - Device is responsive, resume normal polling
    if (connectivity == "Degraded")
    {
        stopPollingForEid(foundEid);
    }
    else if (connectivity == "Available")
    {
        resumePollingForEid(foundEid);
    }
}

// Cleanup MCTP endpoint contexts for removed EIDs
static void handleMctpEndpointRemoved(const std::set<uint8_t>& removedEids)
{
    lg2::info("Processing MCTP endpoint removal for {COUNT} endpoints", "COUNT",
              removedEids.size());

    // Process all pending removals
    for (uint8_t eid : removedEids)
    {
        // Stop polling and remove context
        auto findContext = nvmeDeviceMap.find(eid);
        if (findContext != nvmeDeviceMap.end())
        {
            lg2::info("Removing context for EID {EID}", "EID", eid);
            findContext->second->close();

            // Remove from communication manager
            if (commManager)
            {
                commManager->removeContext(eid);
            }

            nvmeDeviceMap.erase(findContext);
        }

        // Remove from path mapping
        eidToMctpPath.erase(eid);

        // Remove from paused set if present
        pausedEids.erase(eid);
    }
}

int main()
{
    // Ignore SIGPIPE - handle pipe errors via errno instead of process
    // termination. This prevents crashes when writing to closed pipes during
    // context cleanup race conditions.
    signal(SIGPIPE, SIG_IGN);

    boost::asio::io_context io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    systemBus->request_name("xyz.openbmc_project.NVMeSensor");
    sdbusplus::asio::object_server objectServer(systemBus, true);
    objectServer.add_manager("/xyz/openbmc_project/sensors");

    commManager = std::make_unique<NVMeMiManager>(io);
    commManager->start();

    boost::asio::post(io,
                      [&]() { createSensors(io, objectServer, systemBus); });

    boost::asio::steady_timer filterTimer(io);
    std::function<void(sdbusplus::message_t&)> eventHandler = [&filterTimer,
                                                               &io,
                                                               &objectServer,
                                                               &systemBus](
                                                                  sdbusplus::
                                                                      message_t&) {
        // Check if createSensors is already in progress
        if (createSensorsInProgress.load())
        {
            lg2::debug(
                "createSensors already in progress, ignoring property change");
            return;
        }

        // this implicitly cancels the timer
        filterTimer.expires_after(std::chrono::seconds(1));

        filterTimer.async_wait([&](const boost::system::error_code& ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                return; // we're being canceled
            }

            if (ec)
            {
                lg2::error("Error: {ERROR}", "ERROR", ec.message());
                return;
            }

            // Check again before executing
            if (createSensorsInProgress.exchange(true))
            {
                lg2::debug("createSensors already queued, skipping");
                return;
            }

            boost::asio::post(io, [&io, &objectServer, &systemBus]() {
                createSensors(io, objectServer, systemBus);
            });
        });
    };

    // Debounce timers for MCTP endpoint add/remove events
    boost::asio::steady_timer mctpEndpointAddedDebounceTimer(io);
    boost::asio::steady_timer mctpEndpointRemovedDebounceTimer(io);

    // Track EIDs pending removal during debounce period
    auto pendingRemovedEids = std::make_shared<std::set<uint8_t>>();

    std::vector<std::unique_ptr<sdbusplus::bus::match_t>> matches =
        setupPropertiesChangedMatches(
            *systemBus,
            std::to_array<std::string_view>(
                {NVMeSensor::sensorType, NVMeStatusSensor::sensorType}),
            eventHandler);

    // Watch for MCTP endpoint additions on codeconstruct MCTP stack
    auto mctpEndpointAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*systemBus),
        "type='signal',member='InterfacesAdded',path_namespace='/au/com/"
        "codeconstruct/mctp1'",
        [&mctpEndpointAddedDebounceTimer, &io, &objectServer,
         &systemBus](sdbusplus::message_t&) {
            // Check if createSensors is already in progress
            if (createSensorsInProgress.load())
            {
                lg2::debug(
                    "createSensors already in progress, ignoring InterfacesAdded signal");
                return;
            }

            // Debounce: cancel any existing timer and start a new 3-second
            // delay to collect multiple endpoint additions into a single
            // discovery operation
            mctpEndpointAddedDebounceTimer.expires_after(
                std::chrono::seconds(3));

            mctpEndpointAddedDebounceTimer.async_wait(
                [&io, &objectServer,
                 &systemBus](const boost::system::error_code& ec) {
                    if (ec == boost::asio::error::operation_aborted)
                    {
                        return; // Timer was canceled
                    }

                    if (ec)
                    {
                        lg2::error("MCTP endpoint added debounce timer error: "
                                   "{ERROR}",
                                   "ERROR", ec.message());
                        return;
                    }

                    // Check again before queueing work (defense in depth)
                    if (createSensorsInProgress.exchange(true))
                    {
                        lg2::debug(
                            "createSensors already queued, skipping duplicate");
                        return;
                    }

                    lg2::info("Processing debounced MCTP endpoint added event");
                    boost::asio::post(io, [&io, &objectServer, &systemBus]() {
                        createSensors(io, objectServer, systemBus);
                    });
                });
        });

    // Watch for MCTP endpoint removals on codeconstruct MCTP stack
    auto mctpEndpointRemovedMatch = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*systemBus),
        "type='signal',member='InterfacesRemoved',path_namespace='/au/com/"
        "codeconstruct/mctp1'",
        [&mctpEndpointRemovedDebounceTimer,
         pendingRemovedEids](sdbusplus::message_t& msg) {
            if (msg.is_method_error())
            {
                lg2::error("InterfacesRemoved callback method error");
                return;
            }

            sdbusplus::object_path path;
            std::vector<std::string> interfaces;

            msg.read(path, interfaces);

            // Check if this is a codeconstruct MCTP endpoint
            bool isMctpEndpoint = false;
            for (const auto& interfaceName : interfaces)
            {
                if (interfaceName == "au.com.codeconstruct.MCTP.Endpoint1")
                {
                    isMctpEndpoint = true;
                    break;
                }
            }

            if (!isMctpEndpoint)
            {
                return;
            }

            lg2::info("MCTP endpoint removed: {PATH}", "PATH", path.str);

            // Find and collect the EID associated with this path
            uint8_t removedEid = 0;
            for (const auto& [eid, storedPath] : eidToMctpPath)
            {
                if (storedPath == path.str)
                {
                    removedEid = eid;
                    break;
                }
            }

            // Add to pending removal set (collects all EIDs during debounce)
            if (removedEid != 0)
            {
                pendingRemovedEids->insert(removedEid);
            }

            // Debounce: cancel any existing timer and start a new 3-second
            // delay
            mctpEndpointRemovedDebounceTimer.expires_after(
                std::chrono::seconds(3));

            mctpEndpointRemovedDebounceTimer.async_wait(
                [pendingRemovedEids](const boost::system::error_code& ec) {
                    if (ec == boost::asio::error::operation_aborted)
                    {
                        return; // Timer was canceled
                    }

                    if (ec)
                    {
                        lg2::error(
                            "MCTP endpoint removed debounce timer error: "
                            "{ERROR}",
                            "ERROR", ec.message());
                        return;
                    }

                    lg2::info(
                        "Processing debounced MCTP endpoint removed event");

                    // Call cleanup handler with all collected EIDs
                    handleMctpEndpointRemoved(*pendingRemovedEids);

                    // Clear the pending removals
                    pendingRemovedEids->clear();
                });
        });

    // Watch for MCTP endpoint Connectivity property changes on codeconstruct
    // MCTP stack - this monitors the Connectivity property to detect when
    // devices become unresponsive during power transitions
    auto mctpConnectivityMatch = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*systemBus),
        "type='signal',member='PropertiesChanged',interface='org.freedesktop."
        "DBus.Properties',path_namespace='/au/com/codeconstruct/mctp1',"
        "arg0='au.com.codeconstruct.MCTP.Endpoint1'",
        [](sdbusplus::message_t& msg) { handleMctpConnectivityChange(msg); });

    setupManufacturingModeMatch(*systemBus);
#ifdef NVIDIA_SHMEM
    if (tal::TelemetryAggregator::namespaceInit(tal::ProcessType::Producer,
                                                "nvmesensor"))
    {
        std::cout
            << "Successfully registered TAL namespaceInit for NVMe Sensor\n";
    }
#endif
    io.run();
}
