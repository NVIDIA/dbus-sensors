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

#include "LeakDetectSensor.hpp"

#include <unistd.h>

#include <boost/asio/read_until.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Enable debug logging
static constexpr bool debug = false;

// Scale factor calculated based on Vref of 3.3V for 12-bit ADCs:
// 3.3V / 4096 = 0.000806
static constexpr double sensorScaleFactor = 0.000806;

// Round value to 3 decimal places
static constexpr double roundFactor = 10000;

LeakDetectSensor::LeakDetectSensor(
    const std::string& readPath, sdbusplus::asio::object_server& objectServer,
    boost::asio::io_context& io,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::string& sensorName, const std::shared_ptr<I2CDevice>& i2cDevice,
    const float pollRate, const double configLeakThreshold,
    const double sensorMax, const double sensorMin,
    const std::string& configurationPath, bool shutdownOnLeak,
    const unsigned int shutdownDelaySeconds) :
    i2cDevice(i2cDevice),
    objServer(objectServer), dbusConnection(conn),
    inputDev(io, readPath, boost::asio::random_access_file::read_only),
    waitTimer(io), shutdownTimer(io), name(sensorName), readPath(readPath),
    sensorPollMs(static_cast<unsigned int>(pollRate * 1000)),
    leakThreshold(configLeakThreshold), sensorMax(sensorMax),
    sensorMin(sensorMin), detectorState(DetectorState::NORMAL),
    sensorOverride(false), internalValueSet(false),
    configurationPath(configurationPath), shutdownOnLeak(shutdownOnLeak),
    shutdownDelaySeconds(shutdownDelaySeconds)
{
    sdbusplus::message::object_path sensorObjPath(
        "/xyz/openbmc_project/sensors/voltage/");
    sensorObjPath /= name;

    sensorInterface = objectServer.add_interface(
        sensorObjPath, "xyz.openbmc_project.Sensor.Value");

    // Defines a custom SET property method to handle overrides. Any external
    // calls to set the Value property will trigger the override mode. Real ADC
    // values read by the daemon will be ignored once override mode is active.
    sensorInterface->register_property(
        "Value", detectorValue,
        [this](const double& newValue, double& oldValue) {
        if (!internalValueSet)
        {
            detectorValue = newValue;
            sensorOverride = true;
        }
        else if (!sensorOverride)
        {
            detectorValue = newValue;
        }

        determineDetectorState(detectorValue);
        oldValue = detectorValue;
        return true;
    });
    sensorInterface->register_property("Unit", sensor_paths::unitVolts);
    sensorInterface->register_property("MinValue", sensorMin);
    sensorInterface->register_property("MaxValue", sensorMax);

    if (!sensorInterface->initialize())
    {
        std::cerr << "Error initializing sensor value interface for " << name
                  << "\n";
    }

    thresholdInterface = objectServer.add_interface(
        sensorObjPath, "xyz.openbmc_project.Sensor.Threshold.Critical");

    // Defines a custom SET property method to handle threshold adjustments. In
    // addition to updating the threshold tracked by this object, it will also
    // be persisted so that it will survive resets.
    thresholdInterface->register_property(
        "CriticalLow", leakThreshold,
        [this](const double& newValue, double& oldValue) {
        leakThreshold = newValue;
        persistThreshold(leakThreshold);
        oldValue = leakThreshold;
        return true;
    });

    if (!thresholdInterface->initialize())
    {
        std::cerr << "Error initializing sensor threshold interface for "
                  << name << "\n";
    }

    sensorAssociation = objectServer.add_interface(sensorObjPath,
                                                   association::interface);
    createAssociation(sensorAssociation, configurationPath);

    sdbusplus::message::object_path inventoryObjPath(
        "/xyz/openbmc_project/inventory/leakdetectors/");
    inventoryObjPath /= name;

    // Expose inventory related leak detector interfaces and properties
    inventoryInterface = objectServer.add_interface(
        inventoryObjPath, "xyz.openbmc_project.Inventory.Item.LeakDetector");
    inventoryInterface->register_property("LeakDetectorType",
                                          std::string("Moisture"));
    if (!inventoryInterface->initialize())
    {
        std::cerr << "Error initializing leakage inventory interface for "
                  << name << "\n";
        return;
    }

    // Add association of the inventory object to the chassis.  This is required
    // for other applications such as bmcweb to determine which chassis this
    // particular Leak Detector belongs to.
    inventoryAssociation = objectServer.add_interface(inventoryObjPath,
                                                      association::interface);
    std::vector<Association> inventoryAssociations;
    inventoryAssociations.emplace_back(
        "chassis", "contained_by",
        sdbusplus::message::object_path(configurationPath).parent_path());
    inventoryAssociation->register_property("Associations",
                                            inventoryAssociations);
    if (!inventoryAssociation->initialize())
    {
        std::cerr << "Error initializing association interface for " << name
                  << "\n";
        return;
    }

    sdbusplus::message::object_path stateObjPath(
        "/xyz/openbmc_project/state/leakdetectors/");
    stateObjPath /= name;

    // Expose leak detector state interfaces and properties
    leakStateInterface = objectServer.add_interface(
        stateObjPath, "xyz.openbmc_project.State.LeakDetector");
    leakStateInterface->register_property(
        "DetectorState", getDetectorStatusString(detectorState));
    if (!leakStateInterface->initialize())
    {
        std::cerr << "Error initializing detector state interface for " << name
                  << "\n";
        return;
    }

    // Expose detector operational state interface and properties
    opStateInterface = objectServer.add_interface(
        stateObjPath, "xyz.openbmc_project.State.Decorator.OperationalStatus");
    opStateInterface->register_property("State",
                                        getDetectorStateString(detectorState));
    if (!opStateInterface->initialize())
    {
        std::cerr << "Error initializing operational state interface for "
                  << name << "\n";
        return;
    }

    // Add association of the state object to the invetory object that describes
    // the leak detector.  Other application such as bmcweb may use this to
    // determine which leak detector the state is describing.
    stateAssociation = objectServer.add_interface(stateObjPath,
                                                  association::interface);
    std::vector<Association> stateAssociations;
    stateAssociations.emplace_back("inventory", "leak_detecting",
                                   inventoryObjPath);
    stateAssociation->register_property("Associations", stateAssociations);
    if (!stateAssociation->initialize())
    {
        std::cerr << "Error initializing association interface for " << name
                  << "\n";
        return;
    }
}

