/**
 * Copyright (c) 2024, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#include "DiscreteLeakDetectSensor.hpp"


namespace fs = std::filesystem;
static constexpr float pollRateDefault = 0.5;
constexpr const bool debug = true;
constexpr const char* sensorType = "leakage";
using sysfsAttributesVec = std::vector<std::pair<std::string, std::string>>;

static void findMatchingSysfsAttributes(sysfsAttributesVec& matchingPaths, const std::string& basePath, const std::string& dirPattern, const std::string& filePattern)
{
    for (const auto& entry : fs::recursive_directory_iterator(basePath)) {
        if (entry.is_directory() && entry.path().filename().string().find(dirPattern) != std::string::npos) {
            for (const auto& subEntry : fs::directory_iterator(entry.path())) {
                if (subEntry.path().filename().string().find(filePattern) == 0) {
                    matchingPaths.emplace_back(entry.path().string(), subEntry.path().filename().string());
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
                sysfsAttributesVec matchingPaths;

                std::string basePath = "/sys/bus/i2c/devices/i2c-" + std::to_string(busId) + "/" +
                            std::to_string(busId) + "-00" + std::to_string(address) + "/" +
                            driver + "/hwmon";
                std::string dirPattern = "hwmon";
                std::string filePattern = "leakage";

                findMatchingSysfsAttributes(matchingPaths,
                                basePath,
                                dirPattern,
                                filePattern);

                if (!matchingPaths.empty()) {
                    std::cout << "Found matching sysfs paths:" << std::endl;
                    for (const auto& [dir, file] : matchingPaths) {
                        std::cout << "Directory: " << dir << ", File: " << file << std::endl;
                        auto& sensor = sensors[file];
                        sensor = std::make_unique<DiscreteLeakDetectSensor>(bus,
                                        objectServer,
                                        dbusConnection,
                                        io,
                                        detectorType,
                                        dir,
                                        file,
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