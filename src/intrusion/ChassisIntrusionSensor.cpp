/*
// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "ChassisIntrusionSensor.hpp"

#include <fcntl.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <sys/syslog.h>
#include <systemd/sd-journal.h>
#include <unistd.h>

#include <Utils.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <gpiod.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C"
{
#include <i2c/smbus.h>
#include <linux/i2c-dev.h>
}

static constexpr bool debug = false;

static constexpr unsigned int defaultPollSec = 1;
static constexpr unsigned int sensorFailedPollSec = 5;
static unsigned int intrusionSensorPollSec = defaultPollSec;
static constexpr const char* hwIntrusionValStr =
    "xyz.openbmc_project.Chassis.Intrusion.Status.HardwareIntrusion";
static constexpr const char* normalValStr =
    "xyz.openbmc_project.Chassis.Intrusion.Status.Normal";
static constexpr const char* manualRearmStr =
    "xyz.openbmc_project.Chassis.Intrusion.RearmMode.Manual";
static constexpr const char* autoRearmStr =
    "xyz.openbmc_project.Chassis.Intrusion.RearmMode.Automatic";

// SMLink Status Register
const static constexpr size_t pchStatusRegIntrusion = 0x04;

// Status bit field masks
const static constexpr size_t pchRegMaskIntrusion = 0x01;

// Value to clear intrusion status hwmon file
const static constexpr size_t intrusionStatusHwmonClearValue = 0;

void ChassisIntrusionSensor::updateValue(const size_t& value)
{
    std::string newValue = value != 0 ? hwIntrusionValStr : normalValStr;

    // Take no action if the hardware status does not change
    // Same semantics as Sensor::updateValue(const double&)
    if (newValue == mValue)
    {
        return;
    }

    if constexpr (debug)
    {
        lg2::info("Update value from '{VALUE}' to '{NEWVALUE}'", "VALUE",
                  mValue, "NEWVALUE", newValue);
    }

    // Automatic Rearm mode allows direct update
    // Manual Rearm mode requires a rearm action to clear the intrusion
    // status
    if (!mAutoRearm)
    {
        if (newValue == normalValStr)
        {
            // Chassis is first closed from being open. If it has been
            // rearmed externally, reset the flag, update mValue and
            // return, without having to write "Normal" to DBus property
            // (because the rearm action already did).
            // Otherwise, return with no more action.
            if (mRearmFlag)
            {
                mRearmFlag = false;
                mValue = newValue;
            }
            return;
        }
    }

    // Flush the rearm flag everytime it allows an update to Dbus
    mRearmFlag = false;

    // indicate that it is internal set call
    mOverridenState = false;
    mInternalSet = true;
    mIface->set_property("Status", newValue);
    mInternalSet = false;

    mValue = newValue;
}

int ChassisIntrusionPchSensor::readSensor()
{
    int32_t statusMask = pchRegMaskIntrusion;
    int32_t statusReg = pchStatusRegIntrusion;

    int32_t value = i2c_smbus_read_byte_data(mBusFd, statusReg);
    if constexpr (debug)
    {
        lg2::info("Pch type: raw value is '{VALUE}'", "VALUE", value);
    }

    if (value < 0)
    {
        lg2::error("i2c_smbus_read_byte_data failed");
        return -1;
    }

    // Get status value with mask
    value &= statusMask;

    if constexpr (debug)
    {
        lg2::info("Pch type: masked raw value is '{VALUE}'", "VALUE", value);
    }
    return value;
}

void ChassisIntrusionPchSensor::pollSensorStatus()
{
    std::weak_ptr<ChassisIntrusionPchSensor> weakRef = weak_from_this();

    // setting a new experation implicitly cancels any pending async wait
    mPollTimer.expires_after(std::chrono::seconds(intrusionSensorPollSec));

    mPollTimer.async_wait([weakRef](const boost::system::error_code& ec) {
        // case of being canceled
        if (ec == boost::asio::error::operation_aborted)
        {
            lg2::error("Timer of intrusion sensor is cancelled");
            return;
        }

        std::shared_ptr<ChassisIntrusionPchSensor> self = weakRef.lock();
        if (!self)
        {
            lg2::error("ChassisIntrusionSensor no self");
            return;
        }

        int value = self->readSensor();
        if (value < 0)
        {
            intrusionSensorPollSec = sensorFailedPollSec;
        }
        else
        {
            intrusionSensorPollSec = defaultPollSec;
            self->updateValue(value);
        }

        // trigger next polling
        self->pollSensorStatus();
    });
}

int ChassisIntrusionGpioSensor::readSensor()
{
    mGpioLine.event_read();
    auto value = mGpioLine.get_value();
    if constexpr (debug)
    {
        lg2::info("Gpio type: raw value is '{VALUE}'", "VALUE", value);
    }
    return value;
}

void ChassisIntrusionGpioSensor::pollSensorStatus()
{
    mGpioFd.async_wait(
        boost::asio::posix::stream_descriptor::wait_read,
        [this](const boost::system::error_code& ec) {
            if (ec == boost::system::errc::bad_file_descriptor)
            {
                return; // we're being destroyed
            }

            if (ec)
            {
                lg2::error("Error on GPIO based intrusion sensor wait event");
            }
            else
            {
                int value = readSensor();
                if (value >= 0)
                {
                    updateValue(value);
                }
                // trigger next polling
                pollSensorStatus();
            }
        });
}

int ChassisIntrusionHwmonSensor::readSensor()
{
    int value = 0;

    std::fstream stream(mHwmonPath, std::ios::in | std::ios::out);
    if (!stream.good())
    {
        lg2::error("Error reading status at '{PATH}'", "PATH", mHwmonPath);
        return -1;
    }

    std::string line;
    if (!std::getline(stream, line))
    {
        lg2::error("Error reading status at '{PATH}'", "PATH", mHwmonPath);
        return -1;
    }

    try
    {
        value = std::stoi(line);
        if constexpr (debug)
        {
            lg2::info("Hwmon type: raw value is '{VALUE}'", "VALUE", value);
        }
    }
    catch (const std::invalid_argument& e)
    {
        lg2::error("Error reading status at '{PATH}': '{ERR}'", "PATH",
                   mHwmonPath, "ERR", e);
        return -1;
    }

    // Reset chassis intrusion status after every reading
    stream << intrusionStatusHwmonClearValue;

    return value;
}

void ChassisIntrusionHwmonSensor::pollSensorStatus()
{
    std::weak_ptr<ChassisIntrusionHwmonSensor> weakRef = weak_from_this();

    // setting a new experation implicitly cancels any pending async wait
    mPollTimer.expires_after(std::chrono::seconds(intrusionSensorPollSec));

    mPollTimer.async_wait([weakRef](const boost::system::error_code& ec) {
        // case of being canceled
        if (ec == boost::asio::error::operation_aborted)
        {
            lg2::error("Timer of intrusion sensor is cancelled");
            return;
        }

        std::shared_ptr<ChassisIntrusionHwmonSensor> self = weakRef.lock();
        if (!self)
        {
            lg2::error("ChassisIntrusionSensor no self");
            return;
        }

        int value = self->readSensor();
        if (value < 0)
        {
            intrusionSensorPollSec = sensorFailedPollSec;
        }
        else
        {
            intrusionSensorPollSec = defaultPollSec;
            self->updateValue(value);
        }

        // trigger next polling
        self->pollSensorStatus();
    });
}

static std::string formatTimestamp(uint64_t timestamp)
{
    using namespace std::chrono;
    auto timePoint =
        system_clock::from_time_t(static_cast<std::time_t>(timestamp));
    auto timePointSec = floor<seconds>(timePoint);
    return std::format("{:%Y-%m-%d %H:%M:%S}", timePointSec);
}

// Send the event to the logging service.
void ChassisIntrusionSensor::sendEventToLoggingService(
    IntrusionEvent event, const std::string& timestamp)
{
    struct EventInfo
    {
        const char* id;
        const char* message;
        const char* severity;
    };

    static constexpr std::array<EventInfo, 2> eventInfo = {
        /* Assert   */ EventInfo{
            "OpenBMC.0.1.ChassisIntrusionDetected",
            "Chassis intrusion assert event.",
            "xyz.openbmc_project.Logging.Entry.Level.Warning"},
        /* Deassert */ EventInfo{
            "OpenBMC.0.1.ChassisIntrusionReset",
            "Chassis intrusion de-assert event.",
            "xyz.openbmc_project.Logging.Entry.Level.Informational"}};

    const bool usePlainText =
        (event == IntrusionEvent::Assert && !timestamp.empty());
    const std::string& baseMessageId = eventInfo[static_cast<size_t>(event)].id;
    const std::string severity = eventInfo[static_cast<size_t>(event)].severity;
    const std::string messageId =
        usePlainText
            ? std::format("Chassis intrusion detected at {}", timestamp)
            : baseMessageId;

    std::map<std::string, std::string> addData;
    if (!usePlainText)
    {
        addData.emplace("REDFISH_MESSAGE_ID", baseMessageId);

        // Only include args when it's meaningful
        if (!timestamp.empty())
        {
            addData.emplace("REDFISH_MESSAGE_ARGS", timestamp);
        }
    }

    addEventLog(dbusConnection, messageId, severity, addData);
}

