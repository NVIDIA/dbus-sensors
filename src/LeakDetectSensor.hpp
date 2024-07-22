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

#pragma once

#include "DeviceMgmt.hpp"
#include "Thresholds.hpp"
#include "sensor.hpp"

#include <gpiod.hpp>
#include <boost/asio/random_access_file.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// TODO: Expand to include other leakage states, such as sensor faults,
//       small leak and large leaks
enum class LeakLevel
{
    NORMAL,
    LEAKAGE
};

class LeakDetectSensor :
    public Sensor,
    public std::enable_shared_from_this<LeakDetectSensor>
{
  public:
    static constexpr const char* entityMgrConfigType = "LeakDetector";

    LeakDetectSensor(const std::string& readPath,
                     sdbusplus::asio::object_server& objectServer,
                     std::shared_ptr<sdbusplus::asio::connection>& conn,
                     boost::asio::io_context& io,
                     const std::string& sensorName,
                     std::vector<thresholds::Threshold>&& thresholds,
                     const std::shared_ptr<I2CDevice>& i2cDevice,
                     const float pollRate,
                     PowerState readState,
                     const std::string& configurationPath);
    ~LeakDetectSensor() override;
    void setupRead();
    void checkThresholds() override;

  private:
    std::array<char, 128> readBuf{};
    std::shared_ptr<I2CDevice> i2cDevice;
    sdbusplus::asio::object_server& objServer;
    boost::asio::random_access_file inputDev;
    boost::asio::steady_timer waitTimer;
    std::string readPath;
    unsigned int sensorPollMs;
    LeakLevel leakLevel;
    void handleResponse(const boost::system::error_code& err, size_t bytesRead);
    void restartRead();
    void setLeakLevel(LeakLevel leakLevel);
    void logEvent(LeakLevel leakLevel);
    static std::string getLeakLevelStatusName(LeakLevel leaklevel);
};
