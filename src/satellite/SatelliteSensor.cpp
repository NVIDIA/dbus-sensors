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
#include "SatelliteSensor.hpp"

#include "SensorPaths.hpp"
#include "Thresholds.hpp"
#include "Utils.hpp"
#include "sensor.hpp"
#include "vHMCShmReader.hpp"

#include <fcntl.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/container/flat_map.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>

#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

extern "C"
{
#include <linux/i2c-dev.h>
}

constexpr const bool debug = false;

constexpr const char* configInterface =
    "xyz.openbmc_project.Configuration.Satellite";
constexpr const char* sensorRootPath = "/xyz/openbmc_project/sensors/";
constexpr const char* objectType = "Satellite";

boost::container::flat_map<std::string, std::unique_ptr<SatelliteSensor>>
    sensors;

namespace
{

vhmc_shm::vHMCShmReader& vhmcShmReader()
{
    static vhmc_shm::vHMCShmReader reader;
    return reader;
}

bool shmRecordToDouble(const vhmc_shm::SensorRecord& rec, double& out)
{
    if (rec.stale != 0)
    {
        return false;
    }
    if (rec.type == vhmc_shm::SensorDataType::Double)
    {
        out = rec.value.dVal;
        return true;
    }
    if (rec.type == vhmc_shm::SensorDataType::Uint32)
    {
        out = static_cast<double>(rec.value.u32Val);
        return true;
    }
    if (rec.type == vhmc_shm::SensorDataType::Uint64)
    {
        out = static_cast<double>(rec.value.u64Val);
        return true;
    }
    return false;
}

// True only while waiting for the vHMC writer to publish a ready header for
// the first time. Once the writer has been seen, a not-ready result is a real
// failure and must not be silenced.
bool awaitingFirstShmWriter(int err)
{
    return err == static_cast<int>(vhmc_shm::SensorError::WriterNotReady) &&
           !vhmcShmReader().hasSeenWriter();
}

const char* invalidReadReason(int ret, const std::string& valueType)
{
    if (ret >= 0)
    {
        return "out of range";
    }
    if (valueType == "SHM")
    {
        return vhmc_shm::sensorErrorToString(
            static_cast<vhmc_shm::SensorError>(ret));
    }
    return "read failed";
}

} // namespace

SatelliteSensor::SatelliteSensor(
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    boost::asio::io_context& io, const std::string& sensorName,
    const std::string& sensorConfiguration, const std::string& objType,
    sdbusplus::asio::object_server& objectServer,
    std::vector<thresholds::Threshold>&& thresholdData, uint8_t busId,
    uint8_t addr, uint16_t offset, uint16_t staleOffset, size_t staleBit,
    std::string& sensorType, std::string& valueType, size_t pollTime,
    double minVal, double maxVal, const PowerState powerState) :
    Sensor(escapeName(sensorName), std::move(thresholdData),
           sensorConfiguration, objType, false, false, maxVal, minVal, conn,
           powerState),
    name(escapeName(sensorName)), busId(busId), addr(addr), offset(offset),
    staleOffset(staleOffset), staleBit(staleBit), sensorType(sensorType),
    valueType(valueType), objectServer(objectServer), waitTimer(io),
    pollRate(pollTime)
{
    invalidLogInterval = std::max<size_t>(1, invalidLogReminderSec / pollRate);
    // make the string to lowercase for Dbus sensor type
    for (auto& c : sensorType)
    {
        c = tolower(c);
    }
    std::string sensorPath = sensorRootPath + sensorType + "/";

    sensorInterface =
        objectServer.add_interface(sensorPath + name, sensorValueInterface);

    for (const auto& threshold : thresholds)
    {
        std::string interface = thresholds::getInterface(threshold.level);
        thresholdInterfaces[static_cast<size_t>(threshold.level)] =
            objectServer.add_interface(sensorPath + name, interface);
    }
    association =
        objectServer.add_interface(sensorPath + name, association::interface);

    if (sensorType == "temperature")
    {
        setInitialProperties(sensor_paths::unitDegreesC);
    }
    else if (sensorType == "power")
    {
        setInitialProperties(sensor_paths::unitWatts);
    }
    else if (sensorType == "energy")
    {
        setInitialProperties(sensor_paths::unitJoules);
    }
    else if (sensorType == "voltage")
    {
        setInitialProperties(sensor_paths::unitVolts);
    }
    else
    {
        lg2::error("no sensor type found");
    }
}

