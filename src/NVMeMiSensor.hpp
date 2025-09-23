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

#include "sensor.hpp"

#include <boost/asio/io_context.hpp>

class NVMeSensor : public Sensor
{
  public:
    static constexpr const char* sensorType = "NVME1000";

    NVMeSensor(sdbusplus::asio::object_server& objectServer,
               boost::asio::io_context& io,
               std::shared_ptr<sdbusplus::asio::connection>& conn,
               const std::string& sensorName,
               std::vector<thresholds::Threshold>&& thresholds,
               const std::string& sensorConfiguration, uint8_t eid);
    ~NVMeSensor() override;

    NVMeSensor& operator=(const NVMeSensor& other) = delete;

    bool sample();

    const uint8_t eid;

  private:
    // The time to defer sensor polling if the error count exceeds the threshold
    const unsigned int scanDelayTicks = 5 * 60;
    sdbusplus::asio::object_server& objServer;
    unsigned int scanDelay{0};

    void checkThresholds() override;
};
