#include <IpmbStatus.hpp>
#include <Utils.hpp>
#include <VariantVisitors.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/container/flat_map.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>

constexpr const bool debug = false;

constexpr const char* configInterface =
    "xyz.openbmc_project.Configuration.Ipmbstatus";

static constexpr uint8_t meAddressDefault = 1;
static constexpr uint8_t lun = 0;
static constexpr float pollRateDefault = 1; // in seconds
static constexpr uint8_t cableStatusBit = 0;
static constexpr uint8_t configurationErrorBit = 1;

boost::asio::io_service io;
auto conn = std::make_shared<sdbusplus::asio::connection>(io);

using IpmbMethodType =
    std::tuple<int, uint8_t, uint8_t, uint8_t, uint8_t, std::vector<uint8_t>>;

boost::container::flat_map<std::string, std::shared_ptr<IpmbSensor>> sensors;

std::unique_ptr<boost::asio::steady_timer> initCmdTimer;

IpmbSensor::IpmbSensor(std::shared_ptr<sdbusplus::asio::connection>& conn,
                       boost::asio::io_service& io,
                       const std::string& sensorName,
                       const std::string& sensorConfiguration,
                       sdbusplus::asio::object_server& objectServer,
                       uint8_t deviceAddress, uint8_t channelAddress,
                       const float pollRate) :
    deviceAddress(deviceAddress),
    channelAddress(channelAddress),
    sensorPollMs(static_cast<int>(pollRate * 1000)), dbusConnection(conn),
    objectServer(objectServer), waitTimer(io)
{
    sensorInterface = objectServer.add_interface(
        "/xyz/openbmc_project/sensors/cable/" + sensorName,
        "xyz.openbmc_project.Inventory.Item.Cable");

    association = objectServer.add_interface(
        "/xyz/openbmc_project/sensors/cable/" + sensorName,
        association::interface);

    createAssociation(association, sensorConfiguration);
    sensorInterface->register_property("CableStatus", false);
    sensorInterface->register_property("ConfigurationError", false);

    if (!sensorInterface->initialize())
    {
        std::cerr << "error initializing value interface\n";
    }
}

IpmbSensor::~IpmbSensor()
{
    waitTimer.cancel();
    objectServer.remove_interface(sensorInterface);
    objectServer.remove_interface(association);
}

void IpmbSensor::init(void)
{
    loadDefaults();
    if (initCommand)
    {
        runInitCmd();
    }
    read();
}

void IpmbSensor::runInitCmd()
{
    if (initCommand)
    {
        // setup connection to dbus
        conn->async_method_call(
            [this](boost::system::error_code ec,
                   const IpmbMethodType& response) {
                const int& status = std::get<0>(response);

                if (ec || status)
                {
                    std::cerr << "Error setting init command for device: "
                              << "\n";
                }
            },
            "xyz.openbmc_project.Ipmi.Channel.Ipmb",
            "/xyz/openbmc_project/Ipmi/Channel/Ipmb", "org.openbmc.Ipmb",
            "sendRequest", commandAddress, netfn, lun, *initCommand, initData);
    }
}

void IpmbSensor::loadDefaults()
{
    if (type == IpmbType::meSensor)
    {
        commandAddress = channelAddress;
        netfn = ipmi::sensor::netFn;
        command = ipmi::sensor::getSensorReading;
        commandData = {deviceAddress};
    }
}

bool isValid(const std::vector<uint8_t>& data)
{
    constexpr auto readingUnavailableBit = 5;

    // Proper 'Get Sensor Reading' response has at least 4 bytes, including
    // Completion Code. Our IPMB stack strips Completion Code from payload so we
    // compare here against the rest of payload
    if (data.size() < 3)
    {
        return false;
    }

    // Per IPMI 'Get Sensor Reading' specification
    if (data[1] & (1 << readingUnavailableBit))
    {
        return false;
    }

    return true;
}

bool IpmbSensor::processReading(const std::vector<uint8_t>& data, double& resp)
{

    if (command == ipmi::sensor::getSensorReading && !isValid(data))
    {
        return false;
    }
    resp = data[0];
    return true;
}