LeakDetectSensor::~LeakDetectSensor()
{
    inputDev.close();
    waitTimer.cancel();
    shutdownTimer.cancel();

    objServer.remove_interface(sensorInterface);
    objServer.remove_interface(thresholdInterface);
    objServer.remove_interface(sensorAssociation);
    objServer.remove_interface(inventoryInterface);
    objServer.remove_interface(inventoryAssociation);
    objServer.remove_interface(leakStateInterface);
    objServer.remove_interface(opStateInterface);
    objServer.remove_interface(stateAssociation);
}

std::string LeakDetectSensor::getSensorName()
{
    return name;
}

// Kicks off a read operation for the underlying value of the sensor based on
// the sysfs location stored in readPath.  The read location was set up while
// initializing inputDev member variable during sensor object construction.
void LeakDetectSensor::setupRead()
{
    std::weak_ptr<LeakDetectSensor> weakRef = weak_from_this();
    inputDev.async_read_some_at(
        0, boost::asio::buffer(readBuf),
        [weakRef](const boost::system::error_code& ec, std::size_t bytesRead) {
        std::shared_ptr<LeakDetectSensor> self = weakRef.lock();
        if (self)
        {
            self->handleResponse(ec, bytesRead);
        }
    });
}

// Based on the detector value, determine the current detector state
void LeakDetectSensor::determineDetectorState(double detectorValue)
{
    switch (detectorState)
    {
        case DetectorState::NORMAL:
            if ((detectorValue > sensorMax) || (detectorValue < sensorMin))
            {
                setDetectorState(DetectorState::FAULT);
            }
            else if (detectorValue < leakThreshold)
            {
                setDetectorState(DetectorState::LEAKAGE);
            }
            break;
        case DetectorState::FAULT:
            if ((detectorValue < sensorMax) && (detectorValue > sensorMin))
            {
                setDetectorState(DetectorState::NORMAL);
            }
            break;
        // Once the detector is in "leakage" state, it will remain in this state
        // as this is a critical fault that will require external intervention
        // to resolve.
        case DetectorState::LEAKAGE:
            break;
        default:
            throw std::runtime_error("Invalid detector state.");
    }
}

// Restarts a read operation after waiting for a fixed polling period.
void LeakDetectSensor::restartRead()
{
    std::weak_ptr<LeakDetectSensor> weakRef = weak_from_this();
    waitTimer.expires_after(std::chrono::milliseconds(sensorPollMs));
    waitTimer.async_wait([weakRef](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            std::cerr << "LeakDetectSensor read cancelled!\n";
            return; // we're being canceled
        }
        std::shared_ptr<LeakDetectSensor> self = weakRef.lock();
        if (!self)
        {
            std::cerr << "LeakDetectSensor weakRef no self!\n";
            return;
        }
        self->setupRead();
    });
}

