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

#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Inventory/Item/LeakDetector/server.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>
#include <xyz/openbmc_project/State/LeakDetector/server.hpp>

enum class LeakLevel
{
    NORMAL,
    LEAKAGE
};

class DiscreteLeakDetectSensor :
    public std::enable_shared_from_this<DiscreteLeakDetectSensor>
{
  public:
    DiscreteLeakDetectSensor(sdbusplus::asio::object_server& objectServer,
                             std::shared_ptr<sdbusplus::asio::connection>& conn,
                             boost::asio::io_context& io,
                             const std::string& sensorType,
                             const std::string& sensorSysfsPath,
                             const std::string& sensorName,
                             const std::string& configurationPath,
                             const float pollRate, const uint8_t busId,
                             const uint8_t address, const std::string& driver);
    ~DiscreteLeakDetectSensor();

    void monitor(void);

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
    void createLeakageLogEntry();

    sdbusplus::asio::object_server& objServer;
    boost::asio::steady_timer waitTimer;
    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;
    LeakLevel leakLevel;

    std::shared_ptr<sdbusplus::asio::dbus_interface> inventoryInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> inventoryAssociation;
    std::shared_ptr<sdbusplus::asio::dbus_interface> stateInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> stateAssociation;
};
