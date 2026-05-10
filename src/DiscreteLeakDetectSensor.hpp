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
#pragma once

#include "Utils.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

enum class LeakLevel
{
    NORMAL,
    LEAKAGE
};

class DiscreteLeakDetectSensor :
    public std::enable_shared_from_this<DiscreteLeakDetectSensor>
{
  public:
    DiscreteLeakDetectSensor(
        sdbusplus::asio::object_server& objectServer,
        std::shared_ptr<sdbusplus::asio::connection>& conn,
        boost::asio::io_context& io, const std::string& sensorType,
        const std::string& sensorSysfsPath, const std::string& sensorName,
        const std::string& configurationPath, float pollRate, uint8_t busId,
        uint8_t address, const std::string& driver);
    ~DiscreteLeakDetectSensor();

    /** Resolve xyz.openbmc_project.Configuration.LeakDetectionPolicy for this
     *  detector name (Redfish LeakDetector Id) and subscribe to updates. */
    void startLeakPolicyDiscovery();

    void monitor();

    std::string sensorType;
    std::string sysfsPath;
    std::string name;
    unsigned int sensorPollMs;
    uint8_t busId;
    uint8_t address;
    std::string driver;

  private:
    int getLeakInfo();
    static int readLeakValue(const std::string& filePath);
    static std::string getLeakLevelStatusName(LeakLevel leaklevel);
    static std::string getLeakLevelStateString(LeakLevel leaklevel);
    static std::string getLeakResourceStatusName(LeakLevel leaklevel);
    std::string getLeakResourceResolutionName(LeakLevel leaklevel);
    static std::string getLeakResourceSeverityName(LeakLevel leaklevel);
    void createLeakageLogEntry();

    void leakPolicyScanIndex(size_t index,
                             const std::shared_ptr<GetSubTreeType>& subtree);
    void bindLeakPolicy(const std::string& path, const std::string& service);
    void loadAllPolicyProperties();
    void applyPolicyConfigMap(const SensorBaseConfigMap& map);
    void installLeakPolicyMatch();
    bool criticalReactionTriggersShutdown() const;
    unsigned int effectiveShutdownDelaySeconds() const;
    void cancelPendingShutdown();

    sdbusplus::asio::object_server& objServer;
    boost::asio::steady_timer waitTimer;
    boost::asio::steady_timer shutdownTimer;
    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;
    LeakLevel leakLevel{LeakLevel::NORMAL};

    void startShutdown();
    void executeShutdown();

    std::shared_ptr<sdbusplus::asio::dbus_interface> inventoryInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> inventoryAssociation;
    std::shared_ptr<sdbusplus::asio::dbus_interface> stateInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> opStateInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> stateAssociation;

    unsigned int uid;
    bool startedShutdownTimer;
    uint64_t shutdownTimerGeneration_{0};
    static unsigned int lastUID;

    std::unique_ptr<sdbusplus::bus::match_t> leakPolicyMatch_;
    std::optional<std::string> leakPolicyPath_;
    std::string leakPolicyService_;
    std::string criticalReactionType_{"None"};
    std::string warningReactionType_{"None"};
    double policyReactionDelaySeconds_{0.0};
    bool leakPolicyPropsLoaded_{false};
};
