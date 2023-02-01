#include <SELSensor.hpp>
#include <VariantVisitors.hpp>
#include <WatchdogSensor.hpp>
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

static constexpr auto sensorTypes{
    std::to_array<const char*>({"xyz.openbmc_project.Configuration.watchdog",
                                "xyz.openbmc_project.Configuration.SEL"})};

void createSensors(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    boost::container::flat_map<std::string, std::shared_ptr<WatchdogSensor>>&
        watchdogSensors,
    boost::container::flat_map<std::string, std::shared_ptr<SELSensor>>&
        selSensors,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const std::shared_ptr<boost::container::flat_set<std::string>>&
        sensorsChanged)
{
    auto getter = std::make_shared<GetSensorConfiguration>(
        dbusConnection,
        [&io, &objectServer, &watchdogSensors, &selSensors, &dbusConnection,
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
                    auto sensorBase = sensor.second.find(type);
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
                    std::cerr
                        << "error finding base configuration for sensor types"
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
                auto findWatchdogSensor = watchdogSensors.find(sensorName);
                if (!firstScan && findWatchdogSensor != watchdogSensors.end())
                {
                    bool found = false;
                    for (auto it = sensorsChanged->begin();
                         it != sensorsChanged->end(); it++)
                    {
                        if (findWatchdogSensor->second &&
                            boost::ends_with(*it,
                                             findWatchdogSensor->second->name))
                        {
                            sensorsChanged->erase(it);
                            findWatchdogSensor->second = nullptr;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        continue;
                    }
                }

                auto findSelSensor = selSensors.find(sensorName);
                if (!firstScan && findSelSensor != selSensors.end())
                {
                    bool found = false;
                    for (auto it = sensorsChanged->begin();
                         it != sensorsChanged->end(); it++)
                    {
                        if (findSelSensor->second &&
                            boost::ends_with(*it, findSelSensor->second->name))
                        {
                            sensorsChanged->erase(it);
                            findSelSensor->second = nullptr;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        continue;
                    }
                }

                if (sensor.second.find(
                        "xyz.openbmc_project.Configuration.watchdog") !=
                    sensor.second.end())
                {
                    auto& watchdogSensorConstruct = watchdogSensors[sensorName];
                    watchdogSensorConstruct = nullptr;

                    watchdogSensorConstruct = std::make_shared<WatchdogSensor>(
                        objectServer, dbusConnection, /*io,*/ sensorName,
                        *interfacePath);
                }
                else if (sensor.second.find(
                             "xyz.openbmc_project.Configuration.SEL") !=
                         sensor.second.end())
                {
                    auto& selSensorConstruct = selSensors[sensorName];
                    selSensorConstruct = nullptr;

                    selSensorConstruct = std::make_shared<SELSensor>(
                        objectServer, dbusConnection, /*io,*/ sensorName,
                        *interfacePath);
                }
            }
        });

    getter->getConfiguration(
        std::vector<std::string>{sensorTypes.begin(), sensorTypes.end()});
}

int main()
{
    boost::asio::io_service io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::asio::object_server objectServer(systemBus, true);
    objectServer.add_manager("/xyz/openbmc_project/sensors");

    systemBus->request_name("xyz.openbmc_project.SystemSensor");
    boost::container::flat_map<std::string, std::shared_ptr<WatchdogSensor>>
        watchdogSensors;
    boost::container::flat_map<std::string, std::shared_ptr<SELSensor>>
        selSensors;
    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> matches;
    auto sensorsChanged =
        std::make_shared<boost::container::flat_set<std::string>>();

    io.post([&]() {
        createSensors(io, objectServer, watchdogSensors, selSensors, systemBus,
                      nullptr);
    });

    boost::asio::deadline_timer filterTimer(io);
    std::function<void(sdbusplus::message::message&)> eventHandler =
        [&](sdbusplus::message::message& message) {
            if (message.is_method_error())
            {
                std::cerr << "callback method error\n";
                return;
            }
            sensorsChanged->insert(message.get_path());
            // this implicitly cancels the timer
            filterTimer.expires_from_now(boost::posix_time::seconds(1));

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
                createSensors(io, objectServer, watchdogSensors, selSensors,
                              systemBus, sensorsChanged);
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

    io.run();
}
