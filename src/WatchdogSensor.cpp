#include "WatchdogSensor.hpp"

#include "Utils.hpp"

#include <boost/container/flat_map.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

WatchdogSensor::WatchdogSensor(
    sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    /*boost::asio::io_context& io,*/
    const std::string& sensorName, const std::string& sensorConfiguration) :
    AssocInterface(
        static_cast<sdbusplus::bus_t&>(*conn),
        ("/xyz/openbmc_project/sensors/watchdog/" + escapeName(sensorName))
            .c_str(),
        AssocInterface::action::defer_emit),
    name(sensorName), objServer(objectServer)
{
    sensorInterface = objectServer.add_interface(
        ("/xyz/openbmc_project/sensors/watchdog/" + escapeName(sensorName)),
        "xyz.openbmc_project.Inventory.Item.Watchdog");
    sensorInterface->register_property(
        "Status", status,
        [&](const std::string& newStatus, std::string& oldStatus) {
            oldStatus = newStatus;
            status = newStatus;
            return 1;
        });

    fs::path p(sensorConfiguration);
    AssociationList assocs = {};
    assocs.emplace_back(
        std::make_tuple("chassis", "all_sensors", p.parent_path().string()));
    sdbusplus::xyz::openbmc_project::Association::server::Definitions::
        associations(assocs);
    if (!sensorInterface->initialize())
    {
        std::cerr << "error initializing value interface\n";
    }

    auto watchdogEventMatcherCallback = [this, conn](
                                            sdbusplus::message::message& msg) {
        std::optional<std::string_view> expireAction;

        sdbusplus::message::message getWatchdogStatus =
            conn->new_method_call(msg.get_sender(), msg.get_path(),
                                  "org.freedesktop.DBus.Properties", "GetAll");
        getWatchdogStatus.append("xyz.openbmc_project.State.Watchdog");
        boost::container::flat_map<std::string,
                                   std::variant<std::string, uint64_t, bool>>
            watchdogStatus;

        try
        {
            sdbusplus::message::message getWatchdogStatusResp =
                conn->call(getWatchdogStatus);
            getWatchdogStatusResp.read(watchdogStatus);
        }
        catch (const sdbusplus::exception_t&)
        {
            std::cerr << "error getting watchdog status from " << msg.get_path()
                      << "\n";
            return;
        }

        auto getExpireAction = watchdogStatus.find("ExpireAction");
        if (getExpireAction != watchdogStatus.end())
        {
            expireAction = std::get<std::string>(getExpireAction->second);
            expireAction->remove_prefix(std::min(
                expireAction->find_last_of(".") + 1, expireAction->size()));
        }

        if (*expireAction == "HardReset")
        {
            sensorInterface->set_property(
                "Status", static_cast<std::string>("HardReset"));
        }
        else if (*expireAction == "PowerOff")
        {
            sensorInterface->set_property("Status",
                                          static_cast<std::string>("PowerOff"));
        }
        else if (*expireAction == "PowerCycle")
        {
            sensorInterface->set_property(
                "Status", static_cast<std::string>("PowerCycle"));
        }
        else if (*expireAction == "None")
        {
            sensorInterface->set_property(
                "Status", static_cast<std::string>("TimerExpired"));
        }
        else if (*expireAction == "TimerInterrupt")
        {
            sensorInterface->set_property(
                "Status", static_cast<std::string>("TimerInterrupt"));
        }
    };

    watchdogEventMatcher = std::make_shared<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*conn),
        "type='signal',interface='xyz.openbmc_project.Watchdog',"
        "member='Timeout'",
        std::move(watchdogEventMatcherCallback));
}

WatchdogSensor::~WatchdogSensor()
{
    objServer.remove_interface(sensorInterface);
}
