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
#include <sdbusplus/asio/object_server.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Drive/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/server.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp>

#include <memory>
#include <stdexcept>
#include <string>

using StatusInterface = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Inventory::server::Item,
    sdbusplus::xyz::openbmc_project::State::Decorator::server::
        OperationalStatus,
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;

using DriveInterface =
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Drive;

class NVMeStatusSensor :
    public StatusInterface,
    public std::enable_shared_from_this<NVMeStatusSensor>
{
  public:
    static constexpr const char* sensorType = "Nvmem2";

    NVMeStatusSensor(sdbusplus::asio::object_server& objectServer,
                     boost::asio::io_context& io,
                     std::shared_ptr<sdbusplus::asio::connection>& conn,
                     const std::string& sensorName,
                     const std::string& sensorConfiguration, uint8_t eid,
                     uint8_t pollRate);
    ~NVMeStatusSensor() override;

    NVMeStatusSensor& operator=(const NVMeStatusSensor& other) = delete;

    bool sample();
    void updateStatus(bool present, bool functional, bool fault);

    const int eid;
    std::string name;
    std::string configurationPath;

  private:
    sdbusplus::asio::object_server& objServer;
    unsigned int scanDelay{0};
    std::shared_ptr<sdbusplus::asio::dbus_interface> driveInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> association;
    uint8_t pollRate;
};
