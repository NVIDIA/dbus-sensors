#include "MCTPEndpoint.hpp"

#include "Utils.hpp"
#include "VariantVisitors.hpp"

#include <bits/fs_dir.h>

#include <boost/system/detail/errc.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <cassert>
#include <charconv>
#include <cstdint>
#include <exception>
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
#include <utility>
#include <variant>
#include <vector>

PHOSPHOR_LOG2_USING;

static constexpr const char* mctpdBusName = "au.com.codeconstruct.MCTP1";
static constexpr const char* mctpdControlPath = "/au/com/codeconstruct/mctp1";
static constexpr const char* mctpdControlInterface =
    "au.com.codeconstruct.MCTP.BusOwner1";
static constexpr const char* mctpdEndpointControlInterface =
    "au.com.codeconstruct.MCTP.Endpoint1";

MCTPDDevice::MCTPDDevice(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    const std::string& interface, const std::vector<uint8_t>& physaddr,
    std::optional<std::uint8_t> staticEID) :
    connection(connection),
    interface(interface), physaddr(physaddr), staticEID(staticEID)
{}

MCTPDDevice::MCTPDDevice(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    const std::string& usbInterfaceName) :
    connection(connection),
    usbInterfaceName(usbInterfaceName)
{}

void MCTPDDevice::onEndpointInterfacesRemoved(
    const std::weak_ptr<MCTPDDevice>& weak, const std::string& objpath,
    sdbusplus::message_t& msg)
{
    auto path = msg.unpack<sdbusplus::message::object_path>();
    assert(path.str == objpath);

    auto removedIfaces = msg.unpack<std::set<std::string>>();
    if (!removedIfaces.contains(mctpdEndpointControlInterface))
    {
        return;
    }

    if (auto self = weak.lock())
    {
        self->endpointRemoved();
    }
    else
    {
        info(
            "Device for inventory at '{INVENTORY_PATH}' was destroyed concurrent to endpoint removal",
            "INVENTORY_PATH", objpath);
    }
}

void MCTPDDevice::finaliseEndpoint(
    const std::string& objpath, uint8_t eid, int network,
    std::function<void(const std::error_code& ec,
                       const std::shared_ptr<MCTPEndpoint>& ep)>& added)
{
    const auto matchSpec =
        sdbusplus::bus::match::rules::interfacesRemovedAtPath(objpath);
    removeMatch = std::make_unique<sdbusplus::bus::match_t>(
        *connection, matchSpec,
        std::bind_front(MCTPDDevice::onEndpointInterfacesRemoved,
                        weak_from_this(), objpath));
    endpoint = std::make_shared<MCTPDEndpoint>(shared_from_this(), connection,
                                               objpath, network, eid);
    added({}, endpoint);
}

void MCTPDDevice::setup(
    std::function<void(const std::error_code& ec,
                       const std::shared_ptr<MCTPEndpoint>& ep)>&& added)
{
    // Use a lambda to separate state validation from business logic,
    // where the business logic for a successful setup() is encoded in
    // MctpdDevice::finaliseEndpoint()
    auto onSetup = [weak{weak_from_this()}, added{std::move(added)}](
                       const boost::system::error_code& ec, uint8_t eid,
                       int network, const std::string& objpath,
                       bool allocated [[maybe_unused]]) mutable {
        if (ec)
        {
            added(ec, {});
            return;
        }

        if (auto self = weak.lock())
        {
            self->finaliseEndpoint(objpath, eid, network, added);
        }
        else
        {
            info(
                "Device object for inventory at '{INVENTORY_PATH}' was destroyed concurrent to completion of its endpoint setup",
                "INVENTORY_PATH", objpath);
        }
    };
    if (!usbInterfaceName.empty())
    {
        if (staticEID.has_value() && bridgePoolStartEid.has_value())
        {
            connection->async_method_call(
                onSetup, mctpdBusName,
                mctpdControlPath + std::string("/interfaces/") +
                    usbInterfaceName,
                mctpdControlInterface, "AssignEndpointStatic", physaddr,
                staticEID.value(), bridgePoolStartEid.value());
        }
        else
        {
            connection->async_method_call(
                onSetup, mctpdBusName,
                mctpdControlPath + std::string("/interfaces/") +
                    usbInterfaceName,
                mctpdControlInterface, "AssignEndpoint", physaddr);
        }
    }
    else
    {
        if (staticEID.has_value() && bridgePoolStartEid.has_value())
        {
            connection->async_method_call(
                onSetup, mctpdBusName,
                mctpdControlPath + std::string("/interfaces/") + interface,
                mctpdControlInterface, "AssignEndpointStatic", physaddr,
                staticEID.value(), bridgePoolStartEid.value());
        }
        else
        {
            connection->async_method_call(
                onSetup, mctpdBusName,
                mctpdControlPath + std::string("/interfaces/") + interface,
                mctpdControlInterface, "AssignEndpoint", physaddr);
        }
    }
}

