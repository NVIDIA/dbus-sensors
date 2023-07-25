#include <GPUStatus.hpp>
#include <VariantVisitors.hpp>
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
    std::to_array<const char*>({"xyz.openbmc_project.Configuration.rstgpu"})};

void createSensors(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    boost::container::flat_map<std::string, std::shared_ptr<GPUStatus>>&
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
                    std::cerr << "could not determine configuration name"
                              << "\n";
                    continue;
                }
                std::string sensorName =
                    std::get<std::string>(findSensorName->second);

                auto findTotalGPU = baseConfiguration->second.find("TotalGpu");
                if (findTotalGPU == baseConfiguration->second.end())
                {
                    std::cerr << "could not determine configuration totalGPU"
                              << "\n";
                    continue;
                }
                int totalGPU =
                    std::visit(VariantToIntVisitor(), findTotalGPU->second);

                auto findGPUService = baseConfiguration->second.find("Service");
                if (findGPUService == baseConfiguration->second.end())
                {
                    std::cerr << "could not determine D-bus service"
                              << "\n";
                    continue;
                }
                std::string gpuService =
                    std::get<std::string>(findGPUService->second);

                auto findGPUObject = baseConfiguration->second.find("Object");
                if (findGPUObject == baseConfiguration->second.end())
                {
                    std::cerr << "could not determine D-bus object"
                              << "\n";
                    continue;
                }
                std::string gpuObject =
                    std::get<std::string>(findGPUObject->second);

                auto findGPUInterface =
                    baseConfiguration->second.find("Interface");
                if (findGPUInterface == baseConfiguration->second.end())
                {
                    std::cerr
                        << "could not determine configuration D-bus interface"
                        << "\n";
                    continue;
                }
                std::string gpuInterface =
                    std::get<std::string>(findGPUInterface->second);

                auto findGPUProperty =
                    baseConfiguration->second.find("Property");
                if (findGPUProperty == baseConfiguration->second.end())
                {
                    std::cerr
                        << "could not determine configuration D-bus property"
                        << "\n";
                    continue;
                }
                std::string gpuProperty =
                    std::get<std::string>(findGPUProperty->second);

                // on rescans, only update sensors we were signaled by
                auto findSensor = sensors.find(sensorName);
                if (!firstScan && findSensor != sensors.end())
                {
                    bool found = false;
                    for (auto it = sensorsChanged->begin();
                         it != sensorsChanged->end(); it++)
                    {
                        if (findSensor->second &&
                            boost::ends_with(*it, findSensor->second->name))
                        {
                            sensorsChanged->erase(it);
                            findSensor->second = nullptr;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        continue;
                    }
                }

                auto& sensorConstruct = sensors[sensorName];
                sensorConstruct = nullptr;

                sensorConstruct = std::make_shared<GPUStatus>(
                    objectServer, dbusConnection, sensorName, gpuService,
                    gpuObject, gpuInterface, gpuProperty, totalGPU,
                    *interfacePath);
            }
        });

    getter->getConfiguration(
        std::vector<std::string>{sensorTypes.begin(), sensorTypes.end()});
}

int main()
{
    boost::asio::io_service io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    systemBus->request_name("xyz.openbmc_project.gpustatus");
    sdbusplus::asio::object_server objectServer(systemBus);
    boost::container::flat_map<std::string, std::shared_ptr<GPUStatus>> sensors;
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

    io.run();
}
