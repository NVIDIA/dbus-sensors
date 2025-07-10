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
    if (pollCursor != sensors.end())
    {
        auto& sensorVariant = *pollCursor++;

        std::visit([weakSelf{weak_from_this()}](auto& sensor) {
            using SensorType = std::decay_t<decltype(sensor)>;

            if constexpr (std::is_same_v<SensorType,
                                         std::shared_ptr<NVMeSensor>>)
            {
                // Handle temperature sensor
                if (!sensor->readingStateGood())
                {
                    sensor->markAvailable(false);
                    sensor->updateValue(
                        std::numeric_limits<double>::quiet_NaN());
                    if (auto self = weakSelf.lock())
                    {
                        self->readAndProcessNVMeSensor();
                    }
                    return;
                }

                /* Potentially defer sampling the sensor if it is in error */
                if (!sensor->sample())
                {
                    if (auto self = weakSelf.lock())
                    {
                        self->readAndProcessNVMeSensor();
                    }
                    return;
                }

                // Send temperature sensor command
                if (auto self = weakSelf.lock())
                {
                    self->sendNVMeCommand(
                        NVME_LOG_LID_SMART, "temperature", sensor,
                        [weakSelf](std::shared_ptr<NVMeSensor>& sensor,
                                   void* data, size_t len) {
                        if (auto self = weakSelf.lock())
                        {
                            self->processResponse(sensor, data, len);
                        }
                    });
                }
            }
            else if constexpr (std::is_same_v<
                                   SensorType,
                                   std::shared_ptr<NVMeStatusSensor>>)
            {
                if (!sensor->sample())
                {
                    if (auto self = weakSelf.lock())
                    {
                        self->readAndProcessNVMeSensor();
                    }
                    return;
                }

                // Send status sensor command
                if (auto self = weakSelf.lock())
                {
                    self->sendNVMeCommand(
                        NVME_LOG_LID_SMART, "status", sensor,
                        [weakSelf](std::shared_ptr<NVMeStatusSensor>& sensor,
                                   void* data, size_t len) {
                        if (auto self = weakSelf.lock())
                        {
                            self->processResponse(sensor, data, len);
                        }
                    });
                }
            }
        }, sensorVariant);

        return;
    }

    // Reset cursor and start next polling cycle
    pollCursor = sensors.end();
    this->pollNVMeDevices();
}

void NVMeMiContext::pollNVMeDevices()
{
    pollCursor = sensors.begin();

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

template void
    NVMeMiContext::processResponse(std::shared_ptr<NVMeSensor>& sensor,
                                   void* msg, size_t len);
template void
    NVMeMiContext::processResponse(std::shared_ptr<NVMeStatusSensor>& sensor,
                                   void* msg, size_t len);

template <typename SensorType, typename ProcessFunc>
void NVMeMiContext::sendNVMeCommand(uint8_t command,
                                    const std::string& sensorType,
                                    std::shared_ptr<SensorType> sensor,
                                    ProcessFunc processFunc)
{
    std::array<uint8_t, 4> cmdArray{};
    cmdArray[0] = command;
    cmdArray[1] = 0; // nsid

    /* Issue the request */
    boost::asio::async_write(
        requestStream, boost::asio::buffer(cmdArray.data(), cmdArray.size()),
        [cmdArray, sensorType](boost::system::error_code ec, std::size_t) {
        if (ec)
        {
            lg2::error("Got error writing {SENSOR} query: {ERROR}", "SENSOR",
                       sensorType, "ERROR", ec.message().c_str());
        }
    });

    auto response = std::make_shared<boost::asio::streambuf>();
    response->prepare(1);

    /* Gather the response and dispatch for parsing */
    boost::asio::async_read(responseStream, *response,
                            [response, lengthRead = false, actualLength = 0u,
                             sensorType](const boost::system::error_code& ec,
                                         std::size_t n) mutable -> std::size_t {
        if (ec)
        {
            lg2::error("Got error reading NVMe-MI {SENSOR} command: {ERROR}",
                       "SENSOR", sensorType, "ERROR", ec.message().c_str());
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
                            [weakSelf{weak_from_this()}, sensor, response,
                             processFunc](const boost::system::error_code& ec,
                                          std::size_t length) mutable {
        if (ec)
        {
            lg2::error("Got error reading NVMe-MI command: {ERROR}", "ERROR",
                       ec.message().c_str());
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

            /* Update the sensor using the provided process function */
            processFunc(sensor, data.data(), data.size());

            /* Continue with next sensor */
            self->readAndProcessNVMeSensor();
        }
    });
}

static double getTemperatureReading(nvme_smart_log* log)
{
    uint16_t temp = (static_cast<uint16_t>(log->temperature[1]) << 8) |
                    static_cast<uint16_t>(log->temperature[0]);

    return static_cast<double>(temp) - 273.15;
}

template <typename SensorType>
void NVMeMiContext::processResponse(std::shared_ptr<SensorType>& sensor,
                                    void* msg, size_t len)
{
    if constexpr (std::is_same_v<SensorType, NVMeSensor>)
    {
        // Handle temperature sensor response
        if (msg == nullptr)
        {
            sensor->incrementError();
            return;
        }

        double value = getTemperatureReading(static_cast<nvme_smart_log*>(msg));
        lg2::debug("reading value: {VALUE} eid: {EID}", "VALUE", value, "EID",
                   static_cast<int>(sensor->eid));
        if (!std::isfinite(value))
        {
            sensor->incrementError();
            return;
        }

        sensor->updateValue(value);
    }
    else if constexpr (std::is_same_v<SensorType, NVMeStatusSensor>)
    {
        // Handle status sensor response
        if (msg == nullptr || len < sizeof(nvme_smart_log))
        {
            return;
        }

        nvme_smart_log* smart_log = static_cast<nvme_smart_log*>(msg);

        // Extract status information from SMART log
        bool present = true;    // If we got a response, drive is present
        bool functional = true; // Assume functional unless we detect issues
        bool fault = false;     // Assume no fault unless we detect issues

        // Check for critical warnings in the health status
        if (smart_log->critical_warning != 0)
        {
            std::cout << "critical_warning: " << smart_log->critical_warning
                      << "\n";
            fault = true;
            functional = false;
        }

        lg2::info(
            "esent: {PRESENT} functional: {FUNCTIONAL} fault: {FAULT} eid: {EID}",
            "PRESENT", present, "FUNCTIONAL", functional, "FAULT", fault, "EID",
            static_cast<int>(sensor->eid));
        sensor->updateStatus(present, functional, fault);
    }
}