void ChassisIntrusionSensor::sendAssertEventToJournal(
    bool hasTimestamp, const std::string& timestamp)
{
    const std::string message = "Chassis intrusion assert event";
    const int priority = LOG_INFO;

    if (hasTimestamp)
    {
        const std::string formattedMessage =
            std::format("Chassis intrusion detected at {}", timestamp);
        sd_journal_send("MESSAGE=%s", formattedMessage.c_str(), "PRIORITY=%i",
                        priority, NULL);
    }
    else
    {
        sd_journal_send("MESSAGE=%s", message.c_str(), "PRIORITY=%i", priority,
                        "REDFISH_MESSAGE_ID=%s",
                        "OpenBMC.0.1.ChassisIntrusionDetected", NULL);
    }
}

void ChassisIntrusionSensor::sendDeassertEventToJournal()
{
    sd_journal_send("MESSAGE=%s", "Chassis intrusion de-assert event",
                    "PRIORITY=%i", LOG_INFO, "REDFISH_MESSAGE_ID=%s",
                    "OpenBMC.0.1.ChassisIntrusionReset", NULL);
}

int ChassisIntrusionSensor::setSensorValue(const std::string& req,
                                           std::string& propertyValue)
{
    if (!mInternalSet)
    {
        /*
           1. Assuming that setting property in Automatic mode causes
           no effect but only event logs and propertiesChanged signal
           (because the property will be updated continuously to the
           current hardware status anyway), only update Status property
           and raise rearm flag in Manual rearm mode.
           2. Only accept Normal value from an external call.
        */
        if (!mAutoRearm && req == normalValStr)
        {
            mRearmFlag = true;
            propertyValue = req;
            mOverridenState = true;
        }
    }
    else if (!mOverridenState)
    {
        propertyValue = req;
    }
    else
    {
        return 1;
    }
    // Send intrusion event to Redfish
    if (mValue == normalValStr && propertyValue != normalValStr)
    {
        // Intrusion detected - check if timestamp is available
        const bool hasTimestamp = hasTimestampSupport();
        const std::string timestamp =
            hasTimestamp ? formatTimestamp(getIntrusionTimestamp()) : "";

#ifdef INTRUSION_DBUS_LOG
        sendEventToLoggingService(IntrusionEvent::Assert, timestamp);
#else
        sendAssertEventToJournal(hasTimestamp, timestamp);
#endif
        onEventTrigger();
    }
    else if (mValue == hwIntrusionValStr && propertyValue == normalValStr)
    {
#ifdef INTRUSION_DBUS_LOG
        sendEventToLoggingService(IntrusionEvent::Deassert);
#else
        sendDeassertEventToJournal();
#endif
    }
    return 1;
}

