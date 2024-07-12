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
#include "DiscreteLeakDetectSensor.hpp"
#include "VariantVisitors.hpp"

namespace fs = std::filesystem;
static constexpr float pollRateDefault = 0.5;
constexpr const bool debug = true;
constexpr const char* sensorType = "leakage";
using sysfsAttributesVec = std::vector<std::pair<std::string, std::string>>;

void findMatchingSysfsAttributes(sysfsAttributesVec& matching_paths, const std::string& base_path, const std::string& dir_pattern, const std::string& file_pattern)
{
    for (const auto& entry : fs::recursive_directory_iterator(base_path)) {
        if (entry.is_directory() && entry.path().filename().string().find(dir_pattern) != std::string::npos) {
            for (const auto& sub_entry : fs::directory_iterator(entry.path())) {
                if (sub_entry.path().filename().string().find(file_pattern) == 0) {
                    matching_paths.emplace_back(entry.path().string(), sub_entry.path().filename().string());
                }
            }
        }
    }
}

void createSensors(
    sdbusplus::bus::bus& bus,
    boost::asio::io_context& io, sdbusplus::asio::object_server& objectServer,
    boost::container::flat_map<std::string, std::unique_ptr<DiscreteLeakDetectSensor>>&
        sensors,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection)
{
    if (!dbusConnection)
    {
        std::cerr << "Connection not created\n";
        return;
    }

    dbusConnection->async_method_call(
        [&bus, &io, &objectServer, &dbusConnection, &sensors](
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
                    std::cerr << "Cannot find config interface name\n" ;
                    continue;
                }

                const std::string* interfacePath = nullptr;
                float pollRate = getPollRate(cfg, pollRateDefault);
                uint8_t busId = loadVariant<uint8_t>(cfg, "Bus");
                uint8_t address = loadVariant<uint8_t>(cfg, "Address");
                std::string driver = loadVariant<std::string>(cfg, "Driver");
                std::string detectorType = loadVariant<std::string>(cfg, "DetectorType");

                if constexpr (debug)
                {
                    std::cout << "Configuration parsed for \n\t" << intf << "\n"
                              << "with\n"
                              << "\tBus: " << static_cast<int>(busId) << "\n"
                              << "\tAddress: " << static_cast<int>(address) << "\n"
                              << "\tPollRate: " << pollRate << "\n"
                              << "\tDriver: " << driver << "\n"
                              << "\tDetectorType: " << detectorType << "\n"
                              << "\n";
                }

                sdbusplus::server::manager::manager objManager(bus, "/");
                sysfsAttributesVec matching_paths;

                std::string base_path = "/sys/bus/i2c/devices/i2c-" + std::to_string(busId) + "/" +
                            std::to_string(busId) + "-00" + std::to_string(address) + "/" +
                            driver + "/hwmon";
                std::string dir_pattern = "hwmon";
                std::string file_pattern = "leakage";

                findMatchingSysfsAttributes(matching_paths,
                                base_path,
                                dir_pattern,
                                file_pattern);

                if (!matching_paths.empty()) {
                    std::cout << "Found matching sysfs paths:" << std::endl;
                    for (const auto& [dir, file] : matching_paths) {
                        std::cout << "Directory: " << dir << ", File: " << file << std::endl;
                        auto& sensor = sensors[file];
                        sensor = std::make_unique<DiscreteLeakDetectSensor>(bus,
                                        objectServer,
                                        dbusConnection,
                                        io,
                                        detectorType,
                                        dir,
                                        file,
                                        *interfacePath,
                                        pollRate,
                                        busId,
                                        address,
                                        driver);
                    }
                } else {
                    std::cout << "No matching sysfs paths found." << std::endl;
                }
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
    boost::container::flat_map<std::string, std::unique_ptr<DiscreteLeakDetectSensor>>
        leakSensors;
        
    objectServer.add_manager("/xyz/openbmc_project/sensors");
    systemBus->request_name("xyz.openbmc_project.DiscreteLeakDetectSensor");

    auto bus = sdbusplus::bus::new_default();

    boost::asio::post(
        io, [&]() { createSensors(bus, io, objectServer, leakSensors, systemBus); });

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
            createSensors(bus, io, objectServer, leakSensors, systemBus);
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