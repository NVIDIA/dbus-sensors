#include "PLXTempSensor.hpp"
#include "Utils.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <iostream>
static constexpr float pollRateDefault = 0.5;

namespace fs = std::filesystem;
static auto sensorTypes{
    std::to_array<const char*>({"xyz.openbmc_project.Configuration.PLX"})};

void createSensors(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    boost::container::flat_map<std::string, std::shared_ptr<PLXTempSensor>>&
        sensors,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const std::shared_ptr<boost::container::flat_set<std::string>>&
        sensorsChanged)
{
    auto getter = std::make_shared<GetSensorConfiguration>(
        dbusConnection,
        [&io, &objectServer, &sensors, &dbusConnection,
         sensorsChanged](const ManagedObjectType& sensorConfigurations) {
            bool firstScan = sensorsChanged == nullptr;

            const SensorData* sensorData = nullptr;
            const std::string* interfacePath = nullptr;
            const char* sensorType = nullptr;
            const SensorBaseConfiguration* baseConfiguration = nullptr;
            const SensorBaseConfigMap* baseConfigMap = nullptr;

            for (const std::pair<sdbusplus::message::object_path, SensorData>&
                     sensor : sensorConfigurations)
            {
                sensorData = &(sensor.second);
                for (const char* type : sensorTypes)
                {
                    auto sensorBase = sensorData->find(type);
                    if (sensorBase != sensorData->end())
                    {
                        baseConfiguration = &(*sensorBase);
                        sensorType = type;
                        break;
                    }
                }
                if (baseConfiguration == nullptr)
                {
                    std::cerr << "error finding base configuration "
                              << "\n";
                    continue;
                }
                baseConfigMap = &baseConfiguration->second;
                auto configurationBus = baseConfigMap->find("Bus");
                auto configurationAddress = baseConfigMap->find("Address");

                if (configurationBus == baseConfigMap->end() ||
                    configurationAddress == baseConfigMap->end())
                {
                    std::cerr << "error finding bus or address in "
                                 "configuration\n";
                    continue;
                }

                uint8_t deviceAddress =
                    std::visit(VariantToUnsignedIntVisitor(),
                               configurationAddress->second);
                uint8_t deviceBus = std::visit(VariantToUnsignedIntVisitor(),
                                               configurationBus->second);
                interfacePath = &(sensor.first.str);
                auto findSensorName = baseConfigMap->find("Name");
                if (findSensorName == baseConfigMap->end())
                {
                    std::cerr << "could not determine configuration name for "
                              << "\n";
                    continue;
                }
                std::string sensorName =
                    std::get<std::string>(findSensorName->second);
                // on rescans, only update sensors we were signaled by
                auto findSensor = sensors.find(sensorName);
                if (!firstScan && findSensor != sensors.end())
                {
                    bool found = false;
                    auto it = sensorsChanged->begin();
                    while (it != sensorsChanged->end())
                    {
                        if (boost::ends_with(*it, findSensor->second->name))
                        {
                            it = sensorsChanged->erase(it);
                            findSensor->second = nullptr;
                            found = true;
                            break;
                        }
                        ++it;
                    }

                    if (!found)
                    {
                        continue;
                    }
                }

                std::vector<thresholds::Threshold> sensorThresholds;
                if (!parseThresholdsFromConfig(*sensorData, sensorThresholds))
                {
                    std::cerr << "error populating thresholds for "
                              << sensorName << "\n";
                }

                auto findPowerOn = baseConfiguration->second.find("PowerState");
                PowerState readState = PowerState::always;
                if (findPowerOn != baseConfiguration->second.end())
                {
                    std::string powerState = std::visit(
                        VariantToStringVisitor(), findPowerOn->second);
                    setReadState(powerState, readState);
                }

                auto findPollRate = baseConfiguration->second.find("PollRate");
                float pollRate = pollRateDefault;
                if (findPollRate != baseConfiguration->second.end())
                {
                    pollRate = std::visit(VariantToFloatVisitor(),
                                          findPollRate->second);
                    if (pollRate <= 0.0f)
                    {
                        std::cerr << "polling time too short for " << sensorName
                                  << "\n";
                        pollRate = pollRateDefault; // polling time too short
                    }
                }
                auto& sensorEntry = sensors[sensorName];
                sensorEntry = nullptr;

                sensorEntry = std::make_shared<PLXTempSensor>(
                    sensorType, objectServer, dbusConnection, io, sensorName,
                    std::move(sensorThresholds), *interfacePath, readState,
                    deviceBus, deviceAddress, pollRate);
                sensorEntry->setupRead();
            }
        });
    getter->getConfiguration(
        std::vector<std::string>(sensorTypes.begin(), sensorTypes.end()));
}