void IpmbSensor::read(void)
{
    
    waitTimer.expires_from_now(std::chrono::milliseconds(sensorPollMs));
    waitTimer.async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            return; // we're being canceled
        }
        // setup connection to dbus
        conn->async_method_call(
            [this](boost::system::error_code ec,
                   const IpmbMethodType& response) {
                const int& status = std::get<0>(response);
                if (ec || status)
                {
                    read();
                    return;
                }
                const std::vector<uint8_t>& data = std::get<5>(response);
                if constexpr (debug)
                {
                    for (size_t d : data)
                    {
                        std::cout << d << " ";
                    }
                    std::cout << "\n";
                }
                if (data.empty())
                {
                    read();
                    return;
                }

                double value = 0;

                if (!processReading(data, value))
                {
                    read();
                    return;
                }
                // Per IPMI 'Get Sensor Reading' specification , 3th byte
                // discrete reading sensor
                sensorInterface->set_property(
                    "CableStatus",  static_cast<bool>(data[2] & (1 << cableStatusBit)));
                sensorInterface->set_property(
                    "ConfigurationError",
                     static_cast<bool>(data[2] & (1 << configurationErrorBit)));
                if (sensorMaskEnable){
                   bool cableMsgSent = false;
                   bool cableStatusMsg= false;
                    if (static_cast<bool>(data[2] & (1 << cableStatusBit)) && sensorReport){
                        std::cerr << "Sensor " << statusSensorName << " is enabled"<< "\n";
                        sensorReport = false;
                        cableStatusMsg = true;
                        cableMsgSent = true;
                    }
                    if (static_cast<bool>(data[2] & (1 << configurationErrorBit)) && !sensorReport){
                        std::cerr << "Sensor " << statusSensorName << " is in error"<< "\n";
                        sensorReport = true;
                        cableMsgSent = true;
                    }
                    if (cableMsgSent){
                        try
                            {
                                sdbusplus::message::message msg =
                                   sensorInterface->new_signal("CableStatus");
                                msg.append(statusSensorName, sensorInterface->get_interface_name(),cableMsgSent,cableStatusMsg);
                                msg.signal_send();
                            }
                            catch (const sdbusplus::exception::exception& e)
                            {
                                std::cerr
                                    << "Failed to send thresholdAsserted signal with assertValue\n";
                            }
                    }
                }
                
                if constexpr (debug)
                {
                    std::cout << value;
                }
                read();
            },
            "xyz.openbmc_project.Ipmi.Channel.Ipmb",
            "/xyz/openbmc_project/Ipmi/Channel/Ipmb", "org.openbmc.Ipmb",
            "sendRequest", commandAddress, netfn, lun, command, commandData);
    });
}
void createSensors(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    boost::container::flat_map<std::string, std::shared_ptr<IpmbSensor>>&
        sensors,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection)
{
    if (!dbusConnection)
    {
        std::cerr << "Connection not created\n";
        return;
    }
    dbusConnection->async_method_call(
        [&](boost::system::error_code ec, const ManagedObjectType& resp) {
            if (ec)
            {
                std::cerr << "Error contacting entity manager\n";
                return;
            }
            for (const auto& pathPair : resp)
            {
                for (const auto& entry : pathPair.second)
                {
                    if (entry.first != configInterface)
                    {
                        continue;
                    }
                    std::string name =
                        loadVariant<std::string>(entry.second, "Name");

                    uint8_t deviceAddress =
                        loadVariant<uint8_t>(entry.second, "Address");

                    std::string sensorClass =
                        loadVariant<std::string>(entry.second, "Class");

                    uint8_t channelAddress = meAddressDefault;
                    auto findmType = entry.second.find("ChannelAddress");
                    if (findmType != entry.second.end())
                    {
                        channelAddress = std::visit(
                            VariantToUnsignedIntVisitor(), findmType->second);
                    }

                    float pollRate = pollRateDefault;
                    auto findPollRate = entry.second.find("PollRate");
                    if (findPollRate != entry.second.end())
                    {
                        pollRate = std::visit(VariantToFloatVisitor(),
                                              findPollRate->second);
                        if (pollRate <= 0.0f)
                        {
                            pollRate = pollRateDefault;
                        }
                    }

                    auto& sensor = sensors[name];
                    sensor = nullptr;
                    sensor = std:: 
                        make_shared<IpmbSensor>(dbusConnection, io, name,
                                                pathPair.first, objectServer,
                                                deviceAddress, channelAddress,
                                                pollRate);

                    if (sensorClass == "METemp" || sensorClass == "MESensor" ||
                        sensorClass == "MECable")
                    {
                        sensor->type = IpmbType::meSensor;
                    }
                    else
                    {
                        std::cerr << "Invalid class " << sensorClass << "\n";
                        continue;
                    }
                                    
				    auto findmMask = entry.second.find("MaskEnable");
                    if (findmMask != entry.second.end())
                    {
                        std::string maskEnableStatus= loadVariant<std::string>(entry.second, "MaskEnable");
                        if (maskEnableStatus == "True"){
                            sensor->sensorMaskEnable = true;
                        }
                    }
                    
                    sensor->statusSensorName = name;

                    sensor->init();
                }
            }
        },
        entityManagerName, "/xyz/openbmc_project/inventory",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
}

void reinitSensors(sdbusplus::message::message& message)
{
    constexpr const size_t reinitWaitSeconds = 2;
    std::string objectName;
    boost::container::flat_map<std::string, std::variant<std::string>> values;
    message.read(objectName, values);

    auto findStatus = values.find(power::property);
    if (findStatus != values.end())
    {
        bool powerStatus = boost::ends_with(
            std::get<std::string>(findStatus->second), ".Running");
        if (powerStatus)
        {
            if (!initCmdTimer)
            {
                // this should be impossible
                return;
            }
            // we seem to send this command too fast sometimes, wait before
            // sending
            initCmdTimer->expires_from_now(
                std::chrono::seconds(reinitWaitSeconds));

            initCmdTimer->async_wait([](const boost::system::error_code ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    return; // we're being canceled
                }

                for (const auto& sensor : sensors)
                {
                    if (sensor.second)
                    {
                        sensor.second->runInitCmd();
                    }
                }
            });
        }
    }
}
int main()
{
    boost::asio::io_service io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::asio::object_server objectServer(systemBus, true);
    objectServer.add_manager("/xyz/openbmc_project/sensors");
    systemBus->request_name("xyz.openbmc_project.IpmbStatus");

    initCmdTimer = std::make_unique<boost::asio::steady_timer>(io);

    io.post([&]() { createSensors(io, objectServer, sensors, systemBus); });

    boost::asio::steady_timer configTimer(io);

    std::function<void(sdbusplus::message::message&)> eventHandler =
        [&](sdbusplus::message::message&) {
            configTimer.expires_from_now(std::chrono::seconds(1));
            // create a timer because normally multiple properties change
            configTimer.async_wait([&](const boost::system::error_code& ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    return; // we're being canceled
                }
                createSensors(io, objectServer, sensors, systemBus);
                if (sensors.empty())
                {
                    std::cout << "Configuration not detected\n";
                }
            });
        };

    sdbusplus::bus::match::match configMatch(
        static_cast<sdbusplus::bus::bus&>(*systemBus),
        "type='signal',member='PropertiesChanged',path_namespace='" +
            std::string(inventoryPath) + "',arg0namespace='" + configInterface +
            "'",
        eventHandler);

    sdbusplus::bus::match::match powerChangeMatch(
        static_cast<sdbusplus::bus::bus&>(*systemBus),
        "type='signal',interface='" + std::string(properties::interface) +
            "',path='" + std::string(power::path) + "',arg0='" +
            std::string(power::interface) + "'",
        reinitSensors);

    setupManufacturingModeMatch(*systemBus);
    io.run();
    return 0;
}
