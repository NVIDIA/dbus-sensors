#include "MCTPEndpoint.hpp"
#include "MCTPReactor.hpp"
#include "Utils.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <phosphor-logging/device_error_log.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

extern std::set<uint8_t> suppressedHealthCheckEids;

// MCTP Control Message Type
enum
{
    MCTP_CTRL_HDR_MSG_TYPE = 0x00
};

// MCTP Control Command Codes (from DSP0236)
enum
{
    MCTP_CTRL_CMD_SET_ENDPOINT_ID = 0x01,
    MCTP_CTRL_CMD_GET_ENDPOINT_ID = 0x02,
    MCTP_CTRL_CMD_GET_ENDPOINT_UUID = 0x03,
    MCTP_CTRL_CMD_ALLOCATE_ENDPOINT_IDS = 0x08
};

// MCTP Direction value
enum
{
    MCTP_DIR_TX = 0,
    MCTP_DIR_RX = 1
};

PHOSPHOR_LOG2_USING;

// Use the nv::lg2 namespace for error logging
using nv::lg2::CommitDeviceError;
using nv::lg2::ErrorClass;

class DBusAssociationServer : public AssociationServer
{
  public:
    DBusAssociationServer() = delete;
    DBusAssociationServer(const DBusAssociationServer&) = delete;
    DBusAssociationServer(DBusAssociationServer&&) = delete;
    explicit DBusAssociationServer(
        const std::shared_ptr<sdbusplus::asio::connection>& connection) :
        server(connection)
    {
        server.add_manager("/au/com/codeconstruct/mctp1");
    }
    ~DBusAssociationServer() override = default;
    DBusAssociationServer& operator=(const DBusAssociationServer&) = delete;
    DBusAssociationServer& operator=(DBusAssociationServer&&) = delete;

    void associate(const std::string& path,
                   const std::vector<Association>& associations) override
    {
        auto [entry, _] = objects.emplace(
            path, server.add_interface(path, association::interface));
        std::shared_ptr<sdbusplus::asio::dbus_interface> iface = entry->second;
        iface->register_property("Associations", associations);
        iface->initialize();
    }

    void disassociate(const std::string& path) override
    {
        const auto entry = objects.find(path);
        if (entry == objects.end())
        {
            throw std::logic_error(std::format(
                "Attempted to untrack path that was not tracked: {}", path));
        }
        std::shared_ptr<sdbusplus::asio::dbus_interface> iface = entry->second;
        server.remove_interface(entry->second);
        objects.erase(entry);
    }

  private:
    std::shared_ptr<sdbusplus::asio::connection> connection;
    sdbusplus::asio::object_server server;
    std::map<std::string, std::shared_ptr<sdbusplus::asio::dbus_interface>>
        objects;
};

static std::shared_ptr<MCTPDevice> deviceFromConfig(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    const SensorData& config)
{
    try
    {
        std::optional<SensorBaseConfigMap> iface;
        iface = I2CMCTPDDevice::match(config);
        if (iface)
        {
            info("Creating I2CMCTPDDevice");
            return I2CMCTPDDevice::from(connection, *iface);
        }

        iface = I3CMCTPDDevice::match(config);
        if (iface)
        {
            info("Creating I3CMCTPDDevice");
            return I3CMCTPDDevice::from(connection, *iface);
        }

        iface = USBMCTPDDevice::match(config);
        if (iface)
        {
            return USBMCTPDDevice::from(connection, *iface);
        }

        iface = SPIMCTPDDevice::match(config);
        if (iface)
        {
            return SPIMCTPDDevice::from(connection, *iface);
        }

        iface = I2CMCTPDDevice::match(config);
        if (iface)
        {
            info("Creating I2CMCTPDDevice");
            return I2CMCTPDDevice::from(connection, *iface);
        }

        iface = I3CMCTPDDevice::match(config);
        if (iface)
        {
            info("Creating I3CMCTPDDevice");
            return I3CMCTPDDevice::from(connection, *iface);
        }

        iface = XROTMCTPDDevice::match(config);
        if (iface)
        {
            return XROTMCTPDDevice::from(connection, *iface);
        }
    }
    catch (const std::invalid_argument& ex)
    {
        error("Unable to create device: {EXCEPTION}", "EXCEPTION", ex);
    }

    return {};
}

static void addInventory(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    const std::shared_ptr<MCTPReactor>& reactor, sdbusplus::message_t& msg)
{
    auto [path,
          exposed] = msg.unpack<sdbusplus::message::object_path, SensorData>();
    try
    {
        reactor->manageMCTPDevice(path, deviceFromConfig(connection, exposed));
    }
    catch (const std::logic_error& e)
    {
        error(
            "Addition of inventory at '{INVENTORY_PATH}' caused an invalid program state: {EXCEPTION}",
            "INVENTORY_PATH", path, "EXCEPTION", e);
    }
    catch (const std::system_error& e)
    {
        error(
            "Failed to manage device described by inventory at '{INVENTORY_PATH}: {EXCEPTION}'",
            "INVENTORY_PATH", path, "EXCEPTION", e);
    }
}

