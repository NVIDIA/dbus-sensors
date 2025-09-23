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

#include "FileHandle.hpp"
#include "MiContext.hpp"
#include "NVMeMiSensor.hpp"
#include "NVMeMiStatusSensor.hpp"

#include <endian.h>
#include <nvme/types.h>

// NVMe-MI NSS (NVM Subsystem Status) constants
constexpr uint8_t NVME_MI_NSS_DRIVE_FAULT = (1 << 5); // Drive Fault Status

// NVMe-MI CTEMP (Composite Temperature) constants
constexpr uint8_t NVME_MI_CTEMP_NO_DATA =
    0x80; // No temperature data or >5s old
constexpr uint8_t NVME_MI_CTEMP_SENSOR_FAIL =
    0x81; // Temperature sensor failure
constexpr uint8_t NVME_MI_CTEMP_MAX_TEMP = 0x7F; // 127°C or higher
constexpr uint8_t NVME_MI_CTEMP_MIN_TEMP = 0xC4; // -60°C or lower
constexpr uint8_t NVME_MI_CTEMP_TWOS_COMP_START =
    0xC5; // Start of two's complement range

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/impl/read.hpp>
#include <boost/asio/impl/write.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/streambuf.hpp>
#include <phosphor-logging/lg2.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

NVMeMiContext::NVMeMiContext(boost::asio::io_context& io, uint8_t eid) :
    NVMeContext::NVMeContext(io, eid), eid(eid), requestPipe(-1),
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
    scanTimer.expires_after(
        std::chrono::milliseconds(static_cast<unsigned int>(pollRate * 1000)));
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
    // Use NVM Subsystem Health Status Polling instead of log page
    // This is the proper NVMe-MI command for health status
    constexpr uint8_t NVME_MI_CMD_HEALTH_STATUS_POLL = 0x01;
    cmdArray[0] = NVME_MI_CMD_HEALTH_STATUS_POLL;
    cmdArray[1] = 0; // nsid

    /* Issue the request */
    boost::asio::async_write(
        requestStream, boost::asio::buffer(cmdArray.data(), cmdArray.size()),
        [weakSelf{weak_from_this()}](boost::system::error_code ec,
                                     std::size_t) {
        if (ec)
        {
            lg2::error("Got error send NVMe-MI request: {ERROR}", "ERROR",
                       ec.message().c_str());

            if (auto self = weakSelf.lock())
            {
                self->pollNVMeDevices();
            }
        }
    });

    auto response = std::make_shared<boost::asio::streambuf>();
    response->prepare(1);

    /* Gather the response and dispatch for parsing */
    boost::asio::async_read(
        responseStream, *response,
        [response, lengthRead = false, actualLength = 0U,
         weakSelf{weak_from_this()}](const boost::system::error_code& ec,
                                     std::size_t n) mutable -> std::size_t {
        if (ec)
        {
            lg2::error("Got error reading NVMe-MI response: {ERROR}", "ERROR",
                       ec.message().c_str());
            if (auto self = weakSelf.lock())
            {
                self->pollNVMeDevices();
            }
            return static_cast<std::size_t>(0);
        }

        if (!lengthRead && n >= 4)
        {
            std::istream is(response.get());
            uint32_t len = 0;
            for (int i = 0; i < 4; ++i)
            {
                uint8_t byte = static_cast<uint8_t>(is.get());
                len |= static_cast<uint32_t>(byte) << (i * 8);
            }
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
        if (ec || length == 0)
        {
            lg2::error(
                "Got error reading NVMe-MI response: {ERROR} length: {LEN}",
                "ERROR", ec.message().c_str(), "LEN", length);

            if (auto self = weakSelf.lock())
            {
                self->pollNVMeDevices();
            }
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

static double
    getTemperatureReading(struct nvme_mi_nvm_ss_health_status* healthLog)
{
    uint8_t ctemp = healthLog->ctemp;

    // Handle special temperature values according to NVMe specification
    if (ctemp == NVME_MI_CTEMP_NO_DATA)
    {
        // 80h: No temperature data or temperature data is more than 5s old
        return std::numeric_limits<double>::quiet_NaN();
    }

    if (ctemp == NVME_MI_CTEMP_SENSOR_FAIL)
    {
        // 81h: Temperature sensor failure
        return std::numeric_limits<double>::quiet_NaN();
    }

    if (ctemp == NVME_MI_CTEMP_MAX_TEMP)
    {
        // 7Fh: 127°C or higher
        return 127.0;
    }

    if (ctemp == NVME_MI_CTEMP_MIN_TEMP)
    {
        // C4h: -60°C or lower
        return -60.0;
    }

    if (ctemp <= 0x7E)
    {
        // 00h to 7Eh: Temperature is measured in degrees Celsius (0°C to 126°C)
        return static_cast<double>(ctemp);
    }

    if (ctemp >= NVME_MI_CTEMP_TWOS_COMP_START)
    {
        // C5h to FFh: Temperature measured in degrees Celsius is represented in
        // two's complement Convert from two's complement 8-bit to signed
        // integer
        int8_t temp_signed = static_cast<int8_t>(ctemp);
        return static_cast<double>(temp_signed);
    }

    // Reserved values (82h to C3h) - return NaN
    return std::numeric_limits<double>::quiet_NaN();
}

void NVMeMiContext::processResponse(void* msg, size_t len)
{
    if (msg == nullptr || len < sizeof(struct nvme_mi_nvm_ss_health_status))
    {
        consecutiveFailures++;
        lg2::warning("Consecutive failures for eid: {EID}: {FAILURES}/{MAX}",
                     "EID", eid, "FAILURES", consecutiveFailures, "MAX",
                     maxConsecutiveFailures);

        if (consecutiveFailures < maxConsecutiveFailures)
        {
            pollNVMeDevices();
        }
        return;
    }

    // Reset failure counter on successful response
    consecutiveFailures = 0;

    struct nvme_mi_nvm_ss_health_status* healthLog =
        static_cast<struct nvme_mi_nvm_ss_health_status*>(msg);

    // Process all sensors in this context
    for (auto& sensorVariant : sensors)
    {
        std::visit([healthLog](auto& sensor) {
            using SensorType = std::decay_t<decltype(sensor)>;

            if constexpr (std::is_same_v<SensorType,
                                         std::shared_ptr<NVMeSensor>>)
            {
                // Update temperature sensor using health data
                double value = getTemperatureReading(healthLog);

                if (std::isnan(value))
                {
                    // Temperature data is unavailable, old, or sensor failed
                    sensor->incrementError();
                }
                else
                {
                    sensor->updateValue(value);
                }
            }
            else if constexpr (std::is_same_v<
                                   SensorType,
                                   std::shared_ptr<NVMeStatusSensor>>)
            {
                // Update status sensor using health data
                bool present = true;
                bool functional = true;
                bool fault = false;

                // Check for drive fault from bit 5 of NVM Subsystem Status
                // (nss)
                if (healthLog->nss & NVME_MI_NSS_DRIVE_FAULT)
                {
                    fault = true;
                    functional = false;
                }

                // Check for critical warnings in the health status
                if (healthLog->sw != 0)
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
