/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
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

#include "WriteProtectSensor.hpp"

#include <gpiod.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/vtable.hpp>

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace write_protect
{

void WriteProtect::addObj(const std::string& name, const Config& config)
{
    objIfaces[name] = {config};

    if (settingsIfPtr)
    {
        settingsIfPtr->set_property(
            write_protect::properties::propertyWriteProtected,
            std::filesystem::exists(writeProtectFile));
    }
}

void WriteProtect::removeObj(const std::string& name)
{
    objIfaces.erase(name);

    // If all objects are removed, cleanup the interface
    if (objEmpty())
    {
        cleanupWriteProtectIf();
    }
}

void WriteProtect::cleanupWriteProtectIf()
{
    if (settingsIfPtr)
    {
        objectServerPtr->remove_interface(settingsIfPtr);
        settingsIfPtr.reset();
    }
}

bool WriteProtect::hasObj(const std::string& name)
{
    return objIfaces.find(name) != objIfaces.end();
}

bool WriteProtect::objEmpty()
{
    return objIfaces.empty();
}

void WriteProtect::addLine(const std::string& lineLabel, bool value)
{
    if (gpioLines.find(lineLabel) == gpioLines.end())
    {
        ::gpiod::line line = ::gpiod::find_line(lineLabel);
        line.request({service, ::gpiod::line_request::DIRECTION_OUTPUT,
                      static_cast<unsigned long long>(value)});
        gpioLines[lineLabel] = line;
    }
}

void WriteProtect::setLine(const std::string& lineLabel, bool value)
{
    if (gpioLines.find(lineLabel) == gpioLines.end())
    {
        addLine(lineLabel, value);
    }
    gpioLines[lineLabel].set_config(::gpiod::line_request::DIRECTION_OUTPUT,
                                    static_cast<unsigned long long>(value));
}

void WriteProtect::releaseLine(const std::string& lineLabel)
{
    if (gpioLines.find(lineLabel) != gpioLines.end())
    {
        ::gpiod::line line = ::gpiod::find_line(lineLabel);
        line.release();
        gpioLines.erase(lineLabel);
    }
}

bool WriteProtect::setWriteProtect(const bool& value)
{
    for (auto& obj : objIfaces)
    {
        auto& config = obj.second.config;
        try
        {
            setLine(config.gpioLine, config.activeLow ? !value : value);
        }
        catch (std::exception& e)
        {
            lg2::error("Failed gpio line write {GPIO_LINE}: {ERROR}",
                       "GPIO_LINE", config.gpioLine, "ERROR", e.what());
            continue;
        }
        config.writeprotected = value;
    }
    if (std::filesystem::exists(writeProtectFile) && (!value))
    {
        std::filesystem::remove(writeProtectFile);
    }
    else if (!std::filesystem::exists(writeProtectFile) && (value))
    {
        std::ofstream ofs(writeProtectFile);
        ofs.close();
    }

    return true;
}

bool WriteProtect::readWriteProtect()
{
    auto globalWriteProtected = true;
    for (auto& obj : objIfaces)
    {
        auto& config = obj.second.config;
        // Use the stored writeprotected value instead of reading GPIO
        if (!config.writeprotected)
        {
            globalWriteProtected = false;
        }
    }
    return globalWriteProtected;
}

void WriteProtect::createWriteProtectIf(const boost::system::error_code& e)
{
    if (e)
    {
        lg2::error("Failed to create Write Protect dbus interface: {ERROR}",
                   "ERROR", e.message());
        return;
    }

    settingsIfPtr = objectServerPtr->add_interface(
        std::string(softwareWriteProtectObjPath) + writeProtectParentChassisId,
        write_protect::interfaces::settingsIf);

    settingsIfPtr->register_property_rw<bool>(
        write_protect::properties::propertyWriteProtected,
        sdbusplus::vtable::property_::emits_change,
        [this](const auto& newStatus, const auto&) {
            return setWriteProtect(newStatus);
        },
        [this](const auto&) { return readWriteProtect(); });

    settingsIfPtr->initialize();

    settingsIfPtr->set_property(
        write_protect::properties::propertyWriteProtected,
        std::filesystem::exists(writeProtectFile));
}
void WriteProtect::setupWriteProtectIf()
{
    timer.cancel();
    timer.expires_after(std::chrono::seconds(
        /*delay to setup interface*/ 3));
    timer.async_wait(
        std::bind_front(&WriteProtect::createWriteProtectIf, this));
}
} // namespace write_protect