void MCTPDDevice::endpointRemoved()
{
    if (endpoint)
    {
        debug("Endpoint removed @ [ {MCTP_ENDPOINT} ]", "MCTP_ENDPOINT",
              endpoint->describe());
        removeMatch.reset();
        endpoint->removed();
        endpoint.reset();
    }
}

void MCTPDDevice::remove()
{
    if (endpoint)
    {
        debug("Removing endpoint @ [ {MCTP_ENDPOINT} ]", "MCTP_ENDPOINT",
              endpoint->describe());
        endpoint->remove();
    }
}

std::string MCTPDDevice::describe() const
{
    std::string description = std::format("interface: {}", interface);
    if (!physaddr.empty())
    {
        description.append(", address: 0x [ ");
        auto it = physaddr.begin();
        for (; it != physaddr.end() - 1; it++)
        {
            description.append(std::format("{:02x} ", *it));
        }
        description.append(std::format("{:02x} ]", *it));
    }
    return description;
}

std::string MCTPDEndpoint::path(const std::shared_ptr<MCTPEndpoint>& ep)
{
    return std::format("{}/networks/{}/endpoints/{}", mctpdControlPath,
                       ep->network(), ep->eid());
}

void MCTPDEndpoint::onMctpEndpointChange(sdbusplus::message_t& msg)
{
    auto [iface, changed,
          _] = msg.unpack<std::string, std::map<std::string, BasicVariantType>,
                          std::vector<std::string>>();
    if (iface != mctpdEndpointControlInterface)
    {
        return;
    }

    auto it = changed.find("Connectivity");
    if (it == changed.end())
    {
        return;
    }

    updateEndpointConnectivity(std::get<std::string>(it->second));
}

void MCTPDEndpoint::updateEndpointConnectivity(const std::string& connectivity)
{
    if (connectivity == "Degraded")
    {
        if (notifyDegraded)
        {
            notifyDegraded(shared_from_this());
        }
    }
    else if (connectivity == "Available")
    {
        if (notifyAvailable)
        {
            notifyAvailable(shared_from_this());
        }
    }
    else
    {
        debug("Unrecognised connectivity state: '{CONNECTIVITY_STATE}'",
              "CONNECTIVITY_STATE", connectivity);
    }
}

int MCTPDEndpoint::network() const
{
    return mctp.network;
}

uint8_t MCTPDEndpoint::eid() const
{
    return mctp.eid;
}

void MCTPDEndpoint::subscribe(Event&& degraded, Event&& available,
                              Event&& removed)
{
    const auto matchSpec =
        sdbusplus::bus::match::rules::propertiesChangedNamespace(
            objpath.str, mctpdEndpointControlInterface);

    this->notifyDegraded = std::move(degraded);
    this->notifyAvailable = std::move(available);
    this->notifyRemoved = std::move(removed);

    try
    {
        connectivityMatch.emplace(static_cast<sdbusplus::bus_t&>(*connection),
                                  matchSpec,
                                  [weak{weak_from_this()}, path{objpath.str}](
                                      sdbusplus::message_t& msg) {
            if (auto self = weak.lock())
            {
                self->onMctpEndpointChange(msg);
            }
            else
            {
                info(
                    "The endpoint for the device at inventory path '{INVENTORY_PATH}' was destroyed concurrent to the removal of its state change match",
                    "INVENTORY_PATH", path);
            }
        });
        connection->async_method_call(
            [weak{weak_from_this()},
             path{objpath.str}](const boost::system::error_code& ec,
                                const std::variant<std::string>& value) {
            if (ec)
            {
                debug(
                    "Failed to get current connectivity state: {ERROR_MESSAGE}",
                    "ERROR_MESSAGE", ec.message(), "ERROR_CATEGORY",
                    ec.category().name(), "ERROR_CODE", ec.value());
                return;
            }

            if (auto self = weak.lock())
            {
                const std::string& connectivity = std::get<std::string>(value);
                self->updateEndpointConnectivity(connectivity);
            }
            else
            {
                info(
                    "The endpoint for the device at inventory path '{INVENTORY_PATH}' was destroyed concurrent to the completion of its connectivity state query",
                    "INVENTORY_PATH", path);
            }
        },
            mctpdBusName, objpath.str, "org.freedesktop.DBus.Properties", "Get",
            mctpdEndpointControlInterface, "Connectivity");
    }
    catch (const sdbusplus::exception::SdBusError& err)
    {
        this->notifyDegraded = nullptr;
        this->notifyAvailable = nullptr;
        this->notifyRemoved = nullptr;
        std::throw_with_nested(
            MCTPException("Failed to register connectivity signal match"));
    }
}

