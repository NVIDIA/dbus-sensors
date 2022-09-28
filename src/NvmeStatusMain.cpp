#include <NvmeStatus.hpp>
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
    std::to_array<const char*>({"xyz.openbmc_project.Configuration.Nvmecpld"})};

void createSensors(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    boost::container::flat_map<std::string, std::shared_ptr<NvmeStatus>>&
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
                    std::cerr
                        << "could not determine configuration address for "
                        << "\n";
                    continue;
                }
                unsigned int address = std::visit(VariantToUnsignedIntVisitor(),
                                                  findAddress->second);

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
                    std::string nameWithIndex =
                        sensorName + "_" + std::to_string(nvmeIndex);
                    auto& sensorConstruct = sensors[nameWithIndex];
                    sensorConstruct = nullptr;

                    sensorConstruct = std::make_shared<NvmeStatus>(
                        objectServer, dbusConnection, io, nameWithIndex,
                        *interfacePath, nvmeIndex, busId, address, reg);
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
    systemBus->request_name("xyz.openbmc_project.NvmeStatus");
    sdbusplus::asio::object_server objectServer(systemBus);
    boost::container::flat_map<std::string, std::shared_ptr<NvmeStatus>>
        sensors;
    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> matches;
    auto sensorsChanged =
        std::make_shared<boost::container::flat_set<std::string>>();

    io.post([&]() {
        createSensors(io, objectServer, sensors, systemBus, nullptr);
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
                    std::cerr << "timer operation aborted\n";
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
