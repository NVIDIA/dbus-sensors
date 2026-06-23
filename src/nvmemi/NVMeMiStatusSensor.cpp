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

#include "NVMeMiStatusSensor.hpp"

#include "Utils.hpp"

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/server.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/common.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

NVMeStatusSensor::NVMeStatusSensor(
    sdbusplus::asio::object_server& objectServer,
    boost::asio::io_context& /*unused*/,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::string& sensorName, const std::string& sensorConfiguration,
    const uint8_t eid) :
    StatusInterface(
        static_cast<sdbusplus::bus_t&>(*conn),
        ("/xyz/openbmc_project/sensors/drive/" + escapeName(sensorName))
            .c_str(),
        StatusInterface::action::defer_emit),
    eid(eid), name(sensorName), configurationPath(sensorConfiguration),
    objServer(objectServer)
{
    // Create drive interface
    driveInterface = objectServer.add_interface(
        ("/xyz/openbmc_project/sensors/drive/" + escapeName(sensorName)),
        DriveInterface::interface);

    // Create association interface
    association = objectServer.add_interface(
        ("/xyz/openbmc_project/sensors/drive/" + escapeName(sensorName)),
        association::interface);

    // Set up associations
    fs::path p(sensorConfiguration);
    AssociationList assocs = {};
    assocs.emplace_back(
        std::make_tuple("chassis", "all_sensors", p.parent_path().string()));
    sdbusplus::xyz::openbmc_project::Association::server::Definitions::
        associations(assocs);

    // Initialize drive interface
    if (!driveInterface->initialize())
    {
        std::cerr << "error initializing drive interface\n";
    }

    // Set initial status - mark as present but not functional until first
    // update
    sdbusplus::xyz::openbmc_project::Inventory::server::Item::present(true);
    sdbusplus::xyz::openbmc_project::State::Decorator::server::
        OperationalStatus::functional(false);
    sdbusplus::xyz::openbmc_project::State::Decorator::server::
        OperationalStatus::state(
            sdbusplus::xyz::openbmc_project::State::Decorator::server::
                OperationalStatus::StateType::None);
}

NVMeStatusSensor::~NVMeStatusSensor()
{
    objServer.remove_interface(driveInterface);
    objServer.remove_interface(association);
}

bool NVMeStatusSensor::sample()
{
    if (scanDelay == 0)
    {
        scanDelay = 5; // 5 seconds
    }

    scanDelay--;
    return scanDelay == 0;
}

void NVMeStatusSensor::updateStatus(bool present, bool functional, bool fault)
{
    // Update present status
    sdbusplus::xyz::openbmc_project::Inventory::server::Item::present(present);

    // Update functional status
    sdbusplus::xyz::openbmc_project::State::Decorator::server::
        OperationalStatus::functional(functional);

    // Update fault status
    if (fault)
    {
        sdbusplus::xyz::openbmc_project::State::Decorator::server::
            OperationalStatus::state(
                sdbusplus::xyz::openbmc_project::State::Decorator::server::
                    OperationalStatus::StateType::Fault);
    }
    else
    {
        sdbusplus::xyz::openbmc_project::State::Decorator::server::
            OperationalStatus::state(
                sdbusplus::xyz::openbmc_project::State::Decorator::server::
                    OperationalStatus::StateType::None);
    }
}