void MCTPDEndpoint::remove()
{
    connection->async_method_call(
        [self{shared_from_this()}](const boost::system::error_code& ec) {
        if (ec)
        {
            debug("Failed to remove endpoint @ [ {MCTP_ENDPOINT} ]",
                  "MCTP_ENDPOINT", self->describe());
            return;
        }
    },
        mctpdBusName, objpath.str, mctpdEndpointControlInterface, "Remove");
}

void MCTPDEndpoint::removed()
{
    if (notifyRemoved)
    {
        notifyRemoved(shared_from_this());
    }
}

std::string MCTPDEndpoint::describe() const
{
    return std::format("network: {}, EID: {} | {}", mctp.network, mctp.eid,
                       dev->describe());
}

std::shared_ptr<MCTPDevice> MCTPDEndpoint::device() const
{
    return dev;
}

std::optional<SensorBaseConfigMap>
    I2CMCTPDDevice::match(const SensorData& config)
{
    auto iface = config.find(configInterfaceName(configType));
    if (iface == config.end())
    {
        return std::nullopt;
    }
    return iface->second;
}

bool I2CMCTPDDevice::match(const std::set<std::string>& interfaces)
{
    return interfaces.contains(configInterfaceName(configType));
}

std::shared_ptr<I2CMCTPDDevice> I2CMCTPDDevice::from(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    const SensorBaseConfigMap& iface)
{
    auto mType = iface.find("Type");
    if (mType == iface.end())
    {
        throw std::invalid_argument(
            "No 'Type' member found for provided configuration object");
    }

    auto type = std::visit(VariantToStringVisitor(), mType->second);
    if (type != configType)
    {
        throw std::invalid_argument("Not an SMBus device");
    }

    auto mAddress = iface.find("Address");
    auto mBus = iface.find("Bus");
    auto mName = iface.find("Name");
    auto mStaticEndpointID = iface.find("StaticEndpointID");
    auto mbridgePoolStartEid = iface.find("BridgePoolStartEid");
    if (mAddress == iface.end() || mBus == iface.end() || mName == iface.end())
    {
        throw std::invalid_argument(
            "Configuration object violates MCTPI2CTarget schema");
    }

    auto sAddress = std::visit(VariantToStringVisitor(), mAddress->second);
    std::uint8_t address{};
    auto [aptr, aec] = std::from_chars(
        sAddress.data(), sAddress.data() + sAddress.size(), address);
    if (aec != std::errc{})
    {
        throw std::invalid_argument("Bad device address");
    }

    auto sBus = std::visit(VariantToStringVisitor(), mBus->second);
    int bus{};
    auto [bptr, bec] = std::from_chars(sBus.data(), sBus.data() + sBus.size(),
                                       bus);
    if (bec != std::errc{})
    {
        throw std::invalid_argument("Bad bus index");
    }

    std::optional<std::uint8_t> staticEID{};
    if (mStaticEndpointID == iface.end())
    {
        warning(
            "Info: Key 'StaticEndpointID' is not provided; skipping related processing.");
    }
    else
    {
        auto sStaticEndpointID = std::visit(VariantToStringVisitor(),
                                            mStaticEndpointID->second);
        std::uint8_t parsedEID{};
        auto [cptr, cec] = std::from_chars(
            sStaticEndpointID.data(),
            sStaticEndpointID.data() + sStaticEndpointID.size(), parsedEID);
        if (cec != std::errc{})
        {
            throw std::invalid_argument("Bad endpoint address");
        }
        staticEID = parsedEID;
    }

    std::optional<std::uint8_t> bridgePoolStartEid{};
    if (mbridgePoolStartEid == iface.end())
    {
        warning(
            "Info: Key 'BridgePoolStartEid' is not provided; skipping related processing.");
    }
    else
    {
        auto sbridgePoolStartEid = std::visit(VariantToStringVisitor(),
                                              mbridgePoolStartEid->second);
        std::uint8_t parsedbridgePoolStartEid{};
        auto [dptr, dec] = std::from_chars(sbridgePoolStartEid.data(),
                                           sbridgePoolStartEid.data() +
                                               sbridgePoolStartEid.size(),
                                           parsedbridgePoolStartEid);
        if (dec != std::errc{})
        {
            throw std::invalid_argument("Bad BridgePool Start address");
        }
        bridgePoolStartEid = parsedbridgePoolStartEid;
    }

    try
    {
        if (staticEID.has_value() && bridgePoolStartEid.has_value())
        {
            return std::make_shared<I2CMCTPDDevice>(connection, bus, address,
                                                    staticEID.value(),
                                                    bridgePoolStartEid.value());
        }
        return std::make_shared<I2CMCTPDDevice>(connection, bus, address);
    }
    catch (const MCTPException& ex)
    {
        warning(
            "Failed to create I2CMCTPDDevice at [ bus: {I2C_BUS}, address: {I2C_ADDRESS} ]: {EXCEPTION}",
            "I2C_BUS", bus, "I2C_ADDRESS", address, "EXCEPTION", ex);
        return {};
    }
}

