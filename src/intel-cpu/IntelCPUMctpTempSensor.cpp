/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "IntelCPUMctpTempSensor.hpp"

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

static constexpr double maxTempC = 127.0;
static constexpr double minTempC = -128.0;

IntelCPUMctpTempSensor::IntelCPUMctpTempSensor(
    sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::string& sensorName, const std::string& sensorConfiguration,
    std::vector<thresholds::Threshold>&& thresholdData) :
    Sensor(escapeName(sensorName), std::move(thresholdData),
           sensorConfiguration, "XeonCPU", false, false, maxTempC, minTempC,
           conn, PowerState::on),
    objServer(objectServer)
{
    std::string path = "/xyz/openbmc_project/sensors/temperature/" + name;
    sensorInterface =
        objServer.add_interface(path, "xyz.openbmc_project.Sensor.Value");
    for (const auto& threshold : thresholds)
    {
        std::string interface = thresholds::getInterface(threshold.level);
        thresholdInterfaces[static_cast<size_t>(threshold.level)] =
            objServer.add_interface(path, interface);
    }
    association = objServer.add_interface(path, association::interface);
    setInitialProperties(sensor_paths::unitDegreesC);
}

IntelCPUMctpTempSensor::~IntelCPUMctpTempSensor()
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

void IntelCPUMctpTempSensor::updateReading(double value)
{
    updateValue(value);
}

void IntelCPUMctpTempSensor::checkThresholds()
{
    thresholds::checkThresholds(this);
}
