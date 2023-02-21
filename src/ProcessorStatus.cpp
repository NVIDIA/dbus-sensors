#include <unistd.h>

#include "ProcessorStatus.hpp"

#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

ProcessorStatus::ProcessorStatus(
    sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    boost::asio::io_service& io, const std::string& sensorName,
    const std::string& gpioName,
    const std::string& sensorConfiguration) :
    ItemInterface(static_cast<sdbusplus::bus::bus&>(*conn),
                  ("/xyz/openbmc_project/sensors/motherboard/cpu/" +
                   escapeName(sensorName))
                      .c_str(),
                  ItemInterface::action::defer_emit),
    std::enable_shared_from_this<ProcessorStatus>(),
    name(escapeName(sensorName)), gpio(gpioName), objServer(objectServer),
    procPresentEvent(io)
{
    sensorInterface = objectServer.add_interface(
        ("/xyz/openbmc_project/sensors/motherboard/cpu/" +
         escapeName(sensorName))
            .c_str(),
        CpuInterface::interface);

    fs::path p(sensorConfiguration);
    AssociationList assocs = {};
    assocs.emplace_back(
        std::make_tuple("chassis", "all_sensors", p.parent_path().string()));
    sdbusplus::xyz::openbmc_project::Association::server::Definitions::
        associations(assocs);

    if (!sensorInterface->initialize())
    {
        std::cerr << "error initializing sensor interface\n";
    }
    setupEvent(gpioName, procPresentLine, procPresentEvent);
}

ProcessorStatus::~ProcessorStatus()
{
    objServer.remove_interface(sensorInterface);
}

bool ProcessorStatus::setupEvent(const std::string& procGpioName, gpiod::line& gpioLine,
    boost::asio::posix::stream_descriptor& gpioEventDescriptor)
{
    // Find the GPIO line
    gpioLine = gpiod::find_line(procGpioName);
    if (!gpioLine)
    {
        std::cerr << "Failed to find the line\n";

        return false;
    }

    try
    {
        gpioLine.request({"proc-sensor", gpiod::line_request::EVENT_BOTH_EDGES,
                          gpiod::line_request::FLAG_ACTIVE_LOW});
    }
    catch (std::exception&)
    {
        std::cerr << "Failed to request events\n";
        return false;
    }

    sdbusplus::xyz::openbmc_project::Inventory::server::Item::present(
        gpioLine.get_value() == 1);

    int gpioLineFd = gpioLine.event_get_fd();
    if (gpioLineFd < 0)
    {
        std::cerr << "Failed to get fd\n";
        return false;
    }

    gpioEventDescriptor.assign(gpioLineFd);

    monitor(gpioEventDescriptor, gpioLine);

    return true;
}

void ProcessorStatus::monitor(boost::asio::posix::stream_descriptor& event,
                              gpiod::line& line)
{

    event.async_wait(
        boost::asio::posix::stream_descriptor::wait_read,
        [this, &event, &line](const boost::system::error_code ec) {
            if (ec)
            {
                std::cerr << " fd handler error: " << ec.message() << "\n";
                return;
            }
            gpiod::line_event lineEvent = line.event_read();
            sdbusplus::xyz::openbmc_project::Inventory::server::Item::present(
                lineEvent.event_type == gpiod::line_event::FALLING_EDGE);
            // Start monitoring for next event
            monitor(event, line);
        });
}
