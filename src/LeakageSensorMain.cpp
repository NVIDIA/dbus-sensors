#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/container/flat_set.hpp>

#include <sdbusplus/bus/match.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/container/flat_map.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>

#include "Utils.hpp"
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <variant>
#include <vector>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>

#include "LeakageSensor.hpp"
#include "VariantVisitors.hpp"

static constexpr float pollRateDefault = 0.5;
constexpr const bool debug = true;
constexpr const char* sensorType = "leakage";

void createSensors(
    boost::asio::io_context& io, sdbusplus::asio::object_server& objectServer,
    boost::container::flat_map<std::string, std::unique_ptr<LeakageSensor>>&
        sensors,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection)
{
    if (!dbusConnection)
    {
        std::cerr << "Connection not created\n";
        return;
    }

    dbusConnection->async_method_call(
        [&io, &objectServer, &dbusConnection, &sensors](
            boost::system::error_code ec, const ManagedObjectType& resp) {
        if (ec)
        {
            std::cerr << "Error contacting entity manager\n";
            return;
        }
        for (const auto& [path, interfaces] : resp)
        {
            for (const auto& [intf, cfg] : interfaces)
            {
                if (intf != configInterfaceName(sensorType))
                {
                    continue;
                }

                const std::string* interfacePath = nullptr;
                float pollRate = getPollRate(cfg, pollRateDefault);
                std::string sensorName = loadVariant<std::string>(cfg, "Name");
                uint8_t busId = loadVariant<uint8_t>(cfg, "Bus");
                uint8_t address = loadVariant<uint8_t>(cfg, "Address");
                bool polling = loadVariant<bool>(cfg, "Polling");
                std::string driver = loadVariant<std::string>(cfg, "Driver");

                if constexpr (debug)
                {
                    std::cout << "Configuration parsed for \n\t" << intf << "\n"
                              << "with\n"
                              << "\tName: " << sensorName << "\n"
                              << "\tBus: " << static_cast<int>(busId) << "\n"
                              << "\tAddress: " << static_cast<int>(address) << "\n"
                              << "\tPollRate: " << pollRate << "\n"
                              << "\tPolling: " << polling << "\n"
                              << "\tDriver: " << driver << "\n"
                              << "\n";
                }

                auto& sensor = sensors[sensorName];

                sensor = std::make_unique<LeakageSensor>(
                    objectServer, dbusConnection, io, sensorName,
                    *interfacePath, pollRate, busId, address, 
                    polling, driver);
            }
        }
    },
        entityManagerName, "/xyz/openbmc_project/inventory",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
}

int main()
{
    boost::asio::io_context io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::asio::object_server objectServer(systemBus, true);
    boost::container::flat_map<std::string, std::unique_ptr<LeakageSensor>>
        leakSensors;

    objectServer.add_manager("/xyz/openbmc_project/sensors");
    systemBus->request_name("xyz.openbmc_project.LeakageSensor");

    boost::asio::post(
        io, [&]() { createSensors(io, objectServer, leakSensors, systemBus); });

    boost::asio::steady_timer configTimer(io);

    std::function<void(sdbusplus::message_t&)> eventHandler =
        [&](sdbusplus::message_t&) {
        configTimer.expires_after(std::chrono::seconds(1));
        // create a timer because normally multiple properties change
        configTimer.async_wait([&](const boost::system::error_code& ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                return; // we're being canceled
            }
            // config timer error
            if (ec)
            {
                std::cerr << "timer error\n";
                return;
            }
            createSensors(io, objectServer, leakSensors, systemBus);
            if (leakSensors.empty())
            {
                std::cout << "Configuration not detected\n";
            }
        });
    };

    std::vector<std::unique_ptr<sdbusplus::bus::match_t>> matches =
        setupPropertiesChangedMatches(
            *systemBus, std::to_array<const char*>({sensorType}), eventHandler);
    setupManufacturingModeMatch(*systemBus);
    io.run();
    return 0;
}