void ChassisIntrusionSensor::start()
{
    mIface->register_property(
        "Status", mValue,
        [&](const std::string& req, std::string& propertyValue) {
            return setSensorValue(req, propertyValue);
        });
    std::string rearmStr = mAutoRearm ? autoRearmStr : manualRearmStr;
    mIface->register_property("Rearm", rearmStr);
    mIface->initialize();
    pollSensorStatus();
}

ChassisIntrusionSensor::ChassisIntrusionSensor(
    bool autoRearm, sdbusplus::asio::object_server& objServer,
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    std::string sensorName) :
    mValue(normalValStr), mAutoRearm(autoRearm), mObjServer(objServer),
    dbusConnection(conn), mSensorName(std::move(sensorName))

{
    mIface = mObjServer.add_interface("/xyz/openbmc_project/Chassis/Intrusion",
                                      "xyz.openbmc_project.Chassis.Intrusion");
}

ChassisIntrusionPchSensor::ChassisIntrusionPchSensor(
    bool autoRearm, boost::asio::io_context& io,
    sdbusplus::asio::object_server& objServer, int busId, int slaveAddr) :
    ChassisIntrusionSensor(autoRearm, objServer), mPollTimer(io)
{
    if (busId < 0 || slaveAddr <= 0)
    {
        throw std::invalid_argument(
            "Invalid i2c bus " + std::to_string(busId) + " address " +
            std::to_string(slaveAddr) + "\n");
    }

    mSlaveAddr = slaveAddr;

    std::string devPath = "/dev/i2c-" + std::to_string(busId);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    mBusFd = open(devPath.c_str(), O_RDWR | O_CLOEXEC);
    if (mBusFd < 0)
    {
        throw std::invalid_argument("Unable to open " + devPath + "\n");
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (ioctl(mBusFd, I2C_SLAVE_FORCE, mSlaveAddr) < 0)
    {
        throw std::runtime_error("Unable to set device address\n");
    }

    unsigned long funcs = 0;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (ioctl(mBusFd, I2C_FUNCS, &funcs) < 0)
    {
        throw std::runtime_error("Don't support I2C_FUNCS\n");
    }

    if ((funcs & I2C_FUNC_SMBUS_READ_BYTE_DATA) == 0U)
    {
        throw std::runtime_error(
            "Do not have I2C_FUNC_SMBUS_READ_BYTE_DATA \n");
    }
}

ChassisIntrusionGpioSensor::ChassisIntrusionGpioSensor(
    bool autoRearm, boost::asio::io_context& io,
    sdbusplus::asio::object_server& objServer, bool gpioInverted) :
    ChassisIntrusionSensor(autoRearm, objServer), mGpioInverted(gpioInverted),
    mGpioFd(io)
{
    mGpioLine = gpiod::find_line(mPinName);
    if (!mGpioLine)
    {
        throw std::invalid_argument(
            "Error finding gpio pin name: " + mPinName + "\n");
    }
    mGpioLine.request(
        {"ChassisIntrusionSensor", gpiod::line_request::EVENT_BOTH_EDGES,
         mGpioInverted ? gpiod::line_request::FLAG_ACTIVE_LOW : 0});

    auto gpioLineFd = mGpioLine.event_get_fd();
    if (gpioLineFd < 0)
    {
        throw std::invalid_argument("Failed to get " + mPinName + " fd\n");
    }

    mGpioFd.assign(gpioLineFd);
}

ChassisIntrusionHwmonSensor::ChassisIntrusionHwmonSensor(
    bool autoRearm, boost::asio::io_context& io,
    sdbusplus::asio::object_server& objServer, std::string hwmonName,
    std::string sensorName,
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    std::string expectedDeviceName) :
    ChassisIntrusionSensor(autoRearm, objServer, conn, std::move(sensorName)),
    mHwmonName(std::move(hwmonName)), mPollTimer(io)
{
    std::vector<std::filesystem::path> paths;

    if (!findFiles(std::filesystem::path("/sys/class/hwmon"), mHwmonName,
                   paths))
    {
        throw std::invalid_argument("Failed to find hwmon path in sysfs\n");
    }

    if (paths.empty())
    {
        throw std::invalid_argument(
            "Hwmon file " + mHwmonName + " can't be found in sysfs\n");
    }

    // If multiple alarm files found, verify by checking hwmon device name
    std::filesystem::path validatedPath;
    bool foundValidDevice = false;

    for (const auto& path : paths)
    {
        // Get hwmon device directory (e.g., /sys/class/hwmon/hwmon3)
        std::filesystem::path hwmonDeviceDir = path.parent_path();
        std::filesystem::path nameFilePath = hwmonDeviceDir / "name";

        if (!std::filesystem::exists(nameFilePath))
        {
            if constexpr (debug)
            {
                lg2::info("No name file in '{DIR}'", "DIR",
                          hwmonDeviceDir.string());
            }
            continue;
        }

        // Read the device name from the name file
        std::ifstream nameFile(nameFilePath);
        if (!nameFile.good())
        {
            lg2::warning("Failed to read name file at '{PATH}'", "PATH",
                         nameFilePath.string());
            continue;
        }

        std::string deviceName;
        if (!std::getline(nameFile, deviceName))
        {
            lg2::warning("Failed to read device name from '{PATH}'", "PATH",
                         nameFilePath.string());
            continue;
        }

        if constexpr (debug)
        {
            lg2::info("Found hwmon device '{NAME}' at '{PATH}'", "NAME",
                      deviceName, "PATH", hwmonDeviceDir.string());
        }

        // Verify the device name matches the expected device name
        if (!expectedDeviceName.empty() && deviceName != expectedDeviceName)
        {
            if constexpr (debug)
            {
                lg2::info(
                    "Device name '{NAME}' does not match expected '{EXPECTED}', skipping",
                    "NAME", deviceName, "EXPECTED", expectedDeviceName);
            }
            continue;
        }

        validatedPath = path;
        foundValidDevice = true;
        break; // Use the first matching device
    }

    if (!foundValidDevice)
    {
        std::string errorMsg = "No valid hwmon device found";
        if (!expectedDeviceName.empty())
        {
            errorMsg += " with expected name: " + expectedDeviceName;
        }
        throw std::invalid_argument(errorMsg + "\n");
    }

    mHwmonPath = validatedPath.string();
    // Check if timestamp file exists in this device (NCT3018Y)
    std::filesystem::path timestampFilePath =
        validatedPath.parent_path() / "intrusion0_timestamp";

    if (std::filesystem::exists(timestampFilePath))
    {
        mTimestampPath = timestampFilePath.string();
        mIsTimestampMode = true;
        if constexpr (debug)
        {
            lg2::info("Found timestamp path: '{PATH}'", "PATH", mTimestampPath);
        }

        // Create D-Bus interface for exposing intrusion timestamp
        mTimeIface = getObjectServer().add_interface(
            "/xyz/openbmc_project/Chassis/Intrusion",
            "xyz.openbmc_project.Time.EpochTime");
        mTimeIface->register_property("Elapsed", readTimestampFromRtc());
        mTimeIface->initialize();
    }

    if constexpr (debug)
    {
        lg2::info(
            "Found '{NUM_PATHS}' paths for intrusion status. The path used is: '{PATH}'",
            "NUM_PATHS", paths.size(), "PATH", mHwmonPath);
    }
}

ChassisIntrusionSensor::~ChassisIntrusionSensor()
{
    mObjServer.remove_interface(mIface);
}

ChassisIntrusionPchSensor::~ChassisIntrusionPchSensor()
{
    mPollTimer.cancel();
    if (close(mBusFd) < 0)
    {
        lg2::error("Failed to close fd '{FD}'", "FD", mBusFd);
    }
}

ChassisIntrusionGpioSensor::~ChassisIntrusionGpioSensor()
{
    mGpioFd.close();
    if (mGpioLine)
    {
        mGpioLine.release();
    }
}

ChassisIntrusionHwmonSensor::~ChassisIntrusionHwmonSensor()
{
    mPollTimer.cancel();
    if (mTimeIface)
    {
        getObjectServer().remove_interface(mTimeIface);
    }
}

uint64_t ChassisIntrusionHwmonSensor::readTimestampFromRtc() const
{
    std::ifstream stream(mTimestampPath);
    if (!stream.good())
    {
        lg2::error("Error opening RTC timestamp at '{PATH}'", "PATH",
                   mTimestampPath);
        // Return max uint64 to indicate read failure (0 is a valid timestamp)
        return invalidTimestamp;
    }

    std::string timestampStr;
    if (!std::getline(stream, timestampStr))
    {
        lg2::error("Error reading RTC timestamp at '{PATH}'", "PATH",
                   mTimestampPath);
        return invalidTimestamp;
    }

    uint64_t timestamp = 0;
    try
    {
        timestamp = std::stoull(timestampStr);
        if constexpr (debug)
        {
            lg2::info("RTC timestamp: '{TIMESTAMP}'", "TIMESTAMP", timestamp);
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Error converting RTC timestamp '{STR}': '{ERROR}'", "STR",
                   timestampStr, "ERROR", e);
        return invalidTimestamp;
    }

    return timestamp;
}
