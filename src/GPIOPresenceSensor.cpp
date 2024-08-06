/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
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

#include "GPIOPresenceSensor.hpp"

#include <cerrno>

namespace gpio_presence_sensing
{
void GPIOPresence::addObj(
    std::unique_ptr<sdbusplus::asio::dbus_interface>& statusIfc,
    const std::string& objPath, const Config& config)
{
    std::cerr << "New objPath added " << objPath << std::endl;
    objIfaces[objPath] = {std::move(statusIfc), config};
}

void GPIOPresence::removeObj(const std::string& objPath)
{
    if (objIfaces.find(objPath) != objIfaces.end())
    {
        std::cerr << "Remove objPath " << objPath << std::endl;
        objIfaces.erase(objPath);
    }
}

bool GPIOPresence::hasObj(const std::string& objPath)
{
    return objIfaces.find(objPath) != objIfaces.end();
}

void GPIOPresence::waitForGPIOEvent(
    const std::string& name, const std::function<void(bool)>& eventHandler,
    gpiod::line& line, boost::asio::posix::stream_descriptor& event)
{
    event.async_wait(boost::asio::posix::stream_descriptor::wait_read,
                     [ref{this}, &name, eventHandler, &line,
                      &event](const boost::system::error_code ec) {
        if (ec)
        {
            std::cout << name << ""
                      << " fd handler error: " << ec.message() << " Error"
                      << std::endl;
            return;
        }
        gpiod::line_event lineEvent = line.event_read();
        bool present = true;
        present = !(lineEvent.event_type == gpiod::line_event::RISING_EDGE);
        eventHandler(present);
        ref->waitForGPIOEvent(name, eventHandler, line, event);
    });
}

bool GPIOPresence::requestGPIOEvents(const std::string& name,
                                     const std::function<void(bool)>& handler,
                                     boost::asio::io_context& gpioContext)
{
    // Find the GPIO line
    gpiod::line gpioLine = gpiod::find_line(name);
    std::shared_ptr<boost::asio::posix::stream_descriptor> gpioEventDescriptor =
        std::make_shared<boost::asio::posix::stream_descriptor>(gpioContext);

    lines.push_back(gpioLine);
    linesSD.push_back(gpioEventDescriptor);
    if (!gpioLine)
    {
        std::cout << "Failed to find the {GPIO_NAME} line" << name << std::endl;
        return false;
    }

    try
    {
        gpioLine.request({appName, gpiod::line_request::EVENT_BOTH_EDGES, {}});
    }
    catch (const std::exception& e)
    {
        std::cout << "Failed to request events for {GPIO_NAME}: " << name
                  << " Error" << e.what() << std::endl;
        return false;
    }
    int gpioLineFd = gpioLine.event_get_fd();
    if (gpioLineFd < 0)
    {
        std::cout << "Failed to get {GPIO_NAME} line" << name << std::endl;
        return false;
    }

    gpioEventDescriptor->assign(gpioLineFd);

    waitForGPIOEvent(name, handler, gpioLine, *gpioEventDescriptor);
    return true;
}

void GPIOPresence::updatePresence(const std::string& gpioLine, bool state)
{
    for (auto& obj : objIfaces)
    {
        auto& config = obj.second.config;
        if (config.gpioLine == gpioLine)
        {
            bool present = state;
            present = config.activeLow ? !present : present;
            if (present != config.present)
            {
                std::cout << "gpioLine " << config.name << " change state to "
                          << (present ? "connected" : "disconnected")
                          << std::endl;
                obj.second.statusIfc->set_property(
                    gpio_presence_sensing::properties::propertyPresent,
                    present);
                config.present = present;
            }
        }
    }
}

void GPIOPresence::addInputLine(const std::string& lineLabel)
{
    if (gpioLines.find(lineLabel) == gpioLines.end())
    {
        ::gpiod::line line = ::gpiod::find_line(lineLabel);
        line.request({service, ::gpiod::line_request::DIRECTION_INPUT,
                      /*default_val=*/0});
        gpioLines[lineLabel] = line;
    }
}

int GPIOPresence::readLine(const std::string& lineLabel)
{
    if (gpioLines.find(lineLabel) == gpioLines.end())
    {
        addInputLine(lineLabel);
    }
    return gpioLines[lineLabel].get_value();
}

void GPIOPresence::releaseLine(const std::string& lineLabel)
{
    if (gpioLines.find(lineLabel) != gpioLines.end())
    {
        ::gpiod::line line = ::gpiod::find_line(lineLabel);
        line.release();
        gpioLines.erase(lineLabel);
    }
}

void GPIOPresence::readPresent()
{
    for (auto& obj : objIfaces)
    {
        auto& config = obj.second.config;
        int lineValue = 0;
        try
        {
            lineValue = readLine(config.gpioLine);
            releaseLine(config.gpioLine);
        }
        catch (const std::invalid_argument& e)
        {
            std::cerr << "Failed gpio line read "
                      << std::string(config.gpioLine)
                      << " error is: " << e.what() << std::endl;
            return;
        }
        catch (const std::runtime_error& e)
        {
            std::cerr << "Failed gpio line read "
                      << std::string(config.gpioLine)
                      << " error is: " << e.what() << std::endl;
            continue;
        }
        bool present = static_cast<bool>(lineValue);
        present = config.activeLow ? !present : present;
        if (present != config.present)
        {
            std::cout << "Cable " << config.name << " change state to "
                      << (present ? "connected" : "disconnected") << std::endl;
            obj.second.statusIfc->set_property(
                gpio_presence_sensing::properties::propertyPresent, present);
            config.present = present;
        }
    }
}

void GPIOPresence::startGPIOEventMonitor(boost::asio::io_context& gpioContext)
{
    for (auto& obj : objIfaces)
    {
        auto& config = obj.second.config;
        requestGPIOEvents(
            config.gpioLine,

            [self(shared_from_this()), gpioLine{config.gpioLine}](bool state) {
            self->updatePresence(gpioLine, state);
        },
            gpioContext);
    }
}
} // namespace gpio_presence_sensing
