/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "IntelCPUMctpPowerSensor.hpp"

#include "SensorPaths.hpp"
#include "Thresholds.hpp"
#include "Utils.hpp"
#include "sensor.hpp"

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

static constexpr double maxPowerW = 1000.0;
static constexpr double minPowerW = 0.0;

IntelCPUMctpPowerSensor::IntelCPUMctpPowerSensor(
    sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::string& sensorName, const std::string& sensorConfiguration,
    std::vector<thresholds::Threshold>&& thresholdData) :
    Sensor(escapeName(sensorName), std::move(thresholdData),
           sensorConfiguration, "XeonCPU", false, false, maxPowerW, minPowerW,
           conn, PowerState::on),
    objServer(objectServer)
{
    std::string path = "/xyz/openbmc_project/sensors/power/" + name;
    sensorInterface =
        objServer.add_interface(path, "xyz.openbmc_project.Sensor.Value");
    for (const auto& threshold : thresholds)
    {
        std::string interface = thresholds::getInterface(threshold.level);
        thresholdInterfaces[static_cast<size_t>(threshold.level)] =
            objServer.add_interface(path, interface);
    }
    association = objServer.add_interface(path, association::interface);
    setInitialProperties(sensor_paths::unitWatts);
}

IntelCPUMctpPowerSensor::~IntelCPUMctpPowerSensor()
{
    for (const auto& iface : thresholdInterfaces)
    {
        objServer.remove_interface(iface);
    }
    objServer.remove_interface(sensorInterface);
    objServer.remove_interface(association);
    objServer.remove_interface(availableInterface);
    objServer.remove_interface(operationalInterface);
}

void IntelCPUMctpPowerSensor::updateReading(double value)
{
    updateValue(value);
}

void IntelCPUMctpPowerSensor::checkThresholds()
{
    thresholds::checkThresholds(this);
}
