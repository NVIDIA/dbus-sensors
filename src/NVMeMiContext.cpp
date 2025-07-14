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
#include "NVMeMiContext.hpp"

#include "MiContext.hpp"
#include "NVMeMiSensor.hpp"
#include "Utils.hpp"

#include <endian.h>
#include <libnvme-mi.h>

#include <boost/asio/io_context.hpp>
#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

NVMeMiContext::NVMeMiContext(boost::asio::io_context& io, uint8_t eid) :
    NVMeContext::NVMeContext(io, eid), io(io), eid(eid), requestPipe(-1),
    responsePipe(-1), requestStream(io), responseStream(io)
{}

void NVMeMiContext::setupPipes(FileHandle reqPipe, FileHandle respPipe,
                               boost::asio::posix::stream_descriptor reqStream,
                               boost::asio::posix::stream_descriptor respStream)
{
    requestPipe = std::move(reqPipe);
    responsePipe = std::move(respPipe);
    requestStream = std::move(reqStream);
    responseStream = std::move(respStream);
}

void NVMeMiContext::readAndProcessNVMeSensor()
{
    bool shouldSendCommand = false;

    for (auto& sensorVariant : sensors)
    {
        std::visit([&shouldSendCommand](auto& sensor) {
            using SensorType = std::decay_t<decltype(sensor)>;

            if constexpr (std::is_same_v<SensorType,
                                         std::shared_ptr<NVMeSensor>>)
            {
                // Check temperature sensor conditions
                if (!sensor->readingStateGood())
                {
                    sensor->markAvailable(false);
                    sensor->updateValue(
                        std::numeric_limits<double>::quiet_NaN());
                    return;
                }

                if (sensor->sample())
                {
                    shouldSendCommand = true;
                }
            }
            else if constexpr (std::is_same_v<
                                   SensorType,
                                   std::shared_ptr<NVMeStatusSensor>>)
            {
                // Check status sensor conditions
                if (sensor->sample())
                {
                    shouldSendCommand = true;
                }
            }
        }, sensorVariant);
    }

    // Send unified command if any sensor is ready
    if (shouldSendCommand)
    {
        sendNVMeMICommand();
    }
    else
    {
        pollNVMeDevices();
    }
}

void NVMeMiContext::pollNVMeDevices()
{
    scanTimer.expires_after(std::chrono::seconds(1));
    scanTimer.async_wait([weakSelf{weak_from_this()}](
                             const boost::system::error_code errorCode) {
        if (errorCode == boost::asio::error::operation_aborted)
        {
            return;
        }

        if (errorCode)
        {
            lg2::error("Error in NVMe-Mi context: {ERROR}", "ERROR",
                       errorCode.message().c_str());
            return;
        }

        if (auto self = weakSelf.lock())
        {
            self->readAndProcessNVMeSensor();
        }
    });
}

void NVMeMiContext::close()
{
    // Call the base class close method
    NVMeContext::close();

    // Close the stream descriptors
    requestStream.close();
    responseStream.close();
}

void NVMeMiContext::sendNVMeMICommand()
{
    std::array<uint8_t, 4> cmdArray{};
    cmdArray[0] = NVME_LOG_LID_SMART;
    cmdArray[1] = 0; // nsid

    /* Issue the request */
    boost::asio::async_write(
        requestStream, boost::asio::buffer(cmdArray.data(), cmdArray.size()),
        [cmdArray](boost::system::error_code ec, std::size_t) {
        if (ec)
        {
            lg2::error("Got error writing NVMe-MI command: {ERROR}", "ERROR",
                       ec.message().c_str());
        }
    });

    auto response = std::make_shared<boost::asio::streambuf>();
    response->prepare(1);

    /* Gather the response and dispatch for parsing */
    boost::asio::async_read(
        responseStream, *response,
        [response, lengthRead = false,
         actualLength = 0u](const boost::system::error_code& ec,
                            std::size_t n) mutable -> std::size_t {
        if (ec)
        {
            lg2::error("Got error reading unified NVMe-MI command: {ERROR}",
                       "ERROR", ec.message().c_str());
            return static_cast<std::size_t>(0);
        }

        if (!lengthRead && n >= 4)
        {
            std::istream is(response.get());
            uint32_t len = 0;
            is.read(reinterpret_cast<char*>(&len), 4);
            actualLength = le32toh(len);
            lengthRead = true;
        }

        if (lengthRead && n == 4 + actualLength)
        {
            return static_cast<std::size_t>(0); // Complete
        }

        if (lengthRead)
        {
            return actualLength; // Need more data
        }

        return static_cast<std::size_t>(4 - n); // Need more length bytes
    },
        [weakSelf{weak_from_this()}, response](
            const boost::system::error_code& ec, std::size_t length) mutable {
        if (ec)
        {
            lg2::error("Got error reading unified NVMe-MI command: {ERROR}",
                       "ERROR", ec.message().c_str());
            return;
        }

        if (length == 0)
        {
            lg2::error("Invalid message length: {LENGTH}", "LENGTH", length);
            return;
        }

        if (auto self = weakSelf.lock())
        {
            /* Deserialise the response */
            std::istream is(response.get());
            std::vector<char> data(response->size());
            is.read(data.data(), data.size());

            /* Update all sensors with the same response data */
            self->processResponse(data.data(), data.size());
        }
    });
}

static double getTemperatureReading(nvme_smart_log* log)
{
    uint16_t temp = (static_cast<uint16_t>(log->temperature[1]) << 8) |
                    static_cast<uint16_t>(log->temperature[0]);

    return static_cast<double>(temp) - 273.15;
}

void NVMeMiContext::processResponse(void* msg, size_t len)
{
    if (msg == nullptr || len < sizeof(nvme_smart_log))
    {
        lg2::error("Invalid response data for unified processing");
        return;
    }

    nvme_smart_log* smart_log = static_cast<nvme_smart_log*>(msg);

    // Process all sensors in this context
    for (auto& sensorVariant : sensors)
    {
        std::visit([this, smart_log](auto& sensor) {
            using SensorType = std::decay_t<decltype(sensor)>;

            if constexpr (std::is_same_v<SensorType,
                                         std::shared_ptr<NVMeSensor>>)
            {
                // Update temperature sensor
                double value = getTemperatureReading(smart_log);
                if (std::isfinite(value))
                {
                    sensor->updateValue(value);
                }
                else
                {
                    sensor->incrementError();
                }
            }
            else if constexpr (std::is_same_v<
                                   SensorType,
                                   std::shared_ptr<NVMeStatusSensor>>)
            {
                // Update status sensor
                bool present = true;
                bool functional = true;
                bool fault = false;

                // Check for critical warnings in the health status
                if (smart_log->critical_warning != 0)
                {
                    fault = true;
                    functional = false;
                }

                sensor->updateStatus(present, functional, fault);
            }
        }, sensorVariant);
    }
    // Schedule next polling cycle after updating both sensors
    pollNVMeDevices();
}