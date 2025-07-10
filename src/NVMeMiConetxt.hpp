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

#include "NVMeMiSensor.hpp"
#include "NVMeMiStatusSensor.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <memory>
#include <stdexcept>
#include <variant>

using SensorVariant = std::variant<std::shared_ptr<NVMeSensor>,
                                   std::shared_ptr<NVMeStatusSensor>>;

class NVMeContext
{
  public:
    NVMeContext(boost::asio::io_context& io, int eid) :
        scanTimer(io), eid(eid), pollCursor(sensors.end())
    {}

    virtual ~NVMeContext()
    {
        scanTimer.cancel();
        sensors.clear();
    }

    template <typename SensorType>
    void addSensor(const std::shared_ptr<SensorType>& sensor)
    {
        sensors.emplace_back(sensor);
    }

    template <typename SensorType>
    std::optional<std::shared_ptr<SensorType>>
        getSensorAtPath(const std::string& path)
    {
        for (auto& sensorVariant : sensors)
        {
            if (std::holds_alternative<std::shared_ptr<SensorType>>(
                    sensorVariant))
            {
                auto sensor =
                    std::get<std::shared_ptr<SensorType>>(sensorVariant);
                if (sensor->configurationPath == path)
                {
                    return sensor;
                }
            }
        }
        return std::nullopt;
    }

    template <typename SensorType>
    void removeSensor(const std::shared_ptr<SensorType>& sensor)
    {
        auto found = std::find_if(sensors.begin(), sensors.end(),
                                  [&sensor](const SensorVariant& variant) {
            if (std::holds_alternative<std::shared_ptr<SensorType>>(variant))
            {
                return std::get<std::shared_ptr<SensorType>>(variant) == sensor;
            }
            return false;
        });

        if (found == sensors.end())
        {
            return;
        }

        if (pollCursor == sensors.end())
        {
            sensors.erase(found);
            return;
        }

        if (*pollCursor != *found)
        {
            sensors.erase(found);
            return;
        }

        pollCursor = sensors.erase(found);
    }

    virtual void close()
    {
        scanTimer.cancel();
    }

    virtual void pollNVMeDevices() = 0;

    virtual void readAndProcessNVMeSensor() = 0;

  protected:
    boost::asio::steady_timer scanTimer;
    uint8_t eid;
    std::list<SensorVariant> sensors;
    std::list<SensorVariant>::iterator pollCursor;
};

using NVMEMap =
    boost::container::flat_map<std::string, std::shared_ptr<NVMeContext>>;

NVMEMap& getNVMEMap();
