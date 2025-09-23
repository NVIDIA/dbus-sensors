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

#include <nvme/mi.h>

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
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

static NVMEMap nvmeDeviceMap;
static std::unique_ptr<NVMeMiManager> commManager;

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

static uint8_t extractAddress(const SensorBaseConfigMap& properties)
{
    auto findSlaveAddr = properties.find("Address");
    if (findSlaveAddr == properties.end())
    {
        return 0;
    }

    return std::visit(VariantToUnsignedIntVisitor(), findSlaveAddr->second);
}

static std::optional<std::string>
    extractSensorName(const std::string& path,
                      const SensorBaseConfigMap& properties)
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
            onMctpFound(static_cast<uint8_t>(eid), net);
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

static std::shared_ptr<NVMeContext>
    provideMiContext(boost::asio::io_context& io, NVMEMap& map, uint8_t eid)
{
    auto findRoot = map.find(eid);
    if (findRoot != map.end())
    {
        return findRoot->second;
    }

    std::shared_ptr<NVMeContext> context = std::make_shared<NVMeMiContext>(io,
                                                                           eid);
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
                getPollRate(sensorConfig, 1.0f)); // Default to 1 second

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
                getPollRate(sensorConfig, 1.0f)); // Default to 1 second

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
        discoverMctpEndpoint(eid, dbusConnection,
                             [&io, &objectServer, &dbusConnection,
                              sensorConfigs,
                              eid](uint8_t discoveredEid, int net) {
            // Check if discovered EID matches expected EID
            if (discoveredEid != eid)
            {
                lg2::debug("EID mismatch: expected {EXPECTED}, found {FOUND}",
                           "EXPECTED", eid, "FOUND", discoveredEid);
                return;
            }

            std::shared_ptr<NVMeContext> context =
                provideMiContext(io, nvmeDeviceMap, discoveredEid);

            auto nvmeContext = std::static_pointer_cast<NVMeMiContext>(context);

            if (commManager)
            {
                if (!commManager->addContext(nvmeContext, net, discoveredEid))
                {
                    lg2::error(
                        "Failed to add context in NVMeMiManager for eid: {EID}",
                        "EID", discoveredEid);
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
                                sensorConfig.sensorName, std::move(thresholds),
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

                        context->addSensor<NVMeStatusSensor>(statusSensorPtr);
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
                lg2::error("Failed to add sensors for eid {EID} {ERROR}", "EID",
                           discoveredEid, "ERROR", ex.what());
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
    auto getter = std::make_shared<GetSensorConfiguration>(
        dbusConnection, [&io, &objectServer, &dbusConnection](
                            const ManagedObjectType& sensorConfigurations) {
        handleSensorConfigurations(io, objectServer, dbusConnection,
                                   sensorConfigurations);
    });
    getter->getConfiguration(std::vector<std::string>{
        NVMeSensor::sensorType, NVMeStatusSensor::sensorType});
}

static void interfaceRemoved(sdbusplus::message_t& message, NVMEMap& contexts)
{
    if (message.is_method_error())
    {
        lg2::error("interfacesRemoved callback method error");
        return;
    }

    sdbusplus::message::object_path path;
    std::vector<std::string> interfaces;

    message.read(path, interfaces);

    for (auto& [eid, context] : contexts)
    {
        // Check for temperature sensors
        std::optional<std::shared_ptr<NVMeSensor>> sensor =
            context->getSensorAtPath<NVMeSensor>(path);
        if (sensor)
        {
            auto interface = std::find(interfaces.begin(), interfaces.end(),
                                       (*sensor)->configInterface);
            if (interface != interfaces.end())
            {
                context->removeSensor<NVMeSensor>(sensor.value());
                continue;
            }
        }

        // Check for status sensors
        std::optional<std::shared_ptr<NVMeStatusSensor>> statusSensor =
            context->getSensorAtPath<NVMeStatusSensor>(path);
        if (statusSensor)
        {
            // Use the correct interface name for status sensors
            std::string statusConfigInterface =
                configInterfaceName(NVMeStatusSensor::sensorType);
            auto interface = std::find(interfaces.begin(), interfaces.end(),
                                       statusConfigInterface);
            if (interface != interfaces.end())
            {
                context->removeSensor<NVMeStatusSensor>(statusSensor.value());
            }
        }
    }
}

int main()
{
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
    std::function<void(sdbusplus::message_t&)> eventHandler =
        [&filterTimer, &io, &objectServer, &systemBus](sdbusplus::message_t&) {
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

            createSensors(io, objectServer, systemBus);
        });
    };

    std::vector<std::unique_ptr<sdbusplus::bus::match_t>> matches =
        setupPropertiesChangedMatches(
            *systemBus,
            std::to_array<const char*>(
                {NVMeSensor::sensorType, NVMeStatusSensor::sensorType}),
            eventHandler);

    // Watch for entity-manager to remove configuration interfaces
    // so the corresponding sensors can be removed.
    auto ifaceRemovedMatch = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*systemBus),
        "type='signal',member='InterfacesRemoved',arg0path='" +
            std::string(inventoryPath) + "/'",
        [](sdbusplus::message_t& msg) {
        interfaceRemoved(msg, nvmeDeviceMap);
    });

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
