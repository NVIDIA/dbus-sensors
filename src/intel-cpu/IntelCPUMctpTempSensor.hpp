/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "Thresholds.hpp"

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sensor.hpp>

#include <memory>
#include <string>
#include <vector>

// A CPU/DIMM temperature sensor read over PECI-over-MCTP. The owning reader
// pushes readings via updateReading(); thresholds/availability come from
// Sensor.
class IntelCPUMctpTempSensor :
    public Sensor,
    public std::enable_shared_from_this<IntelCPUMctpTempSensor>
{
  public:
    IntelCPUMctpTempSensor(sdbusplus::asio::object_server& objectServer,
                           std::shared_ptr<sdbusplus::asio::connection>& conn,
                           const std::string& sensorName,
                           const std::string& sensorConfiguration,
                           std::vector<thresholds::Threshold>&& thresholdData);
    ~IntelCPUMctpTempSensor() override;

    // Publish a reading; pass quiet_NaN() to mark unavailable.
    void updateReading(double value);

  private:
    void checkThresholds() override;

    sdbusplus::asio::object_server& objServer;
};
