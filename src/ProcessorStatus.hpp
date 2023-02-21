#pragma once

#include "Utils.hpp"
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/streambuf.hpp>
#include <gpiod.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Cpu/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/server.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using ItemInterface = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Inventory::server::Item,
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;

using CpuInterface =
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Cpu;

class ProcessorStatus :
    public ItemInterface,
    public std::enable_shared_from_this<ProcessorStatus>
{
  public:
    ProcessorStatus(sdbusplus::asio::object_server& objectServer,
                    std::shared_ptr<sdbusplus::asio::connection>& conn,
                    boost::asio::io_service& io, const std::string& sensorName,
                    const std::string& gpioName,
                    const std::string& sensorConfiguration);
    ~ProcessorStatus() override;

    std::string name;
    std::string gpio;

  private:
    sdbusplus::asio::object_server& objServer;
    std::shared_ptr<sdbusplus::asio::dbus_interface> sensorInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> association;
    // GPIO Lines and Event Descriptors
    gpiod::line procPresentLine;
    boost::asio::posix::stream_descriptor procPresentEvent;
    bool setupEvent(const std::string& gpioName,
                    gpiod::line& gpioLine,
                    boost::asio::posix::stream_descriptor& gpioEventDescriptor);
    void monitor(boost::asio::posix::stream_descriptor& event,
                 gpiod::line& line);
};