static void removeInventory(const std::shared_ptr<MCTPReactor>& reactor,
                            sdbusplus::message_t& msg)
{
    auto [path, removed] =
        msg.unpack<sdbusplus::message::object_path, std::set<std::string>>();
    try
    {
        if (I2CMCTPDDevice::match(removed) || I3CMCTPDDevice::match(removed) ||
            USBMCTPDDevice::match(removed) || SPIMCTPDDevice::match(removed) ||
            XROTMCTPDDevice::match(removed))
        {
            reactor->unmanageMCTPDevice(path.str);
        }
    }
    catch (const std::logic_error& e)
    {
        error(
            "Removal of inventory at '{INVENTORY_PATH}' caused an invalid program state: {EXCEPTION}",
            "INVENTORY_PATH", path, "EXCEPTION", e);
    }
    catch (const std::system_error& e)
    {
        error(
            "Failed to unmanage device described by inventory at '{INVENTORY_PATH}: {EXCEPTION}'",
            "INVENTORY_PATH", path, "EXCEPTION", e);
    }
}

static void manageMCTPEntity(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    const std::shared_ptr<MCTPReactor>& reactor, ManagedObjectType& entities)
{
    for (const auto& [path, config] : entities)
    {
        try
        {
            reactor->manageMCTPDevice(path,
                                      deviceFromConfig(connection, config));
        }
        catch (const std::logic_error& e)
        {
            error(
                "Addition of inventory at '{INVENTORY_PATH}' caused an invalid program state: {EXCEPTION}",
                "INVENTORY_PATH", path, "EXCEPTION", e);
        }
        catch (const std::system_error& e)
        {
            error(
                "Failed to manage device described by inventory at '{INVENTORY_PATH}: {EXCEPTION}'",
                "INVENTORY_PATH", path, "EXCEPTION", e);
        }
    }
}

static void exitReactor(boost::asio::io_context* io, sdbusplus::message_t& msg)
{
    auto name = msg.unpack<std::string>();
    info("Shutting down mctpreactor, lost dependency '{SERVICE_NAME}'",
         "SERVICE_NAME", name);
    io->stop();
}

static void logMCTPError(const std::shared_ptr<MCTPReactor>& reactor,
                         uint8_t destEid, int errorCode,
                         const std::string& errorMessage)
{
    auto deviceNameOpt = reactor->getDeviceName(destEid);
    std::string deviceName =
        deviceNameOpt.value_or("EID_" + std::to_string(destEid));
    if (deviceName.empty())
    {
        deviceName = "EID_" + std::to_string(destEid);
    }

    std::string resolution =
        "If problem persists, perform power cycle of the system to recover the device.";

    std::map<std::string, std::string> additionalData = {
        {"REDFISH_MESSAGE_ID", "ResourceEvent.1.0.ResourceErrorsDetected"},
        {"REDFISH_MESSAGE_ARGS", deviceName + ", " + errorMessage},
        {"REDFISH_RESOLUTION", resolution},
        {"REDFISH_SEVERITY", "Critical"},
        {"REDFISH_ORIGIN_OF_CONDITION", deviceName}};

    CommitDeviceError(destEid, errorCode, ErrorClass::MCTP, additionalData);
}