void SatelliteSensor::deactivate()
{
    markAvailable(false);
    waitTimer.cancel();
}

SatelliteSensor::~SatelliteSensor()
{
    waitTimer.cancel();
    for (const auto& iface : thresholdInterfaces)
    {
        objectServer.remove_interface(iface);
    }
    objectServer.remove_interface(sensorInterface);
    objectServer.remove_interface(association);
}

void SatelliteSensor::init()
{
    markAvailable(true);
    restartRead();
}

void SatelliteSensor::checkThresholds()
{
    thresholds::checkThresholds(this);
}

template <typename T>
int i2cCmd(uint8_t bus, uint8_t addr, size_t offset, T* reading, uint8_t length,
           size_t staleOffset, size_t staleBit)
{
    std::string i2cBus = "/dev/i2c-" + std::to_string(bus);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    int fd = open(i2cBus.c_str(), O_RDWR);
    if (fd < 0)
    {
        lg2::error(" unable to open i2c device {BUS} err={FD}", "BUS", i2cBus,
                   "FD", fd);
        return -1;
    }

    unsigned long funcs = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (ioctl(fd, I2C_FUNCS, &funcs) < 0)
    {
        lg2::error(" not support I2C_FUNCS");
        close(fd);
        return -1;
    }

    int ret = 0;
    std::array<uint8_t, 8> cmd{};
    T data = 0;

    if (length > sizeof(data))
    {
        lg2::error("wrong i2c data length");
        close(fd);
        return -1;
    }

    // clang-format off
    std::array<struct i2c_msg, 2> msgs = {{
                                              {
                                                  // write offset
                                                  .addr = addr,
                                                  .flags = 0,
                                                  .len = 2,
                                                  .buf = cmd.data(),
                                              },
                                              {
                                                  // read data from the offset
                                                  .addr = addr,
                                                  .flags = I2C_M_RD,
                                                  .len = length,
                                                  .buf = (uint8_t*)&data
                                              }
                                          }};
    // clang-format on

    struct i2c_rdwr_ioctl_data args = {msgs.data(), 2};

    // handle two bytes offset
    if (offset > 255)
    {
        msgs[0].len = 2;
        msgs[0].buf[0] = offset >> 8;
        msgs[0].buf[1] = offset & 0xFF;
    }
    else
    {
        msgs[0].len = 1;
        msgs[0].buf[0] = offset & 0xFF;
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    ret = ioctl(fd, I2C_RDWR, &args);
    if (ret < 0)
    {
        close(fd);
        return ret;
    }
    // there is no value updated from HMC if reading data is all 0xff
    uint8_t emptyBytes = 0;
    uint8_t* ptr = (uint8_t*)&data;
    for (int i = 0; i < length; i++, ptr++)
    {
        if (*ptr != 0xFF)
        {
            continue;
        }
        emptyBytes++;
    }

    // there is no reading if all bytes are 0xff
    if (emptyBytes == length)
    {
        close(fd);
        return -1;
    }

    // Optional stale check: read a byte at staleOffset and treat bit staleBit
    // as stale flag; if set, treat as invalid (return -1). Uses a separate
    // struct with the same write-then-read pattern as the main read.
    // StaleOffset 0 means disabled (no check); valid offset when
    // enabled 1..0xFFFF.
    if (staleOffset != i2cStaleCheckDisabled)
    {
        // Valid offset is 1..0xFFFF (same as main offset). Valid bit is 0..7.
        if (staleOffset > 0xFFFF || staleBit > 7)
        {
            lg2::error("invalid stale offset or stale bit");
            close(fd);
            return -1;
        }

        uint8_t staleByte = 0;
        std::array<uint8_t, 2> staleCmdBuf{};
        uint16_t staleCmdLen;

        if (staleOffset > 255)
        {
            staleCmdLen = 2;
            staleCmdBuf[0] = (staleOffset >> 8) & 0xFF;
            staleCmdBuf[1] = staleOffset & 0xFF;
        }
        else
        {
            staleCmdLen = 1;
            staleCmdBuf[0] = staleOffset & 0xFF;
        }

        std::array<struct i2c_msg, 2> staleMsgs = {{
            {
                .addr = addr,
                .flags = 0,
                .len = staleCmdLen,
                .buf = staleCmdBuf.data(),
            },
            {
                .addr = addr,
                .flags = I2C_M_RD,
                .len = 1,
                .buf = (uint8_t*)&staleByte,
            },
        }};
        struct i2c_rdwr_ioctl_data staleArgs = {staleMsgs.data(), 2};

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        ret = ioctl(fd, I2C_RDWR, &staleArgs);
        if (ret < 0)
        {
            close(fd);
            return ret;
        }

        if ((staleByte >> staleBit) & 1)
        {
            close(fd);
            return -1;
        }
    }

    *reading = data;
    close(fd);
    return 0;
}

int SatelliteSensor::readRawEepromData(size_t off, uint8_t length,
                                       size_t staleOffset, size_t staleBit,
                                       double* data) const
{
    uint64_t reading = 0;
    int ret = i2cCmd<uint64_t>(busId, addr, off, &reading, length, staleOffset,
                               staleBit);
    if (ret >= 0)
    {
        if (debug)
        {
            std::cout << "offset: " << off << std::hex
                      << " reading: " << reading << "\n";
        }
        if (sensorType == "Temperature")
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            *data = reading2tempEp(reinterpret_cast<uint8_t*>(&reading));
        }
        else if (sensorType == "Power")
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            *data = reading2power(reinterpret_cast<uint8_t*>(&reading));
        }
        else if (sensorType == "Energy")
        {
            *data = reading / 1000.0; // mJ to J (double)
        }
        else
        {
            *data = reading;
        }
        return 0;
    }
    return ret;
}

