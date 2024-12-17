/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
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
#include <boost/container/flat_map.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sensor.hpp>

#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <vector>
// use operandMap to store the sensor names and their mathematical signs (-1 or
// 1)
using operandMap = boost::container::flat_map<std::string, int>;
struct SynthesizedSensor :
    public Sensor,
    std::enable_shared_from_this<SynthesizedSensor>
{
    operandMap sensorOperands;

    SynthesizedSensor(std::shared_ptr<sdbusplus::asio::connection>& conn,
                      const std::string& name,
                      const std::string& sensorConfiguration,
                      sdbusplus::asio::object_server& objectServer,
                      std::vector<thresholds::Threshold>&& thresholdData);
    ~SynthesizedSensor() override;

    void checkThresholds() override;
    void updateReading();
    void setupMatches();

  private:
    double lastReading = 0.0;

    std::vector<sdbusplus::bus::match_t> matches;
    double inletTemp = std::numeric_limits<double>::quiet_NaN();
    boost::container::flat_map<std::string, double> powerReadings;
    sdbusplus::asio::object_server& objServer;
    std::chrono::time_point<std::chrono::steady_clock> lastTime;
    static double getTotalCFM();
    bool calculate(double& val);
};