int main()
{
    constexpr std::chrono::seconds period(5);

    boost::asio::io_context io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    DBusAssociationServer associationServer(systemBus);
    auto reactor = std::make_shared<MCTPReactor>(associationServer);
    boost::asio::steady_timer clock(io);

    std::function<void(const boost::system::error_code&)> alarm =
        [&](const boost::system::error_code& ec) {
            if (ec)
            {
                return;
            }
            clock.expires_after(period);
            clock.async_wait(alarm);
            reactor->tick();
        };
    clock.expires_after(period);
    clock.async_wait(alarm);

    using namespace sdbusplus::bus::match;

    const std::string entityManagerNameLostSpec =
        rules::nameOwnerChanged("xyz.openbmc_project.EntityManager");

    auto entityManagerNameLostMatch = sdbusplus::bus::match_t(
        static_cast<sdbusplus::bus_t&>(*systemBus), entityManagerNameLostSpec,
        std::bind_front(exitReactor, &io));

    const std::string mctpdNameLostSpec =
        rules::nameOwnerChanged("au.com.codeconstruct.MCTP1");

    auto mctpdNameLostMatch = sdbusplus::bus::match_t(
        static_cast<sdbusplus::bus_t&>(*systemBus), mctpdNameLostSpec,
        std::bind_front(exitReactor, &io));

    const std::string interfacesRemovedMatchSpec =
        rules::sender("xyz.openbmc_project.EntityManager") +
        // Trailing slash on path: Listen for signals on the inventory subtree
        rules::interfacesRemovedAtPath("/xyz/openbmc_project/inventory/");

    auto interfacesRemovedMatch = sdbusplus::bus::match_t(
        static_cast<sdbusplus::bus_t&>(*systemBus), interfacesRemovedMatchSpec,
        std::bind_front(removeInventory, reactor));

    const std::string interfacesAddedMatchSpec =
        rules::sender("xyz.openbmc_project.EntityManager") +
        // Trailing slash on path: Listen for signals on the inventory subtree
        rules::interfacesAddedAtPath("/xyz/openbmc_project/inventory/");

    auto interfacesAddedMatch = sdbusplus::bus::match_t(
        static_cast<sdbusplus::bus_t&>(*systemBus), interfacesAddedMatchSpec,
        std::bind_front(addInventory, systemBus, reactor));

    const std::string transportErrorMatchSpec =
        rules::type::signal() + rules::sender("au.com.codeconstruct.MCTP1") +
        rules::interface("au.com.codeconstruct.MCTP.BusOwner1") +
        rules::member("TransportError");

    auto transportErrorMatch = sdbusplus::bus::match_t(
        static_cast<sdbusplus::bus_t&>(*systemBus), transportErrorMatchSpec,
        [reactor](sdbusplus::message_t& msg) {
            uint32_t errorCode = 0;
            uint8_t direction = 0;
            uint8_t binding = 0;
            uint8_t srcEid = 0;
            uint8_t destEid = 0;
            uint8_t tag = 0;
            uint8_t msgType = 0;
            uint8_t commandCode = 0;
            std::string interface;

            msg.read(errorCode, direction, binding, srcEid, destEid, tag,
                     msgType, commandCode, interface);

            if (destEid == 0)
            {
                auto staticEid = reactor->getStaticEidFromInterface(interface);
                if (staticEid)
                {
                    destEid = *staticEid;
                }
                // Supress the 5sec SETEID timeout error logs from flooding when
                // the tick is running and trying to setup a failing device.
                if (reactor->isRetrying())
                {
                    return;
                }
            }

            // Suppress errors during recovery
            if ((suppressedHealthCheckEids.contains(destEid) ||
                 !suppressedHealthCheckEids.empty()) &&
                errorCode == ETIMEDOUT && direction == MCTP_DIR_RX)
            {
                return;
            }

            if (errorCode == ETIMEDOUT && direction == MCTP_DIR_RX)
            {
                if (msgType == MCTP_CTRL_HDR_MSG_TYPE)
                {
                    std::string timeoutType;
                    std::string errorMessage;

                    // Map command codes to timeout types and error messages
                    switch (commandCode)
                    {
                        case MCTP_CTRL_CMD_SET_ENDPOINT_ID:
                            timeoutType = "SetEID Timeout";
                            errorMessage =
                                "MCTP device discovery failed due to device error SetEID Timeout";
                            break;
                        case MCTP_CTRL_CMD_GET_ENDPOINT_UUID:
                            timeoutType = "Get UUID Timeout";
                            errorMessage =
                                "MCTP device discovery failed due to device error Get UUID Timeout";
                            break;
                        case MCTP_CTRL_CMD_ALLOCATE_ENDPOINT_IDS:
                            timeoutType = "Allocate EID Timeout";
                            errorMessage =
                                "MCTP device discovery failed due to device error Allocate EID Timeout";
                            break;
                        default:
                            timeoutType = "MCTP Control Timeout";
                            errorMessage =
                                "MCTP communication failed due to timeout";
                            break;
                    }

                    info("MCTP {TYPE} on EID {EID}", "TYPE", timeoutType, "EID",
                         destEid);
                    logMCTPError(reactor, destEid, errorCode, errorMessage);
                }
            }
            else if (direction == MCTP_DIR_TX)
            {
                info("MCTP TX error {ERROR} on EID {EID}", "ERROR", errorCode,
                     "EID", destEid);

                std::string errorMessage =
                    "MCTP communication failed due to Tx error (errno=" +
                    std::to_string(errorCode) + ")";

                logMCTPError(reactor, destEid, errorCode, errorMessage);
            }
        });

    systemBus->request_name("xyz.openbmc_project.MCTPReactor");

    boost::asio::post(io, [reactor, systemBus]() {
        auto gsc = std::make_shared<GetSensorConfiguration>(
            systemBus, std::bind_front(manageMCTPEntity, systemBus, reactor));
        gsc->getConfiguration({"MCTPI2CTarget", "MCTPI3CTarget"});
    });

    boost::asio::post(io, [reactor, systemBus]() {
        auto gsc = std::make_shared<GetSensorConfiguration>(
            systemBus, std::bind_front(manageMCTPEntity, systemBus, reactor));
        gsc->getConfiguration({"MCTPUSBTarget"});
    });

    boost::asio::post(io, [reactor, systemBus]() {
        auto gsc = std::make_shared<GetSensorConfiguration>(
            systemBus, std::bind_front(manageMCTPEntity, systemBus, reactor));
        gsc->getConfiguration({"MCTPSPIDevice"});
    });

    boost::asio::post(io, [reactor, systemBus]() {
        auto gsc = std::make_shared<GetSensorConfiguration>(
            systemBus, std::bind_front(manageMCTPEntity, systemBus, reactor));
        gsc->getConfiguration({"MCTPXROTTarget"});
    });

    io.run();

    return EXIT_SUCCESS;
}
