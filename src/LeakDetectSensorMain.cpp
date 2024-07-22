/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

#include "LeakDetectSensor.hpp"
#include "DeviceMgmt.hpp"
#include "Utils.hpp"

#include <boost/container/flat_set.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <filesystem>
#include <string>
#include <vector>

static constexpr float pollRateDefault = 0.25;

static const I2CDeviceTypeMap i2CDeviceTypes{
    {"MAX1363", I2CDeviceType{"max1363", false}}
};

static std::shared_ptr<I2CDeviceParams> getI2CParams(
    const SensorBaseConfigMap& cfg)
{
    auto findDeviceType = cfg.find("DeviceType");
    auto findBus = cfg.find("Bus");
    auto findAddress = cfg.find("Address");
    if (findDeviceType == cfg.end() || findBus == cfg.end() ||
        findAddress == cfg.end())
    {
        std::cerr << "Missing device info, cannot instantiate I2CDevice\n";
        return nullptr;
    }

    std::string deviceType =
        std::visit(VariantToStringVisitor(), findDeviceType->second);
    auto findI2CDevType = i2CDeviceTypes.find(deviceType);
    if (findI2CDevType == i2CDeviceTypes.end())
    {
        std::cerr << "Missing device info, cannot instantiate I2CDevice\n";
        return nullptr;
    }

    std::uint64_t bus =
        std::visit(VariantToUnsignedIntVisitor(), findBus->second);
    std::uint64_t address =
        std::visit(VariantToUnsignedIntVisitor(), findAddress->second);

    return std::make_shared<I2CDeviceParams>(findI2CDevType->second, bus,
        address);
}

static std::string getReadPath(const SensorBaseConfigMap& cfg,
    const I2CDeviceParams& params)
{
    // There may be multiple sensors tied to each ADC, hence the configuration
    // needs to define which channel the sensor is populated on
    auto findChannel = cfg.find("Channel");
    if (findChannel == cfg.end())
    {
        std::cerr << "Missing channel information, cannot get Read Path\n";
        return "";
    }

    std::uint64_t channel =
        std::visit(VariantToUnsignedIntVisitor(), findChannel->second);
    std::string sensorFile = "in_voltage" + std::to_string(channel) + "_raw";

    std::string devicePath = params.devicePath();

    // Find the expected readPath by searching in the device path based on
    // device info in params.  We expect to only find one valid path since
    // the bus, address and channel are provided.
    std::vector<std::filesystem::path> readPaths;
    findFiles(std::filesystem::path(devicePath), sensorFile, readPaths);
    if (readPaths.size() != 1)
    {
        std::cerr << "Unexpected number (" << readPaths.size()
            << ") of readPaths found, can not determine correct read path.\n";
        return "";
    }

    std::string readPath = readPaths.front();
    std::cout << "Got sensor readPath " << readPath << "\n";

    return readPath;
}

static std::shared_ptr<I2CDevice> getI2CDevice(
    const I2CDeviceParams& params)
{
    // Keeps an ongoing list of instantiated I2C devices
    static std::vector<std::pair<std::string, std::weak_ptr<I2CDevice>>>
        devices;
    std::string devicePath = params.devicePath();

    std::shared_ptr<I2CDevice> i2cDevice;

    auto it = devices.begin();

    while (it != devices.end())
    {
        std::shared_ptr<I2CDevice> deviceShared = it->second.lock();
        if (deviceShared == nullptr)
        {
            // Clean up any devices that are no longer used by a sensor
            it = devices.erase(it);
            continue;
        }

        if (it->first == devicePath)
        {
            std::cout << "I2C device " << devicePath << " already exists.\n";
            i2cDevice = deviceShared;
        }

        it++;
    }

    if (i2cDevice != nullptr)
    {
        return i2cDevice;
    }

    std::cout << "Instantiating new I2C device " << devicePath << "\n";

    try
    {
        i2cDevice = std::make_shared<I2CDevice>(params);
    }
    catch (std::runtime_error&)
    {
        std::cerr << "Failed to instantiate " << params.type->name
                    << " at address " << params.address << " on bus "
                    << params.bus << "\n";
        return nullptr;
    }

    // Add the new I2C device to the list of instantiated devices
    devices.emplace_back(devicePath, i2cDevice);

    return i2cDevice;
}