// Handles the output of a read operation. If the read yielded no errors, will
// translate the raw value and update the value stored on this sensor object.
void LeakDetectSensor::handleResponse(const boost::system::error_code& err,
                                      size_t bytesRead)
{
    if ((err == boost::system::errc::bad_file_descriptor) ||
        (err == boost::asio::error::misc_errors::not_found))
    {
        std::cerr << "LeakDetectSensor " << name << " is getting destroyed\n";
        return;
    }

    if (!err)
    {
        double rawValue = 0.0;
        const char* bufEnd = readBuf.data() + bytesRead;
        std::from_chars_result ret = std::from_chars(readBuf.data(), bufEnd,
                                                     rawValue);
        if (ret.ec != std::errc())
        {
            std::cerr << "Unable to get value.\n";
        }
        else
        {
            double newValue = rawValue * sensorScaleFactor;
            newValue = std::round(newValue * roundFactor) / roundFactor;
            if constexpr (debug)
            {
                std::cout << name << " detector value: " << newValue << "\n";
            }

            if (sensorInterface && (detectorValue != newValue))
            {
                // Set the flag to indicate this set property was called
                // internally.  If sensorOverride is active, this new value
                // will be ignored.
                internalValueSet = true;
                sensorInterface->set_property("Value", newValue);
                internalValueSet = false;
            }
        }
    }
    else
    {
        std::cerr << "Error in response.\n";
    }

    restartRead();
}

// Updates the sensor object's current detector state and take all appropriate
// actions related to a state transition.
void LeakDetectSensor::setDetectorState(DetectorState newDetectorState)
{
    // Only take action if the detector state has changed
    if (detectorState != newDetectorState)
    {
        // Update the internally tracked state
        detectorState = newDetectorState;

        // Update the dbus properties for Health Status and Resource State
        leakStateInterface->set_property(
            "DetectorState", getDetectorStatusString(detectorState));
        opStateInterface->set_property("State",
                                       getDetectorStateString(detectorState));

        // Take the appropriate actions for the new state
        switch (detectorState)
        {
            case DetectorState::LEAKAGE:
                logCriticalEvent();
                blinkFaultLed();
                if (shutdownOnLeak)
                {
                    startShutdown();
                }
                break;
            case DetectorState::FAULT:
                logFaultEvent();
                break;
            case DetectorState::NORMAL:
                break;
            default:
                throw std::runtime_error("Invalid detector state.");
        }
    }
}

// Log an event indicating a leakage.  This is separate from the logs that may
// be added when thresholds of the sensor is crossed.
void LeakDetectSensor::logCriticalEvent()
{
    if constexpr (debug)
    {
        std::cout << "Logging event for sensor: " << name << "\n";
    }

    std::string messageId = "ResourceEvent.1.0.ResourceStatusChangedCritical";
    std::string resolution =
        "Power down server immediately and inspect for water leakage.";
    std::string severity = "xyz.openbmc_project.Logging.Entry.Level.Error";
    std::string status = getDetectorStatusString(DetectorState::LEAKAGE);

    std::map<std::string, std::string> addData = {};
    addData["REDFISH_MESSAGE_ID"] = messageId;
    addData["REDFISH_MESSAGE_ARGS"] = name + "," + status;
    addData["xyz.openbmc_project.Logging.Entry.Resolution"] = resolution;

    addEventLog(dbusConnection, messageId, severity, addData);
}

// Log an event indicating a leak detector sensor fault.
void LeakDetectSensor::logFaultEvent()
{
    if constexpr (debug)
    {
        std::cout << "Logging event for sensor: " << name << "\n";
    }

    std::string messageId = "ResourceEvent.1.0.ResourceStateChanged";
    std::string resolution = "Service degraded leak detector.";
    std::string severity = "xyz.openbmc_project.Logging.Entry.Level.Warning";
    std::string state = "Degraded";

    std::map<std::string, std::string> addData = {};
    addData["REDFISH_MESSAGE_ID"] = messageId;
    addData["REDFISH_MESSAGE_ARGS"] = name + "," + state;
    addData["xyz.openbmc_project.Logging.Entry.Resolution"] = resolution;

    addEventLog(dbusConnection, messageId, severity, addData);
}

void LeakDetectSensor::startShutdown()
{
    if (shutdownDelaySeconds)
    {
        std::cout << "Setting timer for " << shutdownDelaySeconds
                  << " second(s) delay before shutdown due to " << name
                  << ".\n";

        shutdownTimer.expires_after(std::chrono::seconds(shutdownDelaySeconds));
        shutdownTimer.async_wait([&](const boost::system::error_code& ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                return; // we're being canceled
            }

            if (ec)
            {
                std::cerr << "Shutdown Timer callback error: " << ec.message()
                          << "\n";
                return;
            }

            executeShutdown();
        });
    }
    else
    {
        executeShutdown();
    }
}