int SatelliteSensor::readPLDMEepromData(size_t off, uint8_t length,
                                        size_t staleOffset, size_t staleBit,
                                        double* data) const
{
    double reading = 0;
    int ret = i2cCmd<double>(busId, addr, off, &reading, length, staleOffset,
                             staleBit);
    if (ret >= 0)
    {
        *data = reading;
        return 0;
    }
    return ret;
}

int SatelliteSensor::readShmData(double* data) const
{
    if (data == nullptr)
    {
        return -1;
    }

    vhmc_shm::SensorRecord rec{};
    vhmc_shm::SensorError err = vhmcShmReader().readSensor(offset, rec);
    if (err != vhmc_shm::SensorError::Success)
    {
        return static_cast<int>(err);
    }
    if (!shmRecordToDouble(rec, *data))
    {
        return static_cast<int>(vhmc_shm::SensorError::SensorNoData);
    }
    return 0;
}

void SatelliteSensor::restartRead()
{
    size_t pollTime = getPollRate(); // in seconds

    waitTimer.expires_after(std::chrono::seconds(pollTime));
    waitTimer.async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            return; // we're being cancelled
        }
        // read timer error
        if (ec)
        {
            lg2::error("timer error");
            return;
        }
        read();
    });
}

void SatelliteSensor::read()
{
    if (!readingStateGood())
    {
        markAvailable(false);
        updateValueOnly(std::numeric_limits<double>::quiet_NaN());
        restartRead();
        return;
    }

    double temp = 0;
    int ret = 0;

    if (valueType == "SHM")
    {
        ret = readShmData(&temp);
        if (awaitingFirstShmWriter(ret))
        {
            markAvailable(false);
            updateValueOnly(std::numeric_limits<double>::quiet_NaN());
            invalidReadCount = 0;
            restartRead();
            return;
        }
    }
    else
    {
        int len = getLength(offset);
        if (len == 0)
        {
            lg2::error("no offset is specified");
            return;
        }

        if (valueType == "Raw")
        {
            ret = readRawEepromData(offset, len, staleOffset, staleBit, &temp);
        }
        else if (valueType == "PLDM")
        {
            ret = readPLDMEepromData(offset, len, staleOffset, staleBit, &temp);
        }
        else
        {
            lg2::error("Invalid ValueType for sensor: {NAME}", "NAME", name);
            return;
        }
    }

    // Check if the sensor reading is within the valid range. In the case where
    // the sensor type is "Energy", the sensor value monotonically increases
    // over time, so the sensor reading is not bounded by a max value.
    if (ret >= 0 && ((temp >= minValue && temp <= maxValue) ||
                     (sensorType == "Energy" && temp >= minValue)))
    {
        if constexpr (debug)
        {
            lg2::error("Value update to {TEMP}", "TEMP", temp);
        }
        if (invalidReadCount > 0)
        {
            lg2::info(
                "Sensor {NAME} recovered after {COUNT} invalid reads at offset: {OFFSET}",
                "NAME", name, "COUNT", invalidReadCount, "OFFSET", offset);
        }
        updateValueOnly(temp);
        invalidReadCount = 0;
    }
    else
    {
        invalidReadCount++;
        // Log on the good->bad transition, then periodically as a reminder
        // while the sensor remains invalid. The reminder cadence is derived
        // from invalidLogReminderSec and the sensor's pollRate so it stays
        // ~constant regardless of poll rate or sensor count.
        if (invalidReadCount == 1 || invalidReadCount % invalidLogInterval == 0)
        {
            lg2::error(
                "Invalid sensor read for {NAME} at offset: {OFFSET} with value: {VALUE} (count: {COUNT}, reason: {REASON})",
                "NAME", name, "OFFSET", offset, "VALUE", temp, "COUNT",
                invalidReadCount, "REASON", invalidReadReason(ret, valueType));
        }
        incrementError();
    }
    restartRead();
}

