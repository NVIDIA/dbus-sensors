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
#include <sdbusplus/vtable.hpp>

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <ostream>
#include <string>
#include <utility>

namespace write_protect
{

void WriteProtect::addObj(const std::string& name, const Config& config)
{
    objIfaces[name] = {config};
}

void WriteProtect::removeObj(const std::string& name)
{
    objIfaces.erase(name);
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
        line.request({service, ::gpiod::line_request::DIRECTION_OUTPUT, value});
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
                                    value);
}

int WriteProtect::readLine(const std::string& lineLabel)
{
    if (gpioLines.find(lineLabel) == gpioLines.end())
    {
        addLine(lineLabel, std::filesystem::exists(writeProtectFile));
    }
    gpioLines[lineLabel].set_config(::gpiod::line_request::DIRECTION_AS_IS, 0);
    return gpioLines[lineLabel].get_value();
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
            std::cerr << "Failed gpio line write "
                      << std::string(config.gpioLine)
                      << " error is: " << e.what() << std::endl;
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
        int lineValue = 0;
        try
        {
            lineValue = readLine(config.gpioLine);
        }
        catch (std::exception& e)
        {
            std::cerr << "Failed gpio line read "
                      << std::string(config.gpioLine)
                      << " error is: " << e.what() << std::endl;
            continue;
        }
        bool value = static_cast<bool>(lineValue);

        config.writeprotected = config.activeLow ? !value : value;
        if (!config.writeprotected)
        {
            globalWriteProtected = false;
        }
    }
    return globalWriteProtected;
}

void WriteProtect::createWriteProtectIf(const std::string& parentChassisId,
                                        const boost::system::error_code& e)
{
    if (e)
    {
        std::cerr << "Failed to create Write Protect dbus interface"
                  << " error is: " << e.what() << std::endl;
        return;
    }

    settingsIfPtr = objectServerPtr->add_interface(
        std::string(softwareWriteProtectObjPath) + parentChassisId,
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
void WriteProtect::setupWriteProtectIf(const std::string& parentChassisId)
{
    timer.cancel();
    timer.expires_after(std::chrono::seconds(
        /*delay to setup interface*/ 3));
    timer.async_wait(std::bind_front(&WriteProtect::createWriteProtectIf, this,
                                     parentChassisId));
}
} // namespace write_protect
