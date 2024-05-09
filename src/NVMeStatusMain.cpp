#include "NVMeMIStatus.hpp"
#include "NVMeStatus.hpp"
#include "VariantVisitors.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/container/flat_set.hpp>
#include <sdbusplus/bus/match.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <variant>
#include <vector>

static constexpr unsigned int pollRateDefault = 1;

static constexpr auto sensorTypes{
    std::to_array<const char*>({"Nvmecpld", "Nvmem2"})};

void createSensors(
    boost::asio::io_context& io, sdbusplus::asio::object_server& objectServer,
    boost::container::flat_map<std::string, std::shared_ptr<NVMeStatus>>&
        u2Sensors,
    boost::container::flat_map<std::string, std::shared_ptr<NVMeMIStatus>>&
        m2Sensors,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const std::shared_ptr<boost::container::flat_set<std::string>>&
        sensorsChanged)
{
    auto getter = std::make_shared<GetSensorConfiguration>(
        dbusConnection,
        [&io, &objectServer, &u2Sensors, &m2Sensors, &dbusConnection,
         sensorsChanged](const ManagedObjectType& sensorConfigurations) {
        bool firstScan = sensorsChanged == nullptr;
        const SensorData* sensorData = nullptr;
        const std::string* interfacePath = nullptr;
        const std::pair<std::string, boost::container::flat_map<
                                         std::string, BasicVariantType>>*
            baseConfiguration = nullptr;
        for (const std::pair<sdbusplus::message::object_path, SensorData>&
                 sensor : sensorConfigurations)
        {
            // clear it out each loop
            baseConfiguration = nullptr;

            // find base configuration
            for (const char* type : sensorTypes)
            {
                auto sensorBase = sensor.second.find(configInterfaceName(type));
                if (sensorBase != sensor.second.end())
                {
                    baseConfiguration = &(*sensorBase);
                    break;
                }
            }
            if (baseConfiguration == nullptr)
            {
                continue;
            }
            sensorData = &(sensor.second);
            interfacePath = &(sensor.first.str);

            if (sensorData == nullptr)
            {
                std::cerr << "failed to find sensor type"
                          << "\n";
                continue;
            }

            if (baseConfiguration == nullptr)
            {
                std::cerr << "error finding base configuration for sensor types"
                          << "\n";
                continue;
            }

            auto findSensorName = baseConfiguration->second.find("Name");
            if (findSensorName == baseConfiguration->second.end())
            {
                std::cerr << "could not determine configuration name for "
                          << "\n";
                continue;
            }
            std::string sensorName =
                std::get<std::string>(findSensorName->second);

            // on rescans, only update sensors we were signaled by
            auto findU2Sensor = u2Sensors.find(sensorName);
            if (!firstScan && findU2Sensor != u2Sensors.end())
            {
                bool found = false;
                for (auto it = sensorsChanged->begin();
                     it != sensorsChanged->end(); it++)
                {
                    if (findU2Sensor->second &&
                        boost::ends_with(*it, findU2Sensor->second->name))
                    {
                        sensorsChanged->erase(it);
                        findU2Sensor->second = nullptr;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    continue;
                }
            }

            auto findM2Sensor = m2Sensors.find(sensorName);
            if (!firstScan && findM2Sensor != m2Sensors.end())
            {
                bool found = false;
                for (auto it = sensorsChanged->begin();
                     it != sensorsChanged->end(); it++)
                {
                    if (findM2Sensor->second &&
                        boost::ends_with(*it, findM2Sensor->second->name))
                    {
                        sensorsChanged->erase(it);
                        findM2Sensor->second = nullptr;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    continue;
                }
            }

            auto findPollRate = baseConfiguration->second.find("PollRate");
            unsigned int pollRate = pollRateDefault;
            if (findPollRate != baseConfiguration->second.end())
            {
                pollRate = std::visit(VariantToUnsignedIntVisitor(),
                                      findPollRate->second);
            }

            auto findBusId = baseConfiguration->second.find("Bus");
            if (findBusId == baseConfiguration->second.end())
            {
                std::cerr << "could not determine configuration BusId for "
                          << "\n";
                continue;
            }

            unsigned int busId = std::visit(VariantToUnsignedIntVisitor(),
                                            findBusId->second);

            auto findAddress = baseConfiguration->second.find("Address");
            if (findAddress == baseConfiguration->second.end())
            {
                std::cerr << "could not determine configuration address for "
                          << "\n";
                continue;
            }
            unsigned int address = std::visit(VariantToUnsignedIntVisitor(),
                                              findAddress->second);

            if (sensor.second.find(
                    "xyz.openbmc_project.Configuration.Nvmecpld") !=
                sensor.second.end())
            {
                auto findRegister = baseConfiguration->second.find("Register");
                if (findRegister == baseConfiguration->second.end())
                {
                    std::cerr
                        << "could not determine configuration register for "
                        << "\n";
                    continue;
                }
                unsigned int reg = std::visit(VariantToUnsignedIntVisitor(),
                                              findRegister->second);

                for (uint8_t nvmeIndex = 0; nvmeIndex < 8; nvmeIndex++)
                {
                    std::string nameWithIndex = sensorName + "_" +
                                                std::to_string(nvmeIndex);
                    auto& u2SensorConstruct = u2Sensors[nameWithIndex];
                    u2SensorConstruct = nullptr;

                    u2SensorConstruct = std::make_shared<NVMeStatus>(
                        objectServer, dbusConnection, io, nameWithIndex,
                        *interfacePath, pollRate, nvmeIndex, busId, address,
                        reg);
                }
            }
            else if (sensor.second.find(
                         "xyz.openbmc_project.Configuration.Nvmem2") !=
                     sensor.second.end())
            {
                auto& m2SensorConstruct = m2Sensors[sensorName];
                m2SensorConstruct = nullptr;

                m2SensorConstruct = std::make_shared<NVMeMIStatus>(
                    objectServer, dbusConnection, io, sensorName,
                    *interfacePath, pollRate, busId, address);
            }
        }
    });

    getter->getConfiguration(
        std::vector<std::string>{sensorTypes.begin(), sensorTypes.end()});
}

int main()
{
    boost::asio::io_context io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::asio::object_server objectServer(systemBus, true);
    objectServer.add_manager("/xyz/openbmc_project/sensors");
    systemBus->request_name("xyz.openbmc_project.NvmeStatus");
    boost::container::flat_map<std::string, std::shared_ptr<NVMeStatus>>
        u2Sensors;
    boost::container::flat_map<std::string, std::shared_ptr<NVMeMIStatus>>
        m2Sensors;
    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> matches;
    auto sensorsChanged =
        std::make_shared<boost::container::flat_set<std::string>>();

    boost::asio::post(io, [&]() {
        createSensors(io, objectServer, u2Sensors, m2Sensors, systemBus,
                      nullptr);
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
        filterTimer.expires_after(std::chrono::seconds(1));

        filterTimer.async_wait([&](const boost::system::error_code& ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                /* we were canceled*/
                std::cerr << "timer operation aborted\n";
                return;
            }
            if (ec)
            {
                std::cerr << "timer error\n";
                return;
            }
            createSensors(io, objectServer, u2Sensors, m2Sensors, systemBus,
                          sensorsChanged);
        });
    };

    for (const char* type : sensorTypes)
    {
        auto match = std::make_unique<sdbusplus::bus::match::match>(
            static_cast<sdbusplus::bus::bus&>(*systemBus),
            "type='signal',member='PropertiesChanged',path_namespace='" +
                std::string(inventoryPath) + "',arg0namespace='" +
                configInterfaceName(type) + "'",
            eventHandler);
        matches.emplace_back(std::move(match));
    }

    io.run();
}