void interfaceRemoved(
    sdbusplus::message::message& message,
    boost::container::flat_map<std::string, std::shared_ptr<PLXTempSensor>>&
        sensors)
{
    if (message.is_method_error())
    {
        std::cerr << "interfacesRemoved callback method error\n";
        return;
    }

    sdbusplus::message::object_path path;
    std::vector<std::string> interfaces;

    message.read(path, interfaces);

    // If the xyz.openbmc_project.Confguration.X interface was removed
    // for one or more sensors, delete those sensor objects.
    auto sensorIt = sensors.begin();
    while (sensorIt != sensors.end())
    {
        if ((sensorIt->second->configurationPath == path) &&
            (std::find(interfaces.begin(), interfaces.end(),
                       sensorIt->second->objectType) != interfaces.end()))
        {
            sensorIt = sensors.erase(sensorIt);
        }
        else
        {
            sensorIt++;
        }
    }
}

int main()
{
    boost::asio::io_service io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    systemBus->request_name("xyz.openbmc_project.PLXTempSensor");
    sdbusplus::asio::object_server objectServer(systemBus);
    boost::container::flat_map<std::string, std::shared_ptr<PLXTempSensor>>
        sensors;
    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> matches;
    auto sensorsChanged =
        std::make_shared<boost::container::flat_set<std::string>>();

    io.post([&]() {
        createSensors(io, objectServer, sensors, systemBus, nullptr);
    });

    boost::asio::steady_timer filterTimer(io);
    std::function<void(sdbusplus::message::message&)> eventHandler =
        [&](sdbusplus::message::message& message) {
            if (message.is_method_error())
            {
                std::cerr << "callback method error\n";
                return;
            }
            sensorsChanged->insert(message.get_path());
            // this implicitly cancels the timer
            filterTimer.expires_from_now(std::chrono::seconds(1));

            filterTimer.async_wait([&](const boost::system::error_code& ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    /* we were canceled*/
                    return;
                }
                if (ec)
                {
                    std::cerr << "timer error\n";
                    return;
                }
                createSensors(io, objectServer, sensors, systemBus,
                              sensorsChanged);
            });
        };

    for (const char* type : sensorTypes)
    {
        auto match = std::make_unique<sdbusplus::bus::match::match>(
            static_cast<sdbusplus::bus::bus&>(*systemBus),
            "type='signal',member='PropertiesChanged',path_namespace='" +
                std::string(inventoryPath) + "',arg0namespace='" + type + "'",
            eventHandler);
        matches.emplace_back(std::move(match));
    }

    // Watch for entity-manager to remove configuration interfaces
    // so the corresponding sensors can be removed.
    auto ifaceRemovedMatch = std::make_unique<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*systemBus),
        "type='signal',member='InterfacesRemoved',arg0path='" +
            std::string(inventoryPath) + "/'",
        [&sensors](sdbusplus::message::message& msg) {
            interfaceRemoved(msg, sensors);
        });

    matches.emplace_back(std::move(ifaceRemovedMatch));

    io.run();
}