std::string I2CMCTPDDevice::interfaceFromBus(int bus)
{
    std::filesystem::path netdir =
        std::format("/sys/bus/i2c/devices/i2c-{}/net", bus);
    std::error_code ec;
    std::filesystem::directory_iterator it(netdir, ec);
    if (ec || it == std::filesystem::end(it))
    {
        error("No net device associated with I2C bus {I2C_BUS} at {NET_DEVICE}",
              "I2C_BUS", bus, "NET_DEVICE", netdir);
        throw MCTPException("Bus is not configured as an MCTP interface");
    }

    return it->path().filename();
}

/* Changes for I2CMCTPUSB */

std::optional<SensorBaseConfigMap>
    USBMCTPDDevice::match(const SensorData& config)
{
    auto iface = config.find(configInterfaceName(configType));
    if (iface == config.end())
    {
        return std::nullopt;
    }
    return iface->second;
}

bool USBMCTPDDevice::match(const std::set<std::string>& interfaces)
{
    return interfaces.contains(configInterfaceName(configType));
}

std::shared_ptr<USBMCTPDDevice> USBMCTPDDevice::from(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    const SensorBaseConfigMap& iface)
{
    auto mName = iface.find("Name");
    auto mType = iface.find("Type");
    auto mInterface = iface.find("Interface");
    if (mType == iface.end())
    {
        throw std::invalid_argument(
            "No 'Type' member found for provided configuration object");
    }

    auto type = std::visit(VariantToStringVisitor(), mType->second);
    if (type != configType)
    {
        throw std::invalid_argument("Not an USB device");
    }

    if (mName == iface.end() || mType == iface.end() || mInterface == iface.end())
    {
        throw std::invalid_argument(
            "Configuration object violates MCTPUSBTarget schema");
    }

    auto usbInterfaceName = std::visit(VariantToStringVisitor(), mInterface->second);

    try
    {
        return std::make_shared<USBMCTPDDevice>(connection, usbInterfaceName);
    }
    catch (const MCTPException& ex)
    {
        warning(
            "Failed to create USBMCTPDDevice at [ usbInterfaceName: {USB_INTERFACE}, physaddr_usb: {PHYSADDR_USB} ]: {EXCEPTION}",
            "USB_INTERFACE", usbInterfaceName, "PHYSADDR_USB", physaddr_usb,
            "EXCEPTION", ex);
        return {};
    }
}
