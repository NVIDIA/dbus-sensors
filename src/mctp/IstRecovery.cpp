/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
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
#include "IstRecovery.hpp"

#include "Utils.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/property.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>

#include <array>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

PHOSPHOR_LOG2_USING;

namespace
{
constexpr const char* istPath = "/com/nvidia/vera/ist";
constexpr const char* istStateIface = "com.nvidia.vera.ist.State";
constexpr const char* istInProgressProperty = "IstInProgress";
constexpr const char* mapperService = "xyz.openbmc_project.ObjectMapper";
constexpr const char* mapperPath = "/xyz/openbmc_project/object_mapper";
constexpr const char* mapperInterface = "xyz.openbmc_project.ObjectMapper";

using GetObjectReply =
    std::vector<std::pair<std::string, std::vector<std::string>>>;
} // namespace

DBusIstRecovery::DBusIstRecovery(
    const std::shared_ptr<sdbusplus::asio::connection>& bus) : bus(bus)
{
    const std::string matchSpec =
        sdbusplus::bus::match::rules::propertiesChangedNamespace(
            istPath, istStateIface);

    istStateMatch = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*bus), matchSpec,
        [this](sdbusplus::message_t& msg) {
            std::string iface;
            std::vector<std::pair<std::string, BasicVariantType>> props;
            try
            {
                msg.read(iface, props);
            }
            catch (const std::exception&)
            {
                // A type BasicVariantType cannot hold aborts the whole decode
                // and would strand istInProgress at its last value.
                refreshState();
                return;
            }

            for (const auto& [name, value] : props)
            {
                if (name != istInProgressProperty)
                {
                    continue;
                }
                const bool* inProgress = std::get_if<bool>(&value);
                if (inProgress == nullptr)
                {
                    continue;
                }
                istInProgress = *inProgress;
                info("IST in progress is now {IST_IN_PROGRESS}",
                     "IST_IN_PROGRESS", istInProgress);
            }

            // Retrying is what gets the owner watch installed when the
            // service appears after an empty lookup.
            if (istService.empty())
            {
                findIstService();
            }
        });

    findIstService();
}

void DBusIstRecovery::findIstService()
{
    bus->async_method_call(
        [this, weak = std::weak_ptr<void>(lifetime)](
            const boost::system::error_code& ec, const GetObjectReply& reply) {
            if (weak.expired())
            {
                return;
            }
            if (ec || reply.empty())
            {
                info("Object mapper found no owner for the IST state path");
                istService.clear();
                return;
            }
            watchIstService(reply.front().first);
        },
        mapperService, mapperPath, mapperInterface, "GetObject", istPath,
        std::array<const char*, 1>{istStateIface});
}

void DBusIstRecovery::watchIstService(const std::string& service)
{
    istService = service;

    // mctpreactor can outlive or restart under the IST service, and the
    // property does not change again for the rest of a run once it is true.
    // Without this, a restart mid-run would read nothing, latch false and
    // start clear-halting the CPU it is meant to leave alone.
    istOwnerMatch = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*bus),
        sdbusplus::bus::match::rules::nameOwnerChanged(istService),
        [this](sdbusplus::message_t& msg) {
            std::string name;
            std::string oldOwner;
            std::string newOwner;
            try
            {
                msg.read(name, oldOwner, newOwner);
            }
            catch (const std::exception&)
            {
                return;
            }

            if (newOwner.empty())
            {
                istInProgress = false;
                return;
            }
            readIstInProgress();
        });

    readIstInProgress();
}

void DBusIstRecovery::refreshState()
{
    if (istService.empty())
    {
        findIstService();
    }

    readIstInProgress();
}

void DBusIstRecovery::readIstInProgress()
{
    if (istService.empty())
    {
        info("No IST state path owner known; assuming no run");
        istInProgress = false;
        return;
    }

    sdbusplus::asio::getProperty<bool>(
        *bus, istService, istPath, istStateIface, istInProgressProperty,
        [this, weak = std::weak_ptr<void>(lifetime)](
            const boost::system::error_code& ec, bool inProgress) {
            if (weak.expired())
            {
                return;
            }
            if (ec)
            {
                info("Could not read IST state, assuming no run: {ERROR}",
                     "ERROR", ec.message());
                istInProgress = false;
                return;
            }
            istInProgress = inProgress;
            debug("IST in progress read as {IST_IN_PROGRESS}",
                  "IST_IN_PROGRESS", istInProgress);
        });
}

bool DBusIstRecovery::isIstInProgress() const
{
    return istInProgress;
}
