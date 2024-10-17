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

#include "DeviceMgmt.hpp"
#include "Thresholds.hpp"
#include "sensor.hpp"

#include <boost/asio/random_access_file.hpp>
#include <gpiod.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

enum class DetectorState
{
    NORMAL,
    LEAKAGE,
    FAULT
};

class LeakDetectSensor : public std::enable_shared_from_this<LeakDetectSensor>
{
  public:
    static constexpr const char* entityMgrConfigType = "VoltageLeakDetector";

    LeakDetectSensor(const std::string& readPath,
                     sdbusplus::asio::object_server& objectServer,
                     boost::asio::io_context& io,
                     std::shared_ptr<sdbusplus::asio::connection>& conn,
                     const std::string& sensorName,
                     const std::shared_ptr<I2CDevice>& i2cDevice,
                     const float pollRate, const double configLeakThreshold,
                     const double sensorMax, const double sensorMin,
                     const std::string& configurationPath, bool shutdownOnLeak,
                     const unsigned int shutdownDelaySeconds);
    ~LeakDetectSensor();
    std::string getSensorName();
    void setupRead();

  private:
    std::array<char, 128> readBuf{};
    std::shared_ptr<I2CDevice> i2cDevice;
    sdbusplus::asio::object_server& objServer;
    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;
    boost::asio::random_access_file inputDev;
    boost::asio::steady_timer waitTimer;
    boost::asio::steady_timer shutdownTimer;
    std::string name;
    std::string readPath;
    unsigned int sensorPollMs;
    double leakThreshold;
    double sensorMax;
    double sensorMin;
    DetectorState detectorState;
    bool sensorOverride;
    bool internalValueSet;
    std::string configurationPath;
    bool shutdownOnLeak;
    unsigned int shutdownDelaySeconds;
    std::shared_ptr<sdbusplus::asio::dbus_interface> sensorInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> thresholdInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> sensorAssociation;
    std::shared_ptr<sdbusplus::asio::dbus_interface> inventoryInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> inventoryAssociation;
    std::shared_ptr<sdbusplus::asio::dbus_interface> leakStateInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> opStateInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> stateAssociation;
    double detectorValue = std::numeric_limits<double>::quiet_NaN();

    void handleResponse(const boost::system::error_code& err, size_t bytesRead);
    void restartRead();
    void determineDetectorState(double detectorValue);
    void setDetectorState(DetectorState detectorState);
    void logCriticalEvent();
    void logFaultEvent();
    void startShutdown();
    void executeShutdown();
    void blinkFaultLed();
    void persistThreshold(double newThreshold);
    static std::string getDetectorStatusString(DetectorState detectorState);
    static std::string getDetectorStateString(DetectorState detectorState);
};