static void handleSensorConfigurations(
    boost::asio::io_context& io, sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const std::shared_ptr<boost::container::flat_set<std::string>>&
        sensorsChanged,
    boost::container::flat_map<std::string, std::shared_ptr<LeakDetectSensor>>&
        sensors,
    const ManagedObjectType& sensorConfigurations)
{
    bool firstScan = (sensorsChanged == nullptr);

    // Iterate over each configuration found
    for (const auto& [path, configData] : sensorConfigurations)
    {
        // Find the base configuration
        auto sensorBase = configData.find(
                configInterfaceName(LeakDetectSensor::entityMgrConfigType));
        if (sensorBase == configData.end())
        {
            continue;
        }
        const std::pair<std::string, SensorBaseConfigMap>* 
            baseConfiguration = &(*sensorBase);

        const std::string* interfacePath = &path.str;
        std::cout << "Found interfacePath " << *interfacePath << "\n";

        auto findSensorName = baseConfiguration->second.find("Name");
        if (findSensorName == baseConfiguration->second.end())
        {
            std::cerr << "Could not determine configuration name for "
                    << *interfacePath << "\n";
            continue;
        }
        std::string sensorName =
            std::visit(VariantToStringVisitor(), findSensorName->second);
        std::cout << "Found sensor configuration with name "
            << sensorName << "\n";

        // On rescans, only update sensors we were signaled by
        auto findSensor = sensors.find(sensorName);
        if (!firstScan && findSensor != sensors.end())
        {
            bool found = false;
            for (auto it = sensorsChanged->begin();
                    it != sensorsChanged->end(); it++)
            {
                if (findSensor->second &&
                    it->ends_with(findSensor->second->name))
                {
                    sensorsChanged->erase(it);
                    findSensor->second = nullptr;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                continue;
            }
        }

        std::shared_ptr<I2CDeviceParams> params =
            getI2CParams(baseConfiguration->second);
        if (params == nullptr)
        {
            // Malformed or missing I2C device info, cannot proceed
            continue;
        }

        // Create an I2C device based on the params parsed above, such as
        // bus and address.  This will also instantiate the device with
        // available driver based on the device type
        std::shared_ptr<I2CDevice> i2cDev = getI2CDevice(*params);
        if (i2cDev == nullptr)
        {
            std::cout << "Unable to create I2C device for " << sensorName
                << "\n";
            continue;
        }

        // The read path is the sysfs file path that the sensor value can be
        // retrieved from
        std::string readPath = getReadPath(baseConfiguration->second, *params);
        if (readPath.empty())
        {
            std::cout << "Unable to get Read Path for " << sensorName << "\n";
            continue;
        }

        float pollRate =
            getPollRate(baseConfiguration->second, pollRateDefault);

        std::vector<thresholds::Threshold> sensorThresholds;
        if (!parseThresholdsFromConfig(configData, sensorThresholds))
        {
            std::cerr << "Error populating thresholds for " << sensorName
                << "\n";
        }

        // Create a new sensor object based on configurations deteremined above
        auto& sensor = sensors[sensorName];
        sensor = std::make_shared<LeakDetectSensor>(
            readPath, objectServer, dbusConnection, io, sensorName,
            std::move(sensorThresholds), i2cDev, pollRate, PowerState::always,
            *interfacePath);

        sensor->setupRead();
    }
}

void createSensors(
    boost::asio::io_context& io,
    sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const std::shared_ptr<boost::container::flat_set<std::string>>&
        sensorsChanged,
    boost::container::flat_map<std::string, std::shared_ptr<LeakDetectSensor>>&
        sensors)
{
    auto getter = std::make_shared<GetSensorConfiguration>(dbusConnection,
        [&io, &objectServer, &dbusConnection, &sensorsChanged, &sensors]
        (const ManagedObjectType& sensorConfigurations){
            handleSensorConfigurations(io, objectServer, dbusConnection,
                sensorsChanged, sensors, sensorConfigurations);
        });

    getter->getConfiguration(
            std::vector<std::string>{LeakDetectSensor::entityMgrConfigType});
}

int main()
{
    // Create IO Execution Context for ASIO
    boost::asio::io_context io;

    // Setup connection to the systemBus Dbus
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);

    // Setup object Server
    sdbusplus::asio::object_server objectServer(systemBus, true);
    systemBus->request_name("xyz.openbmc_project.LeakDetectSensor");
    objectServer.add_manager("/xyz/openbmc_project/sensors");

    // Creates flatmaps of all the LeakDetectSensors detected
    boost::container::flat_map<std::string, std::shared_ptr<LeakDetectSensor>>
        sensors;

    auto sensorsChanged =
        std::make_shared<boost::container::flat_set<std::string>>();

    boost::asio::post(io, [&]() {
        createSensors(io, objectServer, systemBus, nullptr, sensors);
    });

    // Setup callback to handle changes in Entity Manager configs at runtime.
    // This is required as the Entity Manager may not immediately enumerate all
    // corresponding configurations on boot
    boost::asio::steady_timer filterTimer(io);
    std::function<void(sdbusplus::message_t&)> eventHandler =
        [&io, &objectServer, &systemBus, &sensorsChanged, &sensors,
         &filterTimer](sdbusplus::message_t& message) {
        if (message.is_method_error())
        {
            std::cerr << "callback method error\n";
            return;
        }
        sensorsChanged->insert(message.get_path());

        std::cout << "LeakDetectSensor change event received: "
            << message.get_path() << "\n";

        // this implicitly cancels the timer
        filterTimer.expires_after(std::chrono::seconds(1));

        filterTimer.async_wait(
            [&io, &objectServer, &systemBus, &sensorsChanged, &sensors]
             (const boost::system::error_code& ec) {
            if (ec != boost::system::errc::success)
            {
                if (ec != boost::asio::error::operation_aborted)
                {
                    std::cerr << "Time callback error: " << ec.message()
                        << "\n";
                }
                return;
            }

            createSensors(io, objectServer, systemBus, sensorsChanged, sensors);
        });
    };

    std::vector<std::unique_ptr<sdbusplus::bus::match_t>> matches =
        setupPropertiesChangedMatches(*systemBus,
            std::to_array<const char*>({LeakDetectSensor::entityMgrConfigType}),
            eventHandler);

    io.run();
}
