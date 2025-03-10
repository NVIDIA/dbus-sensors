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

#include "Utils.hpp"
#include "VariantVisitors.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/container/flat_map.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

extern "C"
{
#include <i2c/smbus.h>
#include <linux/i2c-dev.h>
}

constexpr const bool debug = false;

constexpr const char* configInterface =
    "xyz.openbmc_project.Configuration.Satellite";
constexpr const char* sensorRootPath = "/xyz/openbmc_project/sensors/";
constexpr const char* objectType = "Satellite";

boost::container::flat_map<std::string, std::unique_ptr<SatelliteSensor>>
    sensors;

SatelliteSensor::SatelliteSensor(
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    boost::asio::io_context& io, const std::string& sensorName,
    const std::string& sensorConfiguration, const std::string& objType,
    sdbusplus::asio::object_server& objectServer,
    std::vector<thresholds::Threshold>&& thresholdData, uint8_t busId,
    uint8_t addr, uint16_t offset, std::string& sensorType,
    std::string& valueType, size_t pollTime, double minVal, double maxVal,
    const PowerState powerState) :
    Sensor(escapeName(sensorName), std::move(thresholdData),
           sensorConfiguration, objType, false, false, maxVal, minVal, conn,
           powerState),
    name(escapeName(sensorName)), busId(busId), addr(addr), offset(offset),
    sensorType(sensorType), valueType(valueType), objectServer(objectServer),
    waitTimer(io), pollRate(pollTime)
{
    // make the string to lowercase for Dbus sensor type
    for (auto& c : sensorType)
    {
        c = tolower(c);
    }
    std::string sensorPath = sensorRootPath + sensorType + "/";

    sensorInterface = objectServer.add_interface(sensorPath + name,
                                                 sensorValueInterface);

    for (const auto& threshold : thresholds)
    {
        std::string interface = thresholds::getInterface(threshold.level);
        thresholdInterfaces[static_cast<size_t>(threshold.level)] =
            objectServer.add_interface(sensorPath + name, interface);
    }
    association = objectServer.add_interface(sensorPath + name,
                                             association::interface);

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
int i2cCmd(uint8_t bus, uint8_t addr, size_t offset, T* reading, uint8_t length)
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

    struct i2c_msg msgs[2] = {{
                                  // write offset
                                  .addr = addr,
                                  .flags = 0,
                                  .len = 2,
                                  .buf = cmd.data(),
                              },
                              {// read data from the offset
                               .addr = addr,
                               .flags = I2C_M_RD,
                               .len = length,
                               .buf = (uint8_t*)&data}};

    struct i2c_rdwr_ioctl_data args = {msgs, 2};

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
        *reading = 0;
    }
    else
    {
        *reading = data;
    }
    close(fd);
    return 0;
}

int SatelliteSensor::readRawEepromData(size_t off, uint8_t length,
                                       double* data) const
{
    uint64_t reading = 0;
    int ret = i2cCmd<uint64_t>(busId, addr, off, &reading, length);
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
                                        double* data) const
{
    double reading = 0;
    int ret = i2cCmd<double>(busId, addr, off, &reading, length);
    if (ret >= 0)
    {
        *data = reading;
        return 0;
    }
    return ret;
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
    int len = getLength(offset);
    if (len == 0)
    {
        lg2::error("no offset is specified");
        return;
    }

    int ret = 0;
    // Sensor reading value types are sensor-specific. So, read
    // and interpret sensor data based on it's value type.
    if (valueType == "Raw")
    {
        ret = readRawEepromData(offset, len, &temp);
    }
    else if (valueType == "PLDM")
    {
        ret = readPLDMEepromData(offset, len, &temp);
    }
    else
    {
        lg2::error("Invalid ValueType for sensor: {NAME}", "NAME", name);
        return;
    }

    if (ret >= 0)
    {
        if constexpr (debug)
        {
            lg2::error("Value update to {TEMP}", "TEMP", temp);
        }
        updateValueOnly(temp);
    }
    else
    {
        lg2::error("Invalid read getRegsInfo");
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
                std::string name = loadVariant<std::string>(entry.second,
                                                            "Name");

                std::vector<thresholds::Threshold> sensorThresholds;
                if (!parseThresholdsFromConfig(pathPair.second,
                                               sensorThresholds))
                {
                    lg2::error("error populating thresholds for {NAME}", "NAME",
                               name);
                }

                uint8_t busId = loadVariant<uint8_t>(entry.second, "Bus");

                uint8_t addr = loadVariant<uint8_t>(entry.second, "Address");

                uint16_t off = loadVariant<uint16_t>(entry.second,
                                                     "OffsetValue");

                std::string sensorType = loadVariant<std::string>(entry.second,
                                                                  "SensorType");

                std::string valueType = loadVariant<std::string>(entry.second,
                                                                 "ValueType");

                size_t rate = loadVariant<uint8_t>(entry.second, "PollRate");

                std::string powerSate = loadVariant<std::string>(entry.second,
                                                                 "PowerState");

                PowerState pwrState;
                setReadState(powerSate, pwrState);

                double minVal = loadVariant<double>(entry.second, "MinValue");

                double maxVal = loadVariant<double>(entry.second, "MaxValue");
                if constexpr (debug)
                {
                    lg2::info("Configuration parsed for \n\t {CONF}\nwith\n"
                              "\tName: {NAME}\n"
                              "\tBus: {BUS}\n"
                              "\tAddress:{ADDR}\n"
                              "\tPowerState:{PWRSTATE}\n"
                              "\tOffset: {OFF}\n"
                              "\tType : {TYPE}\n"
                              "\tValue Type : {VALUETYPE}\n"
                              "\tPollrate: {RATE}\n"
                              "\tMinValue: {MIN}\n"
                              "\tMaxValue: {MAX}\n",
                              "CONF", entry.first, "NAME", name, "BUS",
                              static_cast<int>(busId), "ADDR",
                              static_cast<int>(addr), "PWRSTATE", powerSate,
                              "OFF", static_cast<int>(off), "TYPE", sensorType,
                              "VALUETYPE", valueType, "RATE", rate, "MIN",
                              static_cast<double>(minVal), "MAX",
                              static_cast<double>(maxVal));
                }

                auto& sensor = sensors[name];
                sensor = nullptr;

                sensor = std::make_unique<SatelliteSensor>(
                    dbusConnection, io, name, pathPair.first, objectType,
                    objectServer, std::move(sensorThresholds), busId, addr, off,
                    sensorType, valueType, rate, minVal, maxVal, pwrState);

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

    boost::asio::post(
        io, [&]() { createSensors(io, objectServer, sensors, systemBus); });

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
