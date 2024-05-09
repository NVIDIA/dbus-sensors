#include "SELSensor.hpp"

#include <unistd.h>

#include <boost/asio/read_until.hpp>

#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

SELSensor::SELSensor(sdbusplus::asio::object_server& objectServer,
                     std::shared_ptr<sdbusplus::asio::connection>& conn,
                     const std::string& sensorName,
                     const std::string& sensorConfiguration) :
    AssocInterface(
        static_cast<sdbusplus::bus::bus&>(*conn),
        ("/xyz/openbmc_project/sensors/EventLogging/" + escapeName(sensorName))
            .c_str(),
        AssocInterface::action::defer_emit),
    name(sensorName), objServer(objectServer)
{
    sensorInterface = objectServer.add_interface(
        ("/xyz/openbmc_project/sensors/EventLogging/" + escapeName(sensorName)),
        "xyz.openbmc_project.Inventory.Item.SEL");
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

    auto selEventMatcherCallback = [this,
                                    conn](sdbusplus::message::message& msg) {
        std::string selSignal;
        msg.read(selSignal);

        std::string_view signalStrView = selSignal;
        signalStrView.remove_prefix(std::min(
            signalStrView.find_last_of('.') + 1, signalStrView.size()));
        std::optional<std::string_view> signal = signalStrView;

        if (*signal == "Full")
        {
            sensorInterface->set_property("Status",
                                          static_cast<std::string>("SELFull"));
        }
        else if (*signal == "Partially")
        {
            sensorInterface->set_property(
                "Status", static_cast<std::string>("SELAlmostFull"));
        }
        else if (*signal == "Cleared")
        {
            sensorInterface->set_property(
                "Status", static_cast<std::string>("LogCleared"));
        }
    };

    selEventMatcher = std::make_shared<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*conn),
        "type='signal',interface='xyz.openbmc_project.Logging.Create',"
        "member='SEL'",
        std::move(selEventMatcherCallback));
}

SELSensor::~SELSensor()
{
    objServer.remove_interface(sensorInterface);
}
