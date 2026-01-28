#include "MCTPCustomDevices.hpp"
#include "MCTPEndpoint.hpp"
#include "MCTPEndpointUtils.hpp"
#include "MCTPReactor.hpp"
#include "Utils.hpp"

#include <sys/utsname.h>

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
#include <filesystem>
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

PHOSPHOR_LOG2_USING;

// Use the nv::lg2 namespace for error logging
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
            warning("Attempted to untrack path that was not tracked: {PATH}",
                    "PATH", path);
            return;
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

        iface = USBGadgetMCTPDevice::match(config);
        if (iface)
        {
            return USBGadgetMCTPDevice::from(connection, *iface);
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
            XROTMCTPDDevice::match(removed) ||
            USBGadgetMCTPDevice::match(removed))
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

static void handleApplicationTimeout(
    const std::shared_ptr<MCTPReactor>& reactor,
    const TransportErrorInfo& error)
{
    if (mctpCommandTable.contains(error.commandCode))
    {
        std::string errorMessage =
            mctpCommandTable.at(error.commandCode).timeoutErrorMessage;
        std::string logMessage =
            mctpCommandTable.at(error.commandCode).logMessage;

        debug("{MSG} {EID}", "MSG", logMessage, "EID", error.destEid);

        auto deviceNameOpt = reactor->getDeviceName(error.destEid);
        std::string deviceName =
            deviceNameOpt.value_or("EID_" + std::to_string(error.destEid));

        logMCTPError(deviceName, error.destEid, error.errorCode, errorMessage);
    }
    else
    {
        warning(
            "MCTP communication timeout on EID {EID} for unknown command code {CODE}",
            "EID", error.destEid, "CODE", error.commandCode);
    }
}

static void handleTransportError(const std::shared_ptr<MCTPReactor>& reactor,
                                 const TransportErrorInfo& error)
{
    std::string driverOperation = "MessageTransmission";

    if (error.msgType == MCTP_CTRL_HDR_MSG_TYPE)
    {
        if (mctpCommandTable.contains(error.commandCode))
        {
            driverOperation =
                mctpCommandTable.at(error.commandCode).driverOperation;
        }
        else
        {
            driverOperation = "MCTPControlMessage";
        }
    }

    auto deviceNameOpt = reactor->getDeviceName(error.destEid);
    std::string deviceName =
        deviceNameOpt.value_or("EID_" + std::to_string(error.destEid));

    createMctpTransportRedfishEvent(error.errorCode, error.direction,
                                    error.binding, error.destEid,
                                    driverOperation, deviceName);
}

static void handleTransportErrorSignal(
    const std::shared_ptr<MCTPReactor>& reactor, sdbusplus::message_t& msg)
{
    TransportErrorInfo error;

    msg.read(error.errorCode, error.direction, error.binding, error.srcEid,
             error.destEid, error.tag, error.msgType, error.commandCode,
             error.interface);

    // Edge case: If mctpd sends destEid as 0 (broadcast/early setup),
    // try to infer the correct EID from the interface name
    if (error.destEid == 0)
    {
        auto staticEid = reactor->getStaticEidFromInterface(error.interface);
        if (staticEid)
        {
            error.destEid = *staticEid;
        }
    }

    // Suppress logs for EIDs currently undergoing health checks (pings 1 & 2).
    // The 3rd ping failure will remove the EID from this set, allowing the log.
    if (suppressedHealthCheckEids.contains(error.destEid))
    {
        // Suppress ALL transport errors (TX and RX) while EID is suppressed
        return;
    }

    // Suppress logs if this specific EID's device is retrying setup
    // This allows other unrelated devices to still log their errors
    if (reactor->isRetrying(error.destEid))
    {
        return;
    }

    // Dispatch to appropriate handler based on error type
    if (error.errorCode == ETIMEDOUT && error.direction == MCTP_DIR_RX &&
        error.msgType == MCTP_CTRL_HDR_MSG_TYPE)
    {
        handleApplicationTimeout(reactor, error);
    }
    else
    {
        handleTransportError(reactor, error);
    }
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
        std::bind_front(handleTransportErrorSignal, reactor));

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

    struct utsname unameData{};
    if (uname(&unameData) == 0)
    {
        std::filesystem::path mctpModulePath =
            std::format("/lib/modules/{}/kernel/drivers/usb/gadget/function/"
                        "usb_f_mctp.ko",
                        unameData.release);

        if (std::filesystem::exists(mctpModulePath))
        {
            boost::asio::post(io, [reactor, systemBus]() {
                info("Creating USB Gadget MCTP device");

                auto gsc = std::make_shared<GetSensorConfiguration>(
                    systemBus,
                    std::bind_front(manageMCTPEntity, systemBus, reactor));
                gsc->getConfiguration({"MCTPUSBGadgetTarget"});
            });
        }
        else
        {
            info(
                "USB Gadget MCTP not supported: kernel module {PATH} not found",
                "PATH", mctpModulePath.string());
        }
    }
    else
    {
        warning("Failed to get kernel version, skipping USB Gadget MCTP setup");
    }

    io.run();

    return EXIT_SUCCESS;
}
