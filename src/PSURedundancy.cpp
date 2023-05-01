#include <unistd.h>

#include "PSURedundancy.hpp"
#include <boost/asio/read_until.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

PSURedundancy::PSURedundancy(sdbusplus::asio::object_server& objectServer,
                             std::shared_ptr<sdbusplus::asio::connection>& conn,
                             const std::string& sensorName, int totalPSUCount,
                             int redundantPSUCount, int sufficientPSUCount,
                             const std::string& sensorConfiguration) :
    AssocInterface(
        static_cast<sdbusplus::bus::bus&>(*conn),
        ("/xyz/openbmc_project/sensors/PSU/" + escapeName(sensorName)).c_str(),
        AssocInterface::action::defer_emit),
    std::enable_shared_from_this<PSURedundancy>(), name(sensorName),
    totalPSU(totalPSUCount), redundantPSU(redundantPSUCount),
    previousWorkablePSU(totalPSUCount), sufficientPSU(sufficientPSUCount),
    objServer(objectServer)
{
    sensorInterface = objectServer.add_interface(
        ("/xyz/openbmc_project/sensors/PSU/" + escapeName(sensorName)).c_str(),
        "xyz.openbmc_project.Control.PowerSupplyRedundancy");
    sensorInterface->register_property(
        "Status", status,
        [&](const std::string& newStatus, std::string& oldStatus) {
            oldStatus = newStatus;
            status = newStatus;
            return 1;
        });

    sensorInterface->register_property(
        "RedundancyLost", redundancyLost,
        [&](const bool& newStatus, bool& oldStatus) {
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
        std::cerr << "error initializing sensor interface\n";
    }

    /* Get initial status by reading totalPSU status */
    workablePSU = 0;
    previousWorkablePSU = totalPSU;
    for (int i = 0; i < totalPSU; i++)
    {
        std::variant<bool> value;
        std::string objPath = PSU_OBJ + std::to_string(i);
        auto method = static_cast<sdbusplus::bus::bus&>(*conn).new_method_call(
            PSU_SERVICE, objPath.c_str(), "org.freedesktop.DBus.Properties",
            "Get");

        method.append(OPERATIONAL_STATE_IFACE, "Functional");

        auto reply = static_cast<sdbusplus::bus::bus&>(*conn).call(method);

        if (reply.is_method_error())
        {
            std::cerr << "error\n";
        }

        reply.read(value);
        if (std::get<bool>(value))
        {
            workablePSU++;
        }
    }
    setStatus();

    auto psuEventMatcherCallback = [this,
                                    conn](sdbusplus::message::message& msg) {
        std::string objectName;
        boost::container::flat_map<std::string, std::variant<bool>> values;
        try
        {
            msg.read(objectName, values);
        }
        catch (const sdbusplus::exception_t& e)
        {
            std::cerr << "Failed to read message from PSU Event\n";
            return;
        }

        std::string psuEventName = "Functional";
        auto findEvent = values.find(psuEventName);
        if (findEvent != values.end())
        {
            bool* functional = std::get_if<bool>(&(findEvent->second));
            if (functional == nullptr)
            {
                std::cerr << "Unable to get valid functional status\n";
                return;
            }
            if (*functional)
            {
                workablePSU++;
            }
            else
            {
                workablePSU--;
            }
        }

        setStatus();
    };

    /* create properties changed signal handler for the status change*/
    psuEventMatcher = std::make_shared<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*conn),
        "type='signal',member='PropertiesChanged',path_namespace='" +
            std::string(PSU_BASE_OBJ) + "',arg0namespace='" +
            std::string(OPERATIONAL_STATE_IFACE) + "'",
        std::move(psuEventMatcherCallback));
}

PSURedundancy::~PSURedundancy()
{
    objServer.remove_interface(sensorInterface);
}

void PSURedundancy::setStatus()
{

    if (workablePSU > previousWorkablePSU)
    {
        if (workablePSU >= redundantPSU)
        {
            if (workablePSU == totalPSU)
            {
                // When all PSU are work correctly, it is full redundant - 00
                sensorInterface->set_property(
                    "Status", static_cast<std::string>("fullyRedundant"));
            }
            else if (previousWorkablePSU < redundantPSU)
            {
                // Not all PSU can work correctly but system still in
                // redundancy mode and previous status is non redundant - 07
                sensorInterface->set_property(
                    "Status", static_cast<std::string>("redundancyRegained"));
            }
        }
        else if (previousWorkablePSU == sufficientPSU)
        {
            // Now system is not in redundancy mode but still some PSU are
            // workable and previously there is no sufficient workable PSU in
            // the system - 04
            sensorInterface->set_property("RedundancyLost", false);
            sensorInterface->set_property(
                "Status", static_cast<std::string>("sufficient"));
        }
    }
    else if (workablePSU < previousWorkablePSU)
    {
        if (workablePSU >= redundantPSU)
        {
            // One PSU is now not workable, but other workable PSU can still
            // support redundancy mode - 02
            sensorInterface->set_property(
                "Status", static_cast<std::string>("redundancyDegraded"));

            if (previousWorkablePSU == totalPSU)
            {
                // One PSU become not workable and system was in full
                // redundancy mode - 06
                sensorInterface->set_property(
                    "Status",
                    static_cast<std::string>("redundancyDegradedFromFull"));
            }
        }
        else
        {
            if (previousWorkablePSU >= redundantPSU)
            {
                // No enough workable PSU to support redundancy and
                // previously system is in redundancy mode - 01
                sensorInterface->set_property("RedundancyLost", true);
                if (workablePSU > sufficientPSU)
                {
                    // There still some sufficient PSU, but system is not
                    // in redundancy mode - 03
                    sensorInterface->set_property(
                        "Status",
                        static_cast<std::string>("sufficientFromRedundant"));
                }
            }
            if (workablePSU == sufficientPSU)
            {
                // No sufficient PSU on the system - 05
                sensorInterface->set_property(
                    "Status", static_cast<std::string>("insufficient"));
            }
        }
    }
    previousWorkablePSU = workablePSU;
}
