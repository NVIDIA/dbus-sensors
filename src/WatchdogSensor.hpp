#pragma once

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>

#include <memory>
#include <string>

using AssocInterface = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;

class WatchdogSensor :
    public AssocInterface,
    public std::enable_shared_from_this<WatchdogSensor>
{
  public:
    WatchdogSensor(
        sdbusplus::asio::object_server& objectServer,
        std::shared_ptr<sdbusplus::asio::connection>& conn,
        /*boost::asio::io_context& io,*/ const std::string& sensorName,
        const std::string& sensorConfiguration);
    ~WatchdogSensor() override;

    std::string name;
    std::string status;

  private:
    std::shared_ptr<sdbusplus::asio::dbus_interface> sensorInterface;
    sdbusplus::asio::object_server& objServer;
    std::shared_ptr<sdbusplus::bus::match_t> watchdogEventMatcher;
};