void createSensors(
    boost::asio::io_context& io, sdbusplus::asio::object_server& objectServer,
    boost::container::flat_map<std::string, std::unique_ptr<SatelliteSensor>>&
        sensors,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection)
{
    if (!dbusConnection)
    {
        lg2::error("Connection not created");
        return;
    }

    dbusConnection->async_method_call(
        [&io, &objectServer, &dbusConnection, &sensors](
            boost::system::error_code ec, const ManagedObjectType& resp) {
            if (ec)
            {
                lg2::error("Error contacting entity manager");
                return;
            }
            for (const auto& pathPair : resp)
            {
                for (const auto& entry : pathPair.second)
                {
                    if (entry.first != configInterface)
                    {
                        continue;
                    }
                    std::string name =
                        loadVariant<std::string>(entry.second, "Name");

                    std::vector<thresholds::Threshold> sensorThresholds;
                    if (!parseThresholdsFromConfig(pathPair.second,
                                                   sensorThresholds))
                    {
                        lg2::error("error populating thresholds for {NAME}",
                                   "NAME", name);
                    }

                    uint8_t busId = 0;
                    uint8_t addr = 0;
                    uint16_t off;
                    std::string sensorType;
                    std::string valueType;
                    size_t rate;
                    std::string powerSate;
                    PowerState pwrState;
                    double minVal;
                    double maxVal;
                    uint16_t staleOffset = 0;
                    size_t staleBit = 0;
                    try
                    {
                        valueType =
                            loadVariant<std::string>(entry.second, "ValueType");
                        off =
                            loadVariant<uint16_t>(entry.second, "OffsetValue");
                        sensorType = loadVariant<std::string>(entry.second,
                                                              "SensorType");
                        rate = loadVariant<uint8_t>(entry.second, "PollRate");
                        powerSate = loadVariant<std::string>(entry.second,
                                                             "PowerState");
                        setReadState(powerSate, pwrState);
                        minVal = loadVariant<double>(entry.second, "MinValue");
                        maxVal = loadVariant<double>(entry.second, "MaxValue");
                        if (valueType != "SHM")
                        {
                            busId = loadVariant<uint8_t>(entry.second, "Bus");
                            addr =
                                loadVariant<uint8_t>(entry.second, "Address");
                            try
                            {
                                staleOffset = loadVariant<uint16_t>(
                                    entry.second, "StaleOffset");
                                staleBit = loadVariant<size_t>(entry.second,
                                                               "StaleBit");
                            }
                            catch (const std::exception& e)
                            {
                                lg2::info(
                                    "No stale bit or offset provided for {NAME}. Ignoring stalness for this sensor.",
                                    "NAME", name);
                                staleOffset = 0;
                                staleBit = 0;
                            }
                        }
                        if constexpr (debug)
                        {
                            lg2::info(
                                "Configuration parsed for \n\t {CONF}\nwith\n"
                                "\tName: {NAME}\n"
                                "\tBus: {BUS}\n"
                                "\tAddress:{ADDR}\n"
                                "\tPowerState:{PWRSTATE}\n"
                                "\tOffset: {OFF}\n"
                                "\tStaleOffset: {STALEOFFSET}\n"
                                "\tStaleBit: {STALEBIT}\n"
                                "\tType : {TYPE}\n"
                                "\tValue Type : {VALUETYPE}\n"
                                "\tPollrate: {RATE}\n"
                                "\tMinValue: {MIN}\n"
                                "\tMaxValue: {MAX}\n",
                                "CONF", entry.first, "NAME", name, "BUS",
                                static_cast<int>(busId), "ADDR",
                                static_cast<int>(addr), "PWRSTATE", powerSate,
                                "OFF", static_cast<int>(off), "STALEOFFSET",
                                static_cast<int>(staleOffset), "STALEBIT",
                                static_cast<int>(staleBit), "TYPE", sensorType,
                                "VALUETYPE", valueType, "RATE", rate, "MIN",
                                minVal, "MAX", maxVal);
                        }
                    }
                    catch (const std::exception& e)
                    {
                        lg2::error(
                            "Error parsing configuration for {NAME}. There is likely a missing or invalid value. {ERROR}",
                            "NAME", name, "ERROR", e.what());
                        continue;
                    }

                    auto& sensor = sensors[name];
                    sensor = nullptr;

                    sensor = std::make_unique<SatelliteSensor>(
                        dbusConnection, io, name, pathPair.first, objectType,
                        objectServer, std::move(sensorThresholds), busId, addr,
                        off, staleOffset, staleBit, sensorType, valueType, rate,
                        minVal, maxVal, pwrState);

                    sensor->init();
                }
            }
        },
        entityManagerName, "/xyz/openbmc_project/inventory",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
}

static void powerStateChanged(PowerState type, bool newState)
{
    if (type != PowerState::on)
    {
        return;
    }
    for (auto& [name, sensor] : sensors)
    {
        if (sensor != nullptr && sensor->readState == type)
        {
            if (newState)
            {
                // power on
                sensor->init();
            }
            else
            {
                // power off
                sensor->deactivate();
            }
        }
    }
}

int main()
{
    boost::asio::io_context io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::asio::object_server objectServer(systemBus, true);
    objectServer.add_manager("/xyz/openbmc_project/sensors");
    systemBus->request_name("xyz.openbmc_project.Satellite");

    boost::asio::post(io, [&]() {
        createSensors(io, objectServer, sensors, systemBus);
    });

    boost::asio::steady_timer configTimer(io);

    auto powerCallBack = [](PowerState type, bool state) {
        powerStateChanged(type, state);
    };
    setupPowerMatchCallback(systemBus, powerCallBack);

    std::function<void(sdbusplus::message::message&)> eventHandler =
        [&](sdbusplus::message::message&) {
            configTimer.expires_after(std::chrono::seconds(1));
            // create a timer because normally multiple properties change
            configTimer.async_wait([&](const boost::system::error_code& ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    return; // we're being canceled
                }
                // config timer error
                if (ec)
                {
                    lg2::error("timer error");
                    return;
                }
                createSensors(io, objectServer, sensors, systemBus);
                if (sensors.empty())
                {
                    lg2::info("Configuration not detected");
                }
            });
        };

    sdbusplus::bus::match::match configMatch(
        static_cast<sdbusplus::bus::bus&>(*systemBus),
        "type='signal',member='PropertiesChanged',"
        "path_namespace='" +
            std::string(inventoryPath) +
            "',"
            "arg0namespace='" +
            configInterface + "'",
        eventHandler);

    setupManufacturingModeMatch(*systemBus);
    io.run();
    return 0;
}