void LeakDetectSensor::executeShutdown()
{
    std::cout << "Chassis shutdown requested by " << name << ".\n";

    std::variant<std::string> transitionChassisOff =
        "xyz.openbmc_project.State.Chassis.Transition.Off";

    dbusConnection->async_method_call(
        [](const boost::system::error_code& ec) {
        if (ec)
        {
            std::cerr << "Failed to execute shutdown due to " << ec.message()
                      << "\n";
            return;
        }
    },
        "xyz.openbmc_project.State.Chassis",
        "/xyz/openbmc_project/state/chassis0",
        "org.freedesktop.DBus.Properties", "Set",
        "xyz.openbmc_project.State.Chassis", "RequestedPowerTransition",
        transitionChassisOff);
}

// Function to the Fault LED through phosphor-led-sysfs interfaces. The
// fault_led must be defined in the device tree.
void LeakDetectSensor::blinkFaultLed()
{
    std::cout << "Blinking Fault LED due to leak detected by " << name << ".\n";

    const char* ledService = "xyz.openbmc_project.LED.Controller.fault_led";
    const char* ledPath = "/xyz/openbmc_project/led/physical/fault_led";
    const char* ledInterface = "xyz.openbmc_project.Led.Physical";

    // Set blink rate of 4Hz with period of 250ms and duty of 50%
    std::variant<uint8_t> dutyOn = (uint8_t)50;
    std::variant<uint16_t> period = (uint16_t)250;

    std::variant<std::string> ledActionOff =
        "xyz.openbmc_project.Led.Physical.Action.Off";
    std::variant<std::string> ledActionBlink =
        "xyz.openbmc_project.Led.Physical.Action.Blink";

    // Set the LED to an Off state first before configuring the parameters
    // for Blink, as phosphor-led-sysfs requires a State transition for
    // new parameters to take effect.
    dbusConnection->async_method_call(
        [](const boost::system::error_code& ec) {
        if (ec)
        {
            std::cerr << "Failed to set fault LED to Off due to "
                      << ec.message() << "\n";
            return;
        }
    },
        ledService, ledPath, "org.freedesktop.DBus.Properties", "Set",
        ledInterface, "State", ledActionOff);

    // LED parameters such as Duty and Period must be set before enabling the
    // blink action on the LED.
    dbusConnection->async_method_call(
        [](const boost::system::error_code& ec) {
        if (ec)
        {
            std::cerr << "Failed to set fault LED Duty due to " << ec.message()
                      << "\n";
            return;
        }
    },
        ledService, ledPath, "org.freedesktop.DBus.Properties", "Set",
        ledInterface, "DutyOn", dutyOn);

    dbusConnection->async_method_call(
        [](const boost::system::error_code& ec) {
        if (ec)
        {
            std::cerr << "Failed to set fault LED Period due to "
                      << ec.message() << "\n";
            return;
        }
    },
        ledService, ledPath, "org.freedesktop.DBus.Properties", "Set",
        ledInterface, "Period", period);

    dbusConnection->async_method_call(
        [](const boost::system::error_code& ec) {
        if (ec)
        {
            std::cerr << "Failed to set fault LED to Blink due to "
                      << ec.message() << "\n";
            return;
        }
    },
        ledService, ledPath, "org.freedesktop.DBus.Properties", "Set",
        ledInterface, "State", ledActionBlink);
}

// Update the threshold value stored in the Entity Manager configurable so that
// it may be persisted through reboots and power cycles. On next start, the
// leak detect daemon will use the persisted config threshold value to as the
// threshold.
void LeakDetectSensor::persistThreshold(double newThreshold)
{
    std::variant<double> threshold(newThreshold);

    dbusConnection->async_method_call(
        [](const boost::system::error_code& ec) {
        if (ec)
        {
            std::cerr << "Failed to set leak threshold due to " << ec.message()
                      << "\n";
            return;
        }
    },
        entityManagerName, configurationPath, "org.freedesktop.DBus.Properties",
        "Set", "xyz.openbmc_project.Configuration.VoltageLeakDetector",
        "LeakThresholdVolts", threshold);
}

// Converts the current detector state into the corresponding Health Status
// string as defined in the schema
std::string
    LeakDetectSensor::getDetectorStatusString(DetectorState detectorState)
{
    switch (detectorState)
    {
        case DetectorState::NORMAL:
            return "OK";
            break;
        case DetectorState::LEAKAGE:
        case DetectorState::FAULT:
            return "Critical";
            break;
        default:
            throw std::runtime_error("Invalid detector state.");
    }
}

// Converts the current detector state into the corresponding resource State
// string as defined in the schema
std::string
    LeakDetectSensor::getDetectorStateString(DetectorState detectorState)
{
    switch (detectorState)
    {
        case DetectorState::NORMAL:
        case DetectorState::LEAKAGE:
            return "Enabled";
            break;
        case DetectorState::FAULT:
            return "Degraded";
            break;
        default:
            throw std::runtime_error("Invalid detector state.");
    }
}
