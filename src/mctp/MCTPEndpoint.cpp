#include "MCTPEndpoint.hpp"

#include "MCTPEndpointUtils.hpp"
#include "Utils.hpp"
#include "VariantVisitors.hpp"

#include <systemd/sd-bus-protocol.h>
#include <systemd/sd-bus.h>

#include <boost/asio/error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/detail/errc.hpp>
#include <phosphor-logging/device_error_log.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

PHOSPHOR_LOG2_USING;

// Use the nv::lg2 namespace for error logging
using nv::lg2::ErrorClass;

// Global set of EIDs for suppressing errors during recovery or health check
std::set<uint8_t> suppressedHealthCheckEids;

static constexpr const char* mctpdBusName = "au.com.codeconstruct.MCTP1";
static constexpr const char* mctpdControlPath = "/au/com/codeconstruct/mctp1";
static constexpr const char* mctpdEndpointPath =
    "/au/com/codeconstruct/mctp1/networks/1/endpoints/";
static constexpr const char* mctpdControlInterface =
    "au.com.codeconstruct.MCTP.BusOwner1";
static constexpr const char* mctpdEndpointControlInterface =
    "au.com.codeconstruct.MCTP.Endpoint1";
static constexpr const char* mctpdNetworkInterface =
    "au.com.codeconstruct.MCTP.Network1";
static constexpr const char* mctpdNetworkPath =
    "/au/com/codeconstruct/mctp1/networks/1";
static constexpr const char* mctpdBridgeInterface =
    "au.com.codeconstruct.MCTP.Bridge1";

MCTPDDevice::MCTPDDevice(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    const std::string& name, const std::string& interface,
    const std::vector<uint8_t>& physaddr, std::optional<std::uint8_t> staticEID,
    std::optional<std::uint8_t> bridgePoolStartEid,
    std::optional<std::uint8_t> bridgePoolEndEid,
    const std::optional<std::vector<uint8_t>>& ignoreEids,
    const std::optional<std::vector<uint8_t>>& ignoreMessageTypes,
    std::optional<std::uint8_t> pollingInterval,
    const std::vector<std::string>& deviceNames) :
    connection(connection), name(name), deviceNames(deviceNames),
    interface(interface), physaddr(physaddr), staticEID(staticEID),
    bridgePoolStartEid(bridgePoolStartEid), bridgePoolEndEid(bridgePoolEndEid),
    ignoreEids(ignoreEids), ignoreMessageTypes(ignoreMessageTypes),
    pollingInterval(pollingInterval)
{
    if (bridgePoolStartEid.has_value() && bridgePoolEndEid.has_value())
    {
        const auto poolStart = bridgePoolStartEid.value();
        const auto poolEnd = bridgePoolEndEid.value();
        /* Use unsigned iteration: uint8_t would wrap past 255 and loop forever
         * when poolEnd is 255. */
        for (unsigned i = poolStart; i <= poolEnd; ++i)
        {
            const auto eid = static_cast<uint8_t>(i);
            if (ignoreEids.has_value() &&
                std::find(ignoreEids->begin(), ignoreEids->end(), eid) !=
                    ignoreEids->end())
            {
                continue;
            }
            unresponsiveBridgePoolEids.insert(eid);
        }
    }
}

MCTPDDevice::~MCTPDDevice()
{
    cleanupDiscoveryNotify();
}

void MCTPDDevice::cleanupDiscoveryNotify()
{
    discoveryNeeded = false;
    if (discoveryCheckTimer)
    {
        try
        {
            discoveryCheckTimer->cancel();
        }
        catch (const std::exception& e)
        {
            debug("Failed to cancel DiscoveryNotify timer: {ERROR}", "ERROR",
                  e.what());
        }
    }
}

void MCTPDDevice::onDiscoveryMatchRule()
{
    if (!connection)
    {
        warning(
            "Skipping DiscoveryNotify setup for interface {INTERFACE}: connection unavailable",
            "INTERFACE", this->interface);
        return;
    }

    if (this->interface.empty())
    {
        debug(
            "Skipping DiscoveryNotify registration: interface not yet resolved");
        return;
    }

    std::string interfacePath =
        std::string(mctpdControlPath) + "/interfaces/" + this->interface;
    const auto matchRule =
        sdbusplus::bus::match::rules::type::signal() +
        sdbusplus::bus::match::rules::path(interfacePath) +
        sdbusplus::bus::match::rules::interface(mctpdControlInterface) +
        sdbusplus::bus::match::rules::member("DiscoveryNotify");
    info("DiscoveryNotify match registered for interface {INTERFACE}.",
         "INTERFACE", this->interface);

    discoveryNotifyMatch = std::make_unique<sdbusplus::bus::match_t>(
        *connection, matchRule,
        [weakThis{weak_from_this()}](sdbusplus::message_t& msg) {
            if (auto self = weakThis.lock())
            {
                self->onDiscoveryNotify(msg);
            }
            else
            {
                error(
                    "MCTPDDevice instance destroyed during DiscoveryNotify handling.");
            }
        });

    discoveryCheckTimer = std::make_unique<boost::asio::steady_timer>(
        connection->get_io_context());
}

void MCTPDDevice::onDiscoveryNotify(sdbusplus::message_t& /*unused*/)
{
    /* Discovery Notify could be broadcasted from unassigned or assigned
     * EID endpoint
     *
     * To be consistant, we must fetch out configuration StaticEndpointID
     * and PhyAddr for either of these cases rather relying on signal eid
     * data from mctpd.
     *
     * This is feasible because DiscoveryNotify signal is now
     * interface path specific.
     * Prioritse to discover and setup undiscovered endpoint first.
     */
    if (!this->endpoint)
    {
        this->performDiscovery();
        return;
    }

    if (discoveryNeeded)
    {
        info("Ignoring DiscoveryNotify for {INTERFACE}", "INTERFACE",
             this->interface);
        return;
    }

    discoveryNeeded = true;

    info(
        "First DiscoveryNotify for {INTERFACE} => scheduling discovery in ~5s.",
        "INTERFACE", this->interface);
    /* Broad logic: This  bumps up discovery notify handler timer for
    another 5s. This is done to ensure that a flood of discovery notifies do
    not cause us to repeatedly perform rediscovery. Upon a timer expiry, the
    event loop will initiate a re-query of the routing table from the bridge
    and update the D-Bus objects. */

    // Cancel any previous timer (if still pending)
    discoveryCheckTimer->cancel();

    using namespace std::chrono_literals;
    discoveryCheckTimer->expires_after(5s);
    discoveryCheckTimer->async_wait(
        [weakThis = weak_from_this()](const boost::system::error_code& ecWait) {
            if (ecWait == boost::asio::error::operation_aborted)
            {
                return;
            }
            if (auto self = weakThis.lock())
            {
                self->discoveryNeeded = false;
                self->performDiscovery();
            }
        });
}

static bool isBridgeInterfaceAbsent(sdbusplus::message_t& msg)
{
    if (!msg)
    {
        return false;
    }

    const sd_bus_error* dbusError = msg.get_error();
    if (dbusError == nullptr || dbusError->name == nullptr)
    {
        return false;
    }

    std::string_view errorName = dbusError->name;
    return errorName == SD_BUS_ERROR_UNKNOWN_INTERFACE ||
           errorName == SD_BUS_ERROR_INVALID_ARGS;
}

static void hasBridgeInterface(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    const std::string& endpointPath,
    std::function<void(std::optional<bool>)>&& callback)
{
    // Use Properties.GetAll to check if the bridge interface exists.
    // TODO: Use ObjectMapper here to avoid expense on GetAll.
    auto bridgeCallback =
        std::make_shared<std::function<void(std::optional<bool>)>>(
            std::move(callback));
    try
    {
        connection->async_method_call(
            [endpointPath, bridgeCallback](const boost::system::error_code& ec,
                                           sdbusplus::message_t& msg) {
                if (ec)
                {
                    if (isBridgeInterfaceAbsent(msg))
                    {
                        info("{BRIDGE_INTERFACE} not found on {ENDPOINT_PATH}",
                             "BRIDGE_INTERFACE", mctpdBridgeInterface,
                             "ENDPOINT_PATH", endpointPath);
                        (*bridgeCallback)(false);
                        return;
                    }

                    error(
                        "Failed to query {BRIDGE_INTERFACE} on {ENDPOINT_PATH}: {ERROR}",
                        "BRIDGE_INTERFACE", mctpdBridgeInterface,
                        "ENDPOINT_PATH", endpointPath, "ERROR", ec.message());
                    (*bridgeCallback)(std::nullopt);
                    return;
                }

                info("{BRIDGE_INTERFACE} exists on {ENDPOINT_PATH}",
                     "BRIDGE_INTERFACE", mctpdBridgeInterface, "ENDPOINT_PATH",
                     endpointPath);
                (*bridgeCallback)(true);
            },
            mctpdBusName, endpointPath, "org.freedesktop.DBus.Properties",
            "GetAll", std::string(mctpdBridgeInterface));
    }
    catch (const std::exception& e)
    {
        error("Failed to query {BRIDGE_INTERFACE} on {ENDPOINT_PATH}: {ERROR}",
              "BRIDGE_INTERFACE", mctpdBridgeInterface, "ENDPOINT_PATH",
              endpointPath, "ERROR", e.what());
        (*bridgeCallback)(std::nullopt);
    }
}

void MCTPDDevice::performDiscovery()
{
    /* Discovery Notify policy :
     * - For direct endpoint, we need to perform device discovery
     * - For bridge endpoint, we need to get the routing table.
     *
     * - If endpoint is not created, we need to create it first. So focus on
     *   device discovery.
     * - If endpoint is already created i.e discovery is not needed, check
     *    - If it's a bridge endpoint, get the routing table
     *    - If it's not a bridge endpoint, then we have case of reset.
     *      - LearnEndpoint : expected that endpoint EID would reset post reset,
     *        this should cause removal of endpoint and let MCTPReactor do
     *        re-discovery
     *      - if endpoint wasn't remove, only it's properties were updated, then
     *        no need to do re-discovery. MCTPD will send fake connectivity
     *        signal.
     */
    auto requestSetup = [weakSelf = weak_from_this()]() {
        auto self = weakSelf.lock();
        if (!self)
        {
            return;
        }

        if (!self->requestSetupCallback)
        {
            warning("Failed to notify MCTPReactor to do setup for {INTERFACE}",
                    "INTERFACE", self->interface);
            return;
        }

        info("Requesting reactor to do setup for {INTERFACE}", "INTERFACE",
             self->interface);
        self->requestSetupCallback(self);
    };

    if (!this->endpoint)
    {
        info(
            "Discovery Notify received for {INTERFACE} of undiscovered endpoint",
            "INTERFACE", this->interface);
        requestSetup();
        return;
    }

    auto path = std::string(mctpdControlPath) + "/interfaces/" +
                this->interface;
    auto continueDiscovery = [weakSelf = weak_from_this(), path, requestSetup](
                                 const std::string& dbusMethod, uint8_t eid) {
        auto self = weakSelf.lock();
        if (!self)
        {
            return;
        }

        auto callback = [weakSelf, requestSetup,
                         dbusMethod](const boost::system::error_code& ec,
                                     sdbusplus::message_t& msg) {
            auto self = weakSelf.lock();
            if (!self)
            {
                return;
            }

            if (ec)
            {
                error("Failed calling {METHOD} for {INTERFACE}: {ERROR}",
                      "METHOD", dbusMethod, "INTERFACE", self->interface,
                      "ERROR", ec.message());
            }
            else
            {
                info("Successfully called {METHOD} for {INTERFACE}.", "METHOD",
                     dbusMethod, "INTERFACE", self->interface);

                if (dbusMethod == "LearnEndpoint")
                {
                    auto [eid, network, objpath, allocated] =
                        msg.unpack<uint8_t, int32_t, std::string, bool>();
                    info(
                        "LearnEndpoint returned eid: {EID}, network: {NETWORK}, objpath: {OBJPATH}, allocated: {ALLOCATED}",
                        "EID", eid, "NETWORK", network, "OBJPATH", objpath,
                        "ALLOCATED", allocated);
                    if (eid == 0 && !allocated && objpath.empty())
                    {
                        // Post reset, endpoint was removed.
                        requestSetup();
                    }
                }
            }
        };

        if (dbusMethod == "GetRoutingTable")
        {
            info("Calling GetRoutingTable for {INTERFACE} with EID {EID}",
                 "INTERFACE", self->interface, "EID", eid);
            self->connection->async_method_call(
                callback, mctpdBusName, path, mctpdControlInterface, dbusMethod,
                eid);
            return;
        }

        if (!self->requestSetupCallback)
        {
            warning("Failed to notify MCTPReactor to do setup for {INTERFACE}",
                    "INTERFACE", self->interface);
            return;
        }

        info(
            "Discovery Notify received for {INTERFACE} of already discovered endpoint",
            "INTERFACE", self->interface);
        info("Calling LearnEndpoint for {INTERFACE} with EID {EID}",
             "INTERFACE", self->interface, "EID", eid);
        self->connection->async_method_call(callback, mctpdBusName, path,
                                            mctpdControlInterface, dbusMethod,
                                            self->physaddr);
    };

    const uint8_t eid = this->endpoint->eid();
    std::string endpointPath = mctpdEndpointPath + std::to_string(eid);
    info(
        "Discovery Notify received for {INTERFACE} of already discovered endpoint",
        "INTERFACE", this->interface);
    hasBridgeInterface(
        connection, endpointPath,
        [weakSelf = weak_from_this(), continueDiscovery, requestSetup,
         eid](std::optional<bool> bridge) {
            if (!bridge)
            {
                return;
            }

            auto self = weakSelf.lock();
            if (!self)
            {
                return;
            }

            if (!self->endpoint)
            {
                info(
                    "Discovery Notify received for {INTERFACE} of undiscovered endpoint",
                    "INTERFACE", self->interface);
                requestSetup();
                return;
            }

            continueDiscovery(*bridge ? "GetRoutingTable" : "LearnEndpoint",
                              eid);
        });
}

void MCTPDDevice::onEndpointInterfacesRemoved(
    const std::weak_ptr<MCTPDDevice>& weak, const std::string& objpath,
    sdbusplus::message_t& msg)
{
    auto path = msg.unpack<sdbusplus::object_path>();
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
    markDiscoveredMctpEid(eid);

    onEndpointEstablished();

    added({}, endpoint);
}

void MCTPDDevice::onEndpointEstablished()
{
    // Clear recovery mode flag when endpoint is successfully established
    inHealthRecoveryMode = false;
    cancelRecoveryTimeout();
    consecutivePingFailures = 0;
    startHealthMonitoring();
}

void MCTPDDevice::startHealthMonitoring()
{
    // Only enable polling if interval is specified and > 0
    if (!pollingInterval.has_value() || pollingInterval.value() == 0 ||
        !staticEID.has_value())
    {
        return;
    }

    if (endpoint && endpoint->eid() != staticEID.value())
    {
        warning(
            "Endpoint EID {ENDPOINT_EID} does not match static EID {STATIC_EID}",
            "ENDPOINT_EID", endpoint->eid(), "STATIC_EID", staticEID.value());
        return;
    }

    if (!healthTimer)
    {
        healthTimer = std::make_unique<boost::asio::steady_timer>(
            connection->get_io_context());
    }

    healthTimer->expires_after(std::chrono::seconds{pollingInterval.value()});
    healthTimer->async_wait(
        [weak = weak_from_this()](const boost::system::error_code& ec) {
            if (!ec)
            {
                if (auto self = weak.lock())
                {
                    self->performHealthCheck();
                }
            }
        });
}

void MCTPDDevice::stopHealthMonitoring()
{
    if (healthTimer)
    {
        healthTimer->cancel();
    }
}

void MCTPDDevice::performHealthCheck()
{
    if (!staticEID.has_value() || !pollingInterval.has_value())
    {
        return; // Health monitoring not properly configured
    }

    if (!healthTimer)
    {
        return; // Timer not initialized
    }

    // Reset suppression state for this specific check iteration
    suppressedHealthCheckEids.erase(*staticEID);

    // Suppress errors before and after the threshold attempt. The threshold
    // ping is left unsuppressed so injected TransportErrors produce one RF log.
    if (consecutivePingFailures < pingFailureThreshold - 1 ||
        consecutivePingFailures >= pingFailureThreshold)
    {
        suppressedHealthCheckEids.insert(*staticEID);
    }

    // Poll main device health via EndpointPing (which internally does GetUUID)
    connection->async_method_call(
        [weak = weak_from_this()](const boost::system::error_code& ec) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }

            bool isResponsive = !ec;

            if (!isResponsive)
            {
                // Device Failure Handling
                if (!self->inHealthRecoveryMode)
                {
                    // First failure: Log & Start Recovery
                    if (self->endpoint)
                    {
                        self->consecutivePingFailures++;
                        info(
                            "Ping failed for EID {EID}. Failure count: {COUNT}/{THRESHOLD}",
                            "EID", self->endpoint->eid(), "COUNT",
                            self->consecutivePingFailures, "THRESHOLD",
                            self->pingFailureThreshold);

                        if (self->consecutivePingFailures >=
                            self->pingFailureThreshold)
                        {
                            info(
                                "Health error: Device not responsive (Timeout), EID {EID} after {COUNT} failures",
                                "EID", self->endpoint->eid(), "COUNT",
                                self->consecutivePingFailures);
                            // Log the MCTP ping failure for Redfish only after
                            // 3 consecutive timeouts, and only if mctpd has
                            // published this endpoint on D-Bus
                            // (InterfacesAdded).
                            if (ec == boost::system::errc::timed_out &&
                                self->discoveredMctpEids.contains(
                                    self->endpoint->eid()))
                            {
                                logMCTPError(
                                    self->name, self->endpoint->eid(),
                                    nv::lg2::ErrorCode::MCTP::
                                        MCTP_TRANSPORT_FAIL_PING_TIMEOUT,
                                    "MCTP ping failed due to timeout for the device");
                            }
                            self->recover();
                        }
                    }
                }
            }
            else
            {
                // Device Responsive - Reset failure counter and suppression
                // flag
                self->consecutivePingFailures = 0;
                suppressedHealthCheckEids.erase(self->staticEID.value());

                // Device Responsive Handling
                if (self->inHealthRecoveryMode)
                {
                    if (self->endpoint)
                    {
                        // Recovery Complete
                        info("Health recovery: Device responsive, EID {EID}",
                             "EID", self->endpoint->eid());
                        self->inHealthRecoveryMode = false;
                    }
                    else if (self->requestSetupCallback)
                    {
                        // Responsive but endpoint object missing?
                        // Trigger setup to restore object.
                        self->requestSetupCallback(self);
                    }
                }
            }
        },
        mctpdBusName, mctpdNetworkPath, mctpdNetworkInterface, "EndpointPing",
        *staticEID);

    // For bridge devices, also check pool range (in parallel with main device)
    if (bridgePoolStartEid.has_value() && bridgePoolEndEid.has_value())
    {
        const auto start = bridgePoolStartEid.value();
        const auto end = bridgePoolEndEid.value();
        // Ping each EID in the pool
        for (unsigned i = start; i <= end; ++i)
        {
            const auto eid = static_cast<uint8_t>(i);
            if (ignoreEids.has_value() &&
                std::find(ignoreEids->begin(), ignoreEids->end(), eid) !=
                    ignoreEids->end())
            {
                continue;
            }

            std::string deviceName = getNameForEid(eid).value_or("");

            // Reset suppression state for this EID
            suppressedHealthCheckEids.erase(eid);

            // Suppress errors before and after the threshold attempt. The
            // threshold ping is left unsuppressed so injected TransportErrors
            // produce one RF log.
            if (bridgePoolPingFailures[eid] < pingFailureThreshold - 1 ||
                bridgePoolPingFailures[eid] >= pingFailureThreshold)
            {
                suppressedHealthCheckEids.insert(eid);
            }

            connection->async_method_call(
                [weak = weak_from_this(), eid,
                 deviceName](const boost::system::error_code& ec) {
                    auto self = weak.lock();
                    if (!self)
                    {
                        return;
                    }

                    if (ec)
                    {
                        /* Count failures toward threshold even when the EID is
                         * already listed unresponsive (constructor seeds pool
                         * EIDs so the first successful ping triggers
                         * LearnEndpoint). Once at threshold, stop counting
                         * (same as before). */
                        if (!self->unresponsiveBridgePoolEids.contains(eid) ||
                            self->bridgePoolPingFailures[eid] <
                                self->pingFailureThreshold)
                        {
                            self->bridgePoolPingFailures[eid]++;
                            info(
                                "Ping failed for Bridge Pool EID {EID}. Failure count: {COUNT}/{THRESHOLD}",
                                "EID", eid, "COUNT",
                                self->bridgePoolPingFailures[eid], "THRESHOLD",
                                self->pingFailureThreshold);

                            if (self->bridgePoolPingFailures[eid] >=
                                self->pingFailureThreshold)
                            {
                                info(
                                    "Bridge pool EID {EID} not responsive after {COUNT} timeouts",
                                    "EID", eid, "COUNT",
                                    self->bridgePoolPingFailures[eid]);
                                if (ec == boost::system::errc::timed_out &&
                                    self->discoveredMctpEids.contains(eid))
                                {
                                    logMCTPError(
                                        deviceName, eid,
                                        nv::lg2::ErrorCode::MCTP::
                                            MCTP_TRANSPORT_FAIL_PING_TIMEOUT,
                                        "MCTP ping failed due to timeout for the device");
                                }
                                self->unresponsiveBridgePoolEids.insert(eid);
                                self->recover(eid);
                            }
                        }
                    }
                    else
                    {
                        self->bridgePoolPingFailures[eid] = 0;
                        suppressedHealthCheckEids.erase(eid);

                        const bool wasUnresponsive =
                            self->unresponsiveBridgePoolEids.contains(eid);
                        if (wasUnresponsive)
                        {
                            info("Bridge pool EID {EID} accessible", "EID",
                                 eid);
                            self->unresponsiveBridgePoolEids.erase(eid);
                        }

                        if (!self->discoveryNeeded && wasUnresponsive)
                        {
                            self->connection->async_method_call(
                                [weak,
                                 eid](const boost::system::error_code& ec) {
                                    auto self = weak.lock();
                                    if (!self)
                                    {
                                        return;
                                    }
                                    if (ec)
                                    {
                                        error(
                                            "Failed to initiate net LearnEndpoint for EID {EID}: {ERROR}",
                                            "EID", eid, "ERROR", ec.message());
                                        self->unresponsiveBridgePoolEids.insert(
                                            eid);
                                    }
                                },
                                mctpdBusName, mctpdNetworkPath,
                                mctpdNetworkInterface, "LearnEndpoint", eid);
                        }
                    }
                },
                mctpdBusName, mctpdNetworkPath, mctpdNetworkInterface,
                "EndpointPing", eid);
        }
    }

    // Reschedule next health check (after initiating all pings)
    healthTimer->expires_after(std::chrono::seconds{*pollingInterval});
    healthTimer->async_wait(
        [weak = weak_from_this()](const boost::system::error_code& ec) {
            if (!ec)
            {
                if (auto self = weak.lock())
                {
                    self->performHealthCheck();
                }
            }
        });
}

void MCTPDDevice::markDiscoveredMctpEid(uint8_t eid)
{
    if (!managesEid(eid))
    {
        return;
    }
    if (discoveredMctpEids.insert(eid).second)
    {
        info(
            "Recorded MCTP endpoint discovery for EID {EID} on device {DEVICE}",
            "EID", eid, "DEVICE", name);
    }
}

void MCTPDDevice::armRecoveryTimeout()
{
    static constexpr std::chrono::seconds recoveryTimeout{10};

    if (!connection || !pollingInterval.has_value() ||
        pollingInterval.value() == 0)
    {
        return;
    }

    if (!recoveryTimer)
    {
        recoveryTimer = std::make_unique<boost::asio::steady_timer>(
            connection->get_io_context());
    }

    recoveryTimer->expires_after(recoveryTimeout);
    recoveryTimer->async_wait(
        [weak = weak_from_this()](const boost::system::error_code& ec) {
            if (ec)
            {
                return;
            }
            if (auto self = weak.lock())
            {
                self->onRecoveryTimeout();
            }
        });
}

void MCTPDDevice::cancelRecoveryTimeout()
{
    if (recoveryTimer)
    {
        recoveryTimer->cancel();
    }
}

void MCTPDDevice::onRecoveryTimeout()
{
    if (!inHealthRecoveryMode)
    {
        return;
    }

    warning(
        "Recovery timeout for device {DEVICE_NAME}; restarting health monitoring",
        "DEVICE_NAME", name);
    if (endpoint)
    {
        inHealthRecoveryMode = false;
    }
    startHealthMonitoring();
}

void MCTPDDevice::recover(uint8_t eid)
{
    if (!discoveredMctpEids.contains(eid))
    {
        info(
            "Skipping recover for EID {EID}: never observed on D-Bus for device {DEVICE}",
            "EID", eid, "DEVICE", name);
        return;
    }

    // Suppress errors during the recovery attempt
    suppressedHealthCheckEids.insert(eid);

    info("Recovering health for device {DEVICE_NAME}, EID {EID}", "DEVICE_NAME",
         name, "EID", eid);

    std::string path = mctpdEndpointPath + std::to_string(eid);
    connection->async_method_call(
        [eid](const boost::system::error_code& ec) {
            if (ec)
            {
                error("Failed to initiate recovery for EID {EID}: {ERROR}",
                      "EID", eid, "ERROR", ec.message());
            }
        },
        mctpdBusName, path, mctpdEndpointControlInterface, "Recover");
}

void MCTPDDevice::recover()
{
    if (!endpoint)
    {
        inHealthRecoveryMode = true;
        stopHealthMonitoring();
        return;
    }

    const uint8_t eid = endpoint->eid();
    if (!discoveredMctpEids.contains(eid))
    {
        info(
            "Skipping recover for main endpoint EID {EID}: never observed on D-Bus for device {DEVICE}",
            "EID", eid, "DEVICE", name);
        return;
    }

    inHealthRecoveryMode = true;

    // Stop health monitoring while in recovery mode to avoid wasteful pings.
    // It will restart automatically when device is successfully set up again.
    stopHealthMonitoring();
    armRecoveryTimeout();

    recover(eid);
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
            if (!allocated && self->endpoint)
            {
                added({}, {});
                return;
            }

            self->finaliseEndpoint(objpath, eid, network, added);
        }
        else
        {
            info(
                "Device object for inventory at '{INVENTORY_PATH}' was destroyed concurrent to completion of its endpoint setup",
                "INVENTORY_PATH", objpath);
        }
    };
    if (staticEID.has_value())
    {
        connection->async_method_call(
            onSetup, mctpdBusName,
            mctpdControlPath + std::string("/interfaces/") + interface,
            mctpdControlInterface, "AssignEndpointStatic", physaddr,
            staticEID.value(),
            static_cast<uint8_t>(bridgePoolStartEid.value_or(0)),
            ignoreEids.value_or(std::vector<uint8_t>{}),
            ignoreMessageTypes.value_or(std::vector<uint8_t>{}));
    }
    else
    {
        // mctpd 2.5 AssignEndpoint has D-Bus signature "ay" (physaddr only);
        // ignoreMessageTypes is not a per-endpoint argument on the dynamic path
        // (it is a bus-owner-wide setting in mctpd.conf). Passing it here makes
        // the call signature "ayay", which sd-bus rejects with InvalidArgs, so
        // no EID is ever assigned. AssignEndpointStatic above keeps its 5-arg
        // form ("ayyyayay"), which matches mctpd and is unaffected.
        connection->async_method_call(
            onSetup, mctpdBusName,
            mctpdControlPath + std::string("/interfaces/") + interface,
            mctpdControlInterface, "AssignEndpoint", physaddr);
    }
}

void MCTPDDevice::endpointRemoved()
{
    if (!inHealthRecoveryMode)
    {
        cancelRecoveryTimeout();
    }

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
    remove({});
}

void MCTPDDevice::remove(std::function<void()>&& removed)
{
    cleanupDiscoveryNotify();

    if (endpoint)
    {
        debug("Removing endpoint @ [ {MCTP_ENDPOINT} ]", "MCTP_ENDPOINT",
              endpoint->describe());
        endpoint->remove(std::move(removed));
        return;
    }

    if (removed)
    {
        removed();
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

// https://en.cppreference.com/w/cpp/utility/hash/operator().html
//
// https://en.wikipedia.org/w/index.php?title=Fowler%E2%80%93Noll%E2%80%93Vo_hash_function&oldid=1312413750#FNV_hash_parameters
template <typename T, typename V, T p>
static std::size_t fnv1a(T h, V e);

template <std::size_t S>
static std::size_t fnv1a(const std::vector<std::uint8_t>& d);

template <typename V>
static std::size_t fnv1a(std::uint32_t h, V v)
{
    constexpr std::uint32_t p = 0x01000193;
    return (h ^ v) * p;
}

template <>
[[maybe_unused]] std::size_t fnv1a<4UL>(const std::vector<std::uint8_t>& d)
{
    std::uint32_t h = 0x811c9dc5;
    for (const auto& v : d)
    {
        h = fnv1a(h, v);
    }
    return h;
}

template <typename V>
static std::size_t fnv1a(std::uint64_t h, V v)
{
    constexpr std::uint64_t p = 0x00000100000001b3;
    return (h ^ v) * p;
}

template <>
[[maybe_unused]] std::size_t fnv1a<8UL>(const std::vector<std::uint8_t>& d)
{
    std::uint64_t h = 0xcbf29ce484222325;
    for (const auto& v : d)
    {
        h = fnv1a(h, v);
    }
    return h;
}

static std::size_t fnv1aHash(const std::vector<std::uint8_t>& d)
{
    return fnv1a<sizeof(std::size_t)>(d);
}

std::size_t MCTPDDevice::id() const
{
    std::size_t h1 = std::hash<std::string>{}(interface);
    std::size_t h2 = fnv1aHash(physaddr);

    return h1 ^ (h2 << 1);
}

std::string MCTPDEndpoint::path(const std::shared_ptr<MCTPEndpoint>& ep)
{
    return std::format("{}/networks/{}/endpoints/{}", mctpdControlPath,
                       ep->network(), ep->eid());
}

void MCTPDEndpoint::onMctpEndpointChange(sdbusplus::message_t& msg)
{
    auto [iface, changed, _] =
        msg.unpack<std::string, std::map<std::string, BasicVariantType>,
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
    info("Updating connectivity for EID {EID}: {STATE}", "EID", mctp.eid,
         "STATE", connectivity);

    if (connectivity == "Degraded")
    {
        if (notifyDegraded)
        {
            notifyDegraded(shared_from_this());
        }
        if (auto mctpdDevice =
                std::dynamic_pointer_cast<MCTPDDevice>(this->device()))
        {
            mctpdDevice->stopHealthMonitoring();
        }
    }
    else if (connectivity == "Available")
    {
        if (notifyAvailable)
        {
            notifyAvailable(shared_from_this());
        }
        if (auto mctpdDevice =
                std::dynamic_pointer_cast<MCTPDDevice>(this->device()))
        {
            mctpdDevice->onEndpointEstablished();
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
        connectivityMatch.emplace(
            static_cast<sdbusplus::bus_t&>(*connection), matchSpec,
            [weak{weak_from_this()},
             path{objpath.str}](sdbusplus::message_t& msg) {
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
                    const std::string& connectivity =
                        std::get<std::string>(value);
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
    remove({});
}

void MCTPDEndpoint::remove(std::function<void()>&& removed)
{
    connection->async_method_call(
        [self{shared_from_this()}, removed = std::move(removed)](
            const boost::system::error_code& ec) mutable {
            if (ec)
            {
                debug("Failed to remove endpoint @ [ {MCTP_ENDPOINT} ]",
                      "MCTP_ENDPOINT", self->describe());
            }
            if (removed)
            {
                removed();
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

std::optional<SensorBaseConfigMap> I2CMCTPDDevice::match(
    const SensorData& config)
{
    auto iface = config.find(configInterfaceName(configType));
    if (iface == config.end())
    {
        return std::nullopt;
    }
    return iface->second;
}

std::optional<SensorBaseConfigMap> I3CMCTPDDevice::match(
    const SensorData& config)
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

bool I3CMCTPDDevice::match(const std::set<std::string>& interfaces)
{
    return interfaces.contains(configInterfaceName(configType));
}

// Parse a comma-separated byte-list entity-manager property ("IgnoreEIDs",
// "IgnoreMessageTypes") into a validated 0-255 list. Entries that are out of
// range or non-numeric are skipped with a warning. Returns std::nullopt when
// the key is absent, the string is empty, or parsing throws, so callers
// forward the result straight into the MCTPDDevice ctor (which passes the
// lists to mctpd's AssignEndpointStatic — skipping the named pool slots and
// masking the named message types). @a context names the device in log lines.
// Shared by every transport consumer so these properties are honoured
// identically regardless of binding.
static std::optional<std::vector<std::uint8_t>> parseByteList(
    const SensorBaseConfigMap& iface, const std::string& property,
    const std::string& context)
{
    std::optional<std::vector<std::uint8_t>> values{};
    auto mProperty = iface.find(property);
    if (mProperty == iface.end())
    {
        info(
            "Info: Key '{PROPERTY}' is not provided for {CONTEXT}; skipping related processing.",
            "PROPERTY", property, "CONTEXT", context);
        return values;
    }
    try
    {
        auto valuesStr =
            std::visit(VariantToStringVisitor(), mProperty->second);
        if (!valuesStr.empty())
        {
            values = std::vector<std::uint8_t>{};
            std::stringstream ss(valuesStr);
            std::string token;
            while (std::getline(ss, token, ','))
            {
                token.erase(0, token.find_first_not_of(" \t"));
                token.erase(token.find_last_not_of(" \t") + 1);
                if (!token.empty())
                {
                    try
                    {
                        int64_t intVal = std::stoll(token);
                        if (intVal >= 0 && intVal <= 255)
                        {
                            values->push_back(
                                static_cast<std::uint8_t>(intVal));
                        }
                        else
                        {
                            warning(
                                "{PROPERTY} entry out of range (0-255): {VALUE}",
                                "PROPERTY", property, "VALUE", intVal);
                        }
                    }
                    catch (const std::exception& e)
                    {
                        warning("Invalid {PROPERTY} entry: '{VALUE}' - {ERROR}",
                                "PROPERTY", property, "VALUE", token, "ERROR",
                                e.what());
                    }
                }
            }
            info("Successfully parsed {COUNT} {PROPERTY} entries for {CONTEXT}",
                 "COUNT", values->size(), "PROPERTY", property, "CONTEXT",
                 context);
        }
        else
        {
            info(
                "{PROPERTY} string is empty, no entries to parse for {CONTEXT}",
                "PROPERTY", property, "CONTEXT", context);
            values = std::nullopt;
        }
    }
    catch (const std::exception& e)
    {
        warning("Failed to parse {PROPERTY}: {ERROR} for {CONTEXT}", "PROPERTY",
                property, "ERROR", e.what(), "CONTEXT", context);
        values = std::nullopt;
    }
    return values;
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
    auto mbridgePoolEndEid = iface.find("BridgePoolEndEID");
    if (mAddress == iface.end() || mBus == iface.end() || mName == iface.end())
    {
        throw std::invalid_argument(
            "Configuration object violates MCTPI2CTarget schema");
    }

    std::vector<std::string> names = getDeviceNames(iface);
    std::string name = names[0];

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
    auto [bptr,
          bec] = std::from_chars(sBus.data(), sBus.data() + sBus.size(), bus);
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
        auto sStaticEndpointID =
            std::visit(VariantToStringVisitor(), mStaticEndpointID->second);
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
        auto sbridgePoolStartEid =
            std::visit(VariantToStringVisitor(), mbridgePoolStartEid->second);
        std::uint8_t parsedbridgePoolStartEid{};
        auto [dptr, dec] = std::from_chars(
            sbridgePoolStartEid.data(),
            sbridgePoolStartEid.data() + sbridgePoolStartEid.size(),
            parsedbridgePoolStartEid);
        if (dec != std::errc{})
        {
            throw std::invalid_argument("Bad BridgePool Start address");
        }
        bridgePoolStartEid = parsedbridgePoolStartEid;
    }

    std::optional<std::uint8_t> bridgePoolEndEid{};
    if (mbridgePoolEndEid != iface.end())
    {
        auto sbridgePoolEndEid =
            std::visit(VariantToStringVisitor(), mbridgePoolEndEid->second);
        std::uint8_t parsedbridgePoolEndEid{};
        auto [eptr, eec] =
            std::from_chars(sbridgePoolEndEid.data(),
                            sbridgePoolEndEid.data() + sbridgePoolEndEid.size(),
                            parsedbridgePoolEndEid);
        if (eec != std::errc{})
        {
            throw std::invalid_argument("Bad BridgePool End address");
        }
        bridgePoolEndEid = parsedbridgePoolEndEid;
    }

    auto ignoreMessageTypes =
        parseByteList(iface, "IgnoreMessageTypes",
                      std::format("I2C device [ bus: {}, address: {} ]", bus,
                                  static_cast<int>(address)));

    auto pollingInterval = getPollingInterval(iface);

    try
    {
        if (staticEID.has_value() && bridgePoolStartEid.has_value())
        {
            return std::make_shared<I2CMCTPDDevice>(
                connection, name, bus, address, staticEID.value(),
                bridgePoolStartEid.value(), bridgePoolEndEid,
                ignoreMessageTypes, pollingInterval, names);
        }
        if (staticEID.has_value())
        {
            return std::make_shared<I2CMCTPDDevice>(
                connection, name, bus, address, staticEID.value(), std::nullopt,
                bridgePoolEndEid, ignoreMessageTypes, pollingInterval, names);
        }
        return std::make_shared<I2CMCTPDDevice>(
            connection, name, bus, address, std::nullopt, std::nullopt,
            bridgePoolEndEid, ignoreMessageTypes, pollingInterval, names);
    }
    catch (const MCTPException& ex)
    {
        warning(
            "Failed to create I2CMCTPDDevice at [ bus: {I2C_BUS}, address: {I2C_ADDRESS} ]: {EXCEPTION}",
            "I2C_BUS", bus, "I2C_ADDRESS", address, "EXCEPTION", ex);
        return {};
    }
}

std::shared_ptr<I3CMCTPDDevice> I3CMCTPDDevice::from(
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
        throw std::invalid_argument("Not an I3C device");
    }

    auto mAddress = iface.find("Address");
    auto mBus = iface.find("Bus");
    auto mName = iface.find("Name");
    auto mStaticEndpointID = iface.find("StaticEndpointID");
    auto mbridgePoolStartEid = iface.find("BridgePoolStartEid");
    auto mbridgePoolEndEid = iface.find("BridgePoolEndEID");
    if (mAddress == iface.end() || mBus == iface.end() || mName == iface.end())
    {
        throw std::invalid_argument(
            "Configuration object violates MCTPI3CTarget schema");
    }

    std::vector<std::string> names = getDeviceNames(iface);
    std::string name = names[0];

    auto address = std::visit(VariantToNumArrayVisitor<uint8_t, uint64_t>(),
                              mAddress->second);
    if (address.empty())
    {
        throw std::invalid_argument("Bad device address");
    }

    auto sBus = std::visit(VariantToStringVisitor(), mBus->second);
    int bus{};
    auto [bptr,
          bec] = std::from_chars(sBus.data(), sBus.data() + sBus.size(), bus);
    if (bec != std::errc{})
    {
        throw std::invalid_argument("Bad bus index");
    }

    std::optional<std::uint8_t> staticEID{};
    if (mStaticEndpointID == iface.end())
    {
        info(
            "Info: Key 'StaticEndpointID' is not provided; skipping related processing.");
    }
    else
    {
        auto sStaticEndpointID =
            std::visit(VariantToStringVisitor(), mStaticEndpointID->second);
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
        info(
            "Info: Key 'BridgePoolStartEid' is not provided; skipping related processing.");
    }
    else
    {
        auto sbridgePoolStartEid =
            std::visit(VariantToStringVisitor(), mbridgePoolStartEid->second);
        std::uint8_t parsedbridgePoolStartEid{};
        auto [dptr, dec] = std::from_chars(
            sbridgePoolStartEid.data(),
            sbridgePoolStartEid.data() + sbridgePoolStartEid.size(),
            parsedbridgePoolStartEid);
        if (dec != std::errc{})
        {
            throw std::invalid_argument("Bad BridgePool Start address");
        }
        bridgePoolStartEid = parsedbridgePoolStartEid;
    }

    std::optional<std::uint8_t> bridgePoolEndEid{};
    if (mbridgePoolEndEid != iface.end())
    {
        auto sbridgePoolEndEid =
            std::visit(VariantToStringVisitor(), mbridgePoolEndEid->second);
        std::uint8_t parsedbridgePoolEndEid{};
        auto [eptr, eec] =
            std::from_chars(sbridgePoolEndEid.data(),
                            sbridgePoolEndEid.data() + sbridgePoolEndEid.size(),
                            parsedbridgePoolEndEid);
        if (eec != std::errc{})
        {
            throw std::invalid_argument("Bad BridgePool End address");
        }
        bridgePoolEndEid = parsedbridgePoolEndEid;
    }

    auto pollingInterval = getPollingInterval(iface);

    try
    {
        if (staticEID.has_value() && bridgePoolStartEid.has_value())
        {
            return std::make_shared<I3CMCTPDDevice>(
                connection, name, bus, address, staticEID.value(),
                bridgePoolStartEid.value(), bridgePoolEndEid, pollingInterval,
                names);
        }
        if (staticEID.has_value())
        {
            return std::make_shared<I3CMCTPDDevice>(
                connection, name, bus, address, staticEID.value(), std::nullopt,
                bridgePoolEndEid, pollingInterval, names);
        }
        return std::make_shared<I3CMCTPDDevice>(
            connection, name, bus, address, std::nullopt, std::nullopt,
            bridgePoolEndEid, pollingInterval, names);
    }
    catch (const MCTPException& ex)
    {
        warning(
            "Failed to create I3CMCTPDDevice at [ bus: {I3C_BUS} ]: {EXCEPTION}",
            "I3C_BUS", bus, "EXCEPTION", ex);
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

std::string I3CMCTPDDevice::interfaceFromBus(int bus)
{
    std::filesystem::path netdir = std::format("/sys/devices/virtual/net");
    std::error_code ec;
    std::filesystem::directory_iterator it(netdir, ec);
    if (ec || it == std::filesystem::end(it))
    {
        error("No net device associated with I3C bus {I3C_BUS} at {NET_DEVICE}",
              "I3C_BUS", bus, "NET_DEVICE", netdir);
        throw MCTPException("Bus is not configured as an MCTP interface");
    }

    std::string targetInterface = std::format("mctpi3c{}", bus);
    for (const auto& entry : std::filesystem::directory_iterator(netdir))
    {
        if (entry.is_directory() && entry.path().filename() == targetInterface)
        {
            return targetInterface;
        }
    }

    error("No matching net device found for I3C bus {I3C_BUS} at {NET_DEVICE}",
          "I3C_BUS", bus, "NET_DEVICE", netdir);
    throw MCTPException("No matching net device found for the specified bus");
}

/* MCTP USB (upstream MCTPUSBDevice consumer, Gerrit 80452) */

std::optional<SensorBaseConfigMap> USBMCTPDDevice::match(
    const SensorData& config)
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
    std::vector<uint8_t> address{};
    auto mName = iface.find("Name");
    auto mType = iface.find("Type");
    auto mStaticEndpointID = iface.find("StaticEndpointID");
    auto mbridgePoolStartEid = iface.find("BridgePoolStartEID");
    auto mbridgePoolEndEid = iface.find("BridgePoolEndEID");
    auto mIgnoreEids = iface.find("IgnoreEIDs");
    auto mIgnoreMessageTypes = iface.find("IgnoreMessageTypes");
    auto mRecoveryThreshold = iface.find("RecoveryThreshold");
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

    if (mName == iface.end())
    {
        throw std::invalid_argument(
            "Configuration object violates MCTPUSBDevice schema");
    }

    auto name = std::visit(VariantToStringVisitor(), mName->second);

    // The migrated MCTPUSBDevice schema describes the endpoint by physical USB
    // topology (RootHubPath/Port); resolve the netdev at runtime from sysfs,
    // mirroring I2C/I3C/SPI. A literal "Interface" (netdev name) is still
    // accepted as a backward-compatible override.
    std::string interface;
    std::string physicalRootHubPath;
    std::string physicalPort;
    uint8_t physicalConfiguration{};
    uint8_t physicalInterfaceNum{};
    auto mInterface = iface.find("Interface");
    if (mInterface != iface.end())
    {
        interface = std::visit(VariantToStringVisitor(), mInterface->second);
    }
    else
    {
        auto mRootHubPath = iface.find("RootHubPath");
        auto mPort = iface.find("Port");
        auto mConfiguration = iface.find("Configuration");
        auto mInterfaceNum = iface.find("InterfaceNum");
        if (mRootHubPath == iface.end() || mPort == iface.end() ||
            mConfiguration == iface.end() || mInterfaceNum == iface.end())
        {
            throw std::invalid_argument(
                "Configuration object violates MCTPUSBDevice schema:"
                " RootHubPath, Port, Configuration, and InterfaceNum"
                " are all required");
        }

        physicalRootHubPath =
            std::visit(VariantToStringVisitor(), mRootHubPath->second);
        physicalPort = std::visit(VariantToStringVisitor(), mPort->second);
        physicalConfiguration = static_cast<uint8_t>(
            std::visit(VariantToIntVisitor(), mConfiguration->second));
        physicalInterfaceNum = static_cast<uint8_t>(
            std::visit(VariantToIntVisitor(), mInterfaceNum->second));

        try
        {
            interface = interfaceFromRootHubPort(
                physicalRootHubPath, physicalPort, physicalConfiguration,
                physicalInterfaceNum);
            info("USB netdev resolved for"
                 " [ RootHubPath: {ROOT_HUB}, Port: {PORT},"
                 " Config: {CONFIG}, IfaceNum: {IFACE_NUM} ]: {INTERFACE}",
                 "ROOT_HUB", physicalRootHubPath, "PORT", physicalPort,
                 "CONFIG", static_cast<unsigned>(physicalConfiguration),
                 "IFACE_NUM", static_cast<unsigned>(physicalInterfaceNum),
                 "INTERFACE", interface);
        }
        catch (const MCTPException& ex)
        {
            warning("USB netdev not yet visible for"
                    " [ RootHubPath: {ROOT_HUB}, Port: {PORT},"
                    " Config: {CONFIG}, IfaceNum: {IFACE_NUM} ],"
                    " will retry via tick: {EXCEPTION}",
                    "ROOT_HUB", physicalRootHubPath, "PORT", physicalPort,
                    "CONFIG", static_cast<unsigned>(physicalConfiguration),
                    "IFACE_NUM", static_cast<unsigned>(physicalInterfaceNum),
                    "EXCEPTION", ex);
        }
    }

    std::optional<std::uint8_t> staticEID{};
    if (mStaticEndpointID == iface.end())
    {
        warning(
            "Info: Key 'StaticEndpointID' is not provided; skipping related processing.");
    }
    else
    {
        auto sStaticEndpointID =
            std::visit(VariantToStringVisitor(), mStaticEndpointID->second);
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
        auto sbridgePoolStartEid =
            std::visit(VariantToStringVisitor(), mbridgePoolStartEid->second);
        std::uint8_t parsedbridgePoolStartEid{};
        auto [dptr, dec] = std::from_chars(
            sbridgePoolStartEid.data(),
            sbridgePoolStartEid.data() + sbridgePoolStartEid.size(),
            parsedbridgePoolStartEid);
        if (dec != std::errc{})
        {
            throw std::invalid_argument("Bad BridgePool Start address");
        }
        bridgePoolStartEid = parsedbridgePoolStartEid;
    }

    std::optional<std::uint8_t> bridgePoolEndEid{};
    if (mbridgePoolEndEid != iface.end())
    {
        auto sbridgePoolEndEid =
            std::visit(VariantToStringVisitor(), mbridgePoolEndEid->second);
        std::uint8_t parsedbridgePoolEndEid{};
        auto [eptr, eec] =
            std::from_chars(sbridgePoolEndEid.data(),
                            sbridgePoolEndEid.data() + sbridgePoolEndEid.size(),
                            parsedbridgePoolEndEid);
        if (eec != std::errc{})
        {
            throw std::invalid_argument("Bad BridgePool End address");
        }
        bridgePoolEndEid = parsedbridgePoolEndEid;
    }

    std::optional<std::vector<std::uint8_t>> ignoreEids{};
    if (mIgnoreEids == iface.end())
    {
        info(
            "Info: Key 'IgnoreEIDs' is not provided for USB device {USB_DEVICE}; skipping related processing.",
            "USB_DEVICE", interface);
    }
    else
    {
        try
        {
            auto ignoreEidsStr =
                std::visit(VariantToStringVisitor(), mIgnoreEids->second);
            if (!ignoreEidsStr.empty())
            {
                ignoreEids = std::vector<std::uint8_t>{};
                std::stringstream ss(ignoreEidsStr);
                std::string token;
                while (std::getline(ss, token, ','))
                {
                    token.erase(0, token.find_first_not_of(" \t"));
                    token.erase(token.find_last_not_of(" \t") + 1);
                    if (!token.empty())
                    {
                        try
                        {
                            int64_t intVal = std::stoll(token);
                            if (intVal >= 0 && intVal <= 255)
                            {
                                ignoreEids->push_back(
                                    static_cast<uint8_t>(intVal));
                            }
                            else
                            {
                                warning(
                                    "IgnoreEIDs entry out of range (0-255): {EID}",
                                    "EID", intVal);
                            }
                        }
                        catch (const std::exception& e)
                        {
                            warning(
                                "Invalid IgnoreEIDs entry: '{VALUE}' - {ERROR}",
                                "VALUE", token, "ERROR", e.what());
                        }
                    }
                }
                info(
                    "Successfully parsed {COUNT} IgnoreEIDs entries for USB device {USB_DEVICE}",
                    "COUNT", ignoreEids->size(), "USB_DEVICE", interface);
            }
            else
            {
                info(
                    "IgnoreEIDs string is empty, no entries to parse for USB device {USB_DEVICE}",
                    "USB_DEVICE", interface);
                ignoreEids = std::nullopt;
            }
        }
        catch (const std::exception& e)
        {
            warning(
                "Failed to parse IgnoreEIDs: {ERROR} for USB device {USB_DEVICE}",
                "ERROR", e.what(), "USB_DEVICE", interface);
            ignoreEids = std::nullopt;
        }
    }
    // Parse IgnoreMessageTypes
    std::optional<std::vector<std::uint8_t>> ignoreMessageTypes{};
    if (mIgnoreMessageTypes == iface.end())
    {
        info(
            "Info: Key 'IgnoreMessageTypes' is not provided for USB device {USB_DEVICE}; skipping related processing.",
            "USB_DEVICE", interface);
    }
    else
    {
        try
        {
            auto ignoreMessageTypesStr = std::visit(
                VariantToStringVisitor(), mIgnoreMessageTypes->second);
            if (!ignoreMessageTypesStr.empty())
            {
                ignoreMessageTypes = std::vector<std::uint8_t>{};
                std::stringstream ss(ignoreMessageTypesStr);
                std::string token;
                while (std::getline(ss, token, ','))
                {
                    token.erase(0, token.find_first_not_of(" \t"));
                    token.erase(token.find_last_not_of(" \t") + 1);
                    if (!token.empty())
                    {
                        try
                        {
                            int64_t intVal = std::stoll(token);
                            if (intVal >= 0 && intVal <= 255)
                            {
                                ignoreMessageTypes->push_back(
                                    static_cast<uint8_t>(intVal));
                            }
                            else
                            {
                                warning(
                                    "IgnoreMessageTypes entry out of range (0-255): {MSG_TYPE}",
                                    "MSG_TYPE", intVal);
                            }
                        }
                        catch (const std::exception& e)
                        {
                            warning(
                                "Invalid IgnoreMessageTypes entry: '{VALUE}' - {ERROR}",
                                "VALUE", token, "ERROR", e.what());
                        }
                    }
                }
                info(
                    "Successfully parsed {COUNT} IgnoreMessageTypes entries for USB device {USB_DEVICE}",
                    "COUNT", ignoreMessageTypes->size(), "USB_DEVICE",
                    interface);
            }
            else
            {
                info(
                    "IgnoreMessageTypes string is empty, no entries to parse for USB device {USB_DEVICE}",
                    "USB_DEVICE", interface);
                ignoreMessageTypes = std::nullopt;
            }
        }
        catch (const std::exception& e)
        {
            warning(
                "Failed to parse IgnoreMessageTypes: {ERROR} for USB device {USB_DEVICE}",
                "ERROR", e.what(), "USB_DEVICE", interface);
            ignoreMessageTypes = std::nullopt;
        }
    }

    uint8_t recoveryThreshold = 0;
    if (mRecoveryThreshold != iface.end())
    {
        auto sRecoveryThreshold =
            std::visit(VariantToStringVisitor(), mRecoveryThreshold->second);
        unsigned int parsedRecoveryThreshold = 0;
        auto [rptr, rec] = std::from_chars(
            sRecoveryThreshold.data(),
            sRecoveryThreshold.data() + sRecoveryThreshold.size(),
            parsedRecoveryThreshold);
        if (rec != std::errc{} ||
            rptr != sRecoveryThreshold.data() + sRecoveryThreshold.size() ||
            parsedRecoveryThreshold > 10)
        {
            throw std::invalid_argument("Bad RecoveryThreshold value");
        }
        recoveryThreshold = static_cast<uint8_t>(parsedRecoveryThreshold);
        info(
            "Configured RecoveryThreshold={RECOVERY_THRESHOLD} for USB device {USB_DEVICE}",
            "RECOVERY_THRESHOLD", static_cast<unsigned int>(recoveryThreshold),
            "USB_DEVICE", interface);
    }
    else
    {
        info(
            "RecoveryThreshold not provided for USB device {USB_DEVICE}; defaulting to 0 (auto recovery disabled)",
            "USB_DEVICE", interface);
    }

    auto pollingInterval = getPollingInterval(iface);
    std::vector<std::string> names = getDeviceNames(iface);

    try
    {
        std::shared_ptr<USBMCTPDDevice> dev;
        if (staticEID.has_value() && bridgePoolStartEid.has_value())
        {
            dev = std::make_shared<USBMCTPDDevice>(
                connection, name, interface, address, staticEID.value(),
                bridgePoolStartEid.value(), bridgePoolEndEid, ignoreEids,
                ignoreMessageTypes, recoveryThreshold, pollingInterval, names);
        }
        else if (staticEID.has_value())
        {
            dev = std::make_shared<USBMCTPDDevice>(
                connection, name, interface, address, staticEID.value(),
                std::nullopt, bridgePoolEndEid, std::nullopt,
                ignoreMessageTypes, recoveryThreshold, pollingInterval, names);
        }
        else
        {
            dev = std::make_shared<USBMCTPDDevice>(
                connection, name, interface, address, std::nullopt,
                std::nullopt, bridgePoolEndEid, std::nullopt,
                ignoreMessageTypes, recoveryThreshold, pollingInterval, names);
        }
        dev->rootHubPath_ = physicalRootHubPath;
        dev->port_ = physicalPort;
        dev->configuration_ = physicalConfiguration;
        dev->interfaceNum_ = physicalInterfaceNum;
        return dev;
    }
    catch (const MCTPException& ex)
    {
        warning(
            "Failed to create MCTPUSBDevice at [ interface: {USB_INTERFACE} ]: {EXCEPTION}",
            "USB_INTERFACE", interface, "EXCEPTION", ex);
        return {};
    }
}

std::string USBMCTPDDevice::interfaceFromRootHubPort(
    const std::string& rootHubPath, const std::string& port,
    uint8_t configuration, uint8_t interfaceNum)
{
    // USB interface sysfs directories are named "<bus>-<port>:<config>.<alt>".
    // Build the exact suffix so the walk matches only the intended interface —
    // no net/ tiebreaker needed when configuration and interfaceNum are known.
    // Locate the usb<N> root-hub directory that is a direct child of the
    // controller path; its name gives us the kernel-assigned bus number.
    int busNum = -1;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(rootHubPath, ec))
    {
        const std::string fname = entry.path().filename().string();
        if (fname.size() > 3 && fname.starts_with("usb"))
        {
            auto [ptr, err] = std::from_chars(
                fname.data() + 3, fname.data() + fname.size(), busNum);
            if (err == std::errc{} && ptr == fname.data() + fname.size())
            {
                break;
            }
            busNum = -1;
        }
    }
    if (ec || busNum < 0)
    {
        error("Cannot find USB root hub under RootHubPath {ROOT_HUB}",
              "ROOT_HUB", rootHubPath);
        throw MCTPException("Cannot find USB root hub under RootHubPath");
    }

    // Navigate directly to the USB interface directory. Port "1.1.1.1" nests
    // as usb1/1-1/1-1.1/1-1.1.1/1-1.1.1.1/ under the root hub; each dot-
    // separated segment adds one level. No walk or scan needed.
    std::filesystem::path dir(rootHubPath);
    dir /= std::format("usb{}", busNum);

    std::string prefix;
    std::stringstream ss(port);
    std::string segment;
    while (std::getline(ss, segment, '.'))
    {
        prefix = prefix.empty() ? segment : prefix + "." + segment;
        dir /= std::format("{}-{}", busNum, prefix);
    }
    dir /=
        std::format("{}-{}:{}.{}", busNum, port, configuration, interfaceNum);

    std::filesystem::directory_iterator nets(dir / "net", ec);
    if (!ec && nets != std::filesystem::end(nets))
    {
        return nets->path().filename();
    }

    error("No MCTP netdev under RootHubPath {ROOT_HUB} for Port {PORT}",
          "ROOT_HUB", rootHubPath, "PORT", port);
    throw MCTPException("No matching net device found for USB endpoint");
}

std::size_t USBMCTPDDevice::id() const
{
    if (!rootHubPath_.empty())
    {
        std::size_t h1 = std::hash<std::string>{}(rootHubPath_);
        std::size_t h2 = std::hash<std::string>{}(port_);
        return h1 ^ (h2 << 1);
    }
    return MCTPDDevice::id();
}

void USBMCTPDDevice::setup(
    std::function<void(const std::error_code& ec,
                       const std::shared_ptr<MCTPEndpoint>& ep)>&& added)
{
    if (rootHubPath_.empty())
    {
        MCTPDDevice::setup(std::move(added));
        return;
    }

    // Re-resolve the netdev name on every setup attempt until
    // AssignEndpointStatic has succeeded at least once. This handles the udev
    // rename (mctpusb0 → musb1_1_1_1_2): if a previous attempt used the
    // pre-rename name and failed, the next tick picks up the new name here.
    // Once confirmed the name is stable and we skip the sysfs walk entirely.
    if (!interfaceConfirmed_)
    {
        std::string resolved;
        try
        {
            resolved = interfaceFromRootHubPort(rootHubPath_, port_,
                                                configuration_, interfaceNum_);
        }
        catch (const MCTPException& ex)
        {
            debug("USB netdev not yet visible for"
                  " [ RootHubPath: {ROOT_HUB}, Port: {PORT},"
                  " Config: {CONFIG}, IfaceNum: {IFACE_NUM} ],"
                  " will retry on next tick: {EXCEPTION}",
                  "ROOT_HUB", rootHubPath_, "PORT", port_, "CONFIG",
                  static_cast<unsigned>(configuration_), "IFACE_NUM",
                  static_cast<unsigned>(interfaceNum_), "EXCEPTION", ex);
            added(std::make_error_code(std::errc::no_such_device), {});
            return;
        }

        if (resolved != interface)
        {
            if (!interface.empty())
            {
                info("USB netdev name changed"
                     " {OLD_INTERFACE} → {NEW_INTERFACE}"
                     " [ RootHubPath: {ROOT_HUB}, Port: {PORT} ]",
                     "OLD_INTERFACE", interface, "NEW_INTERFACE", resolved,
                     "ROOT_HUB", rootHubPath_, "PORT", port_);
            }
            else
            {
                info("USB netdev resolved for"
                     " [ RootHubPath: {ROOT_HUB}, Port: {PORT} ]: {INTERFACE}",
                     "ROOT_HUB", rootHubPath_, "PORT", port_, "INTERFACE",
                     resolved);
            }
            interface = resolved;
            onDiscoveryMatchRule();
        }
    }

    MCTPDDevice::setup([orig = std::move(added), weak = weak_from_this()](
                           const std::error_code& ec,
                           const std::shared_ptr<MCTPEndpoint>& ep) mutable {
        if (auto self = std::dynamic_pointer_cast<USBMCTPDDevice>(weak.lock()))
        {
            if (!ec)
            {
                self->interfaceConfirmed_ = true;
            }
            // On failure: do NOT clear interface. The next tick will
            // re-resolve via sysfs and pick up any udev rename.
        }
        orig(ec, ep);
    });
}

/* MCTP SPI*/
std::optional<SensorBaseConfigMap> SPIMCTPDDevice::match(
    const SensorData& config)
{
    auto iface = config.find(configInterfaceName(configType));
    if (iface == config.end())
    {
        return std::nullopt;
    }
    return iface->second;
}

bool SPIMCTPDDevice::match(const std::set<std::string>& interfaces)
{
    return interfaces.contains(configInterfaceName(configType));
}

std::shared_ptr<SPIMCTPDDevice> SPIMCTPDDevice::from(
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
        throw std::invalid_argument("Not a SPI device");
    }

    auto mName = iface.find("Name");
    auto mBus = iface.find("Bus");
    auto mChipselect = iface.find("ChipSelect");
    auto mStaticEndpointID = iface.find("StaticEndpointID");
    if (mChipselect == iface.end() || mBus == iface.end() ||
        mName == iface.end())
    {
        throw std::invalid_argument(
            "Configuration object violates MCTPSPIDevice schema");
    }

    std::vector<std::string> names = getDeviceNames(iface);
    const auto* name = names[0].c_str();
    auto sBus = std::visit(VariantToStringVisitor(), mBus->second);
    int bus{};
    auto [bptr,
          bec] = std::from_chars(sBus.data(), sBus.data() + sBus.size(), bus);
    if (bec != std::errc{})
    {
        throw std::invalid_argument("Bad bus index");
    }

    auto sChipselect =
        std::visit(VariantToStringVisitor(), mChipselect->second);
    int chipselect{};
    auto [cptr, cec] =
        std::from_chars(sChipselect.data(),
                        sChipselect.data() + sChipselect.size(), chipselect);
    if (cec != std::errc{})
    {
        throw std::invalid_argument("Bad chip select");
    }

    std::optional<std::uint8_t> staticEID{};
    if (mStaticEndpointID == iface.end())
    {
        warning(
            "Info: Key 'StaticEndpointID' is not provided; skipping related processing.");
    }
    else
    {
        auto sStaticEndpointID =
            std::visit(VariantToStringVisitor(), mStaticEndpointID->second);
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

    auto pollingInterval = getPollingInterval(iface);

    std::string interface;
    try
    {
        interface = interfaceFromBusCs(bus, chipselect);
        info(
            "SPI netdev resolved for [ Bus: {SPI_BUS}, ChipSelect: {SPI_CS} ]: {INTERFACE}",
            "SPI_BUS", bus, "SPI_CS", chipselect, "INTERFACE", interface);
    }
    catch (const MCTPException& ex)
    {
        warning("SPI netdev not yet visible for"
                " [ Bus: {SPI_BUS}, ChipSelect: {SPI_CS} ],"
                " will retry via tick: {EXCEPTION}",
                "SPI_BUS", bus, "SPI_CS", chipselect, "EXCEPTION", ex);
    }

    try
    {
        if (staticEID.has_value())
        {
            return std::make_shared<SPIMCTPDDevice>(
                connection, name, bus, chipselect, interface, staticEID.value(),
                pollingInterval, names);
        }
        return std::make_shared<SPIMCTPDDevice>(
            connection, name, bus, chipselect, interface, std::nullopt,
            pollingInterval, names);
    }
    catch (const MCTPException& ex)
    {
        warning(
            "Failed to create SPIMCTPDDevice at [ bus: {SPI_BUS}, chipselect: {SPI_CS} ]: {EXCEPTION}",
            "SPI_BUS", bus, "SPI_CS", chipselect, "EXCEPTION", ex);
        return {};
    }
}

std::string SPIMCTPDDevice::interfaceFromBusCs(int bus, int chipselect)
{
    std::filesystem::path netdir =
        std::format("/sys/bus/spi/devices/spi{}.{}/net", bus, chipselect);
    std::error_code ec;
    std::filesystem::directory_iterator it(netdir, ec);
    if (ec || it == std::filesystem::end(it))
    {
        error("No net device associated with SPI bus {SPI_BUS} at {NET_DEVICE}",
              "SPI_BUS", bus, "NET_DEVICE", netdir);
        throw MCTPException("Bus is not configured as an MCTP interface");
    }

    return it->path().filename();
}

std::size_t SPIMCTPDDevice::id() const
{
    std::size_t h1 = std::hash<int>{}(bus_);
    std::size_t h2 = std::hash<int>{}(chipselect_);
    return h1 ^ (h2 << 1);
}

void SPIMCTPDDevice::setup(
    std::function<void(const std::error_code& ec,
                       const std::shared_ptr<MCTPEndpoint>& ep)>&& added)
{
    if (!interfaceConfirmed_)
    {
        std::string resolved;
        try
        {
            resolved = interfaceFromBusCs(bus_, chipselect_);
        }
        catch (const MCTPException& ex)
        {
            debug("SPI netdev not yet visible for"
                  " [ Bus: {SPI_BUS}, ChipSelect: {SPI_CS} ],"
                  " will retry on next tick: {EXCEPTION}",
                  "SPI_BUS", bus_, "SPI_CS", chipselect_, "EXCEPTION", ex);
            added(std::make_error_code(std::errc::no_such_device), {});
            return;
        }

        if (resolved != interface)
        {
            if (!interface.empty())
            {
                info("SPI netdev name changed"
                     " {OLD_INTERFACE} → {NEW_INTERFACE}"
                     " [ Bus: {SPI_BUS}, ChipSelect: {SPI_CS} ]",
                     "OLD_INTERFACE", interface, "NEW_INTERFACE", resolved,
                     "SPI_BUS", bus_, "SPI_CS", chipselect_);
            }
            else
            {
                info("SPI netdev resolved for"
                     " [ Bus: {SPI_BUS}, ChipSelect: {SPI_CS} ]: {INTERFACE}",
                     "SPI_BUS", bus_, "SPI_CS", chipselect_, "INTERFACE",
                     resolved);
            }
            interface = resolved;
            onDiscoveryMatchRule();
        }
    }

    MCTPDDevice::setup([orig = std::move(added), weak = weak_from_this()](
                           const std::error_code& ec,
                           const std::shared_ptr<MCTPEndpoint>& ep) mutable {
        if (auto self = std::dynamic_pointer_cast<SPIMCTPDDevice>(weak.lock()))
        {
            if (!ec)
            {
                self->interfaceConfirmed_ = true;
            }
        }
        orig(ec, ep);
    });
}

/* MCTP XROT */
std::optional<SensorBaseConfigMap> XROTMCTPDDevice::match(
    const SensorData& config)
{
    auto iface = config.find(configInterfaceName(configType));
    if (iface == config.end())
    {
        return std::nullopt;
    }
    return iface->second;
}

bool XROTMCTPDDevice::match(const std::set<std::string>& interfaces)
{
    return interfaces.contains(configInterfaceName(configType));
}

std::shared_ptr<XROTMCTPDDevice> XROTMCTPDDevice::from(
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
        throw std::invalid_argument("Not an XROT device");
    }

    auto mName = iface.find("Name");
    auto mStaticEndpointID = iface.find("StaticEndpointID");
    auto mInterface = iface.find("Interface");

    if (mName == iface.end() || mInterface == iface.end())
    {
        throw std::invalid_argument(
            "Configuration object violates MCTPXROTTarget schema");
    }

    std::vector<std::string> names = getDeviceNames(iface);
    const auto* name = names[0].c_str();

    auto sInterface = std::visit(VariantToStringVisitor(), mInterface->second);
    const char* interface = sInterface.c_str();

    std::optional<std::uint8_t> staticEID{};
    if (mStaticEndpointID == iface.end())
    {
        warning(
            "Info: Key 'StaticEndpointID' is not provided; skipping related processing.");
    }
    else
    {
        auto sStaticEndpointID =
            std::visit(VariantToStringVisitor(), mStaticEndpointID->second);
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

    auto pollingInterval = getPollingInterval(iface);

    try
    {
        if (staticEID.has_value())
        {
            return std::make_shared<XROTMCTPDDevice>(
                connection, name, interface, staticEID.value(), pollingInterval,
                names);
        }
        return std::make_shared<XROTMCTPDDevice>(
            connection, name, interface, std::nullopt, pollingInterval, names);
    }
    catch (const MCTPException& ex)
    {
        warning(
            "Failed to create XROTMCTPDDevice at [ name: {XROT_NAME} ]: {EXCEPTION}",
            "XROT_NAME", name, "EXCEPTION", ex);
        return {};
    }
}

/* MCTP PCIe */
std::optional<SensorBaseConfigMap> PCIeMCTPDDevice::match(
    const SensorData& config)
{
    auto iface = config.find(configInterfaceName(configType));
    if (iface == config.end())
    {
        return std::nullopt;
    }
    return iface->second;
}

bool PCIeMCTPDDevice::match(const std::set<std::string>& interfaces)
{
    return interfaces.contains(configInterfaceName(configType));
}

/*
 * Parse a PCIe BDF string of the form "[domain:]bus:device.function"
 * (e.g. "0000:01:00.0" or "01:00.0") into the 2-byte physical address
 * { bus, devfn } that the kernel mctp-pcie binding uses for routing
 * (matches the "address bb:df" column in `mctp link show`).
 *
 * The PCIe domain is parsed but ignored: the link layer address is only
 * the requester ID (bus:devfn).
 */
static unsigned int parsePcieBdfHexField(std::string_view field,
                                         const std::string& bdf)
{
    if (field.empty())
    {
        throw std::invalid_argument("Bad BDF: " + bdf);
    }

    unsigned int value = 0;
    auto [ptr, ec] =
        std::from_chars(field.data(), field.data() + field.size(), value, 16);
    if (ec != std::errc{} || ptr != field.data() + field.size())
    {
        throw std::invalid_argument("Bad BDF: " + bdf);
    }
    return value;
}

static std::vector<uint8_t> parsePcieBdf(const std::string& bdf)
{
    unsigned int bus = 0;
    unsigned int device = 0;
    unsigned int function = 0;

    std::string_view bdfView{bdf};
    size_t firstColon = bdfView.find(':');
    size_t secondColon = std::string_view::npos;
    if (firstColon != std::string_view::npos)
    {
        secondColon = bdfView.find(':', firstColon + 1);
    }

    if (firstColon == std::string_view::npos)
    {
        throw std::invalid_argument("Bad BDF: " + bdf);
    }

    if (secondColon == std::string_view::npos)
    {
        size_t dot = bdfView.find('.', firstColon + 1);
        if (dot == std::string_view::npos)
        {
            throw std::invalid_argument("Bad BDF: " + bdf);
        }
        bus = parsePcieBdfHexField(bdfView.substr(0, firstColon), bdf);
        device = parsePcieBdfHexField(
            bdfView.substr(firstColon + 1, dot - firstColon - 1), bdf);
        function = parsePcieBdfHexField(bdfView.substr(dot + 1), bdf);
    }
    else
    {
        if (bdfView.find(':', secondColon + 1) != std::string_view::npos)
        {
            throw std::invalid_argument("Bad BDF: " + bdf);
        }
        size_t dot = bdfView.find('.', secondColon + 1);
        if (dot == std::string_view::npos)
        {
            throw std::invalid_argument("Bad BDF: " + bdf);
        }
        (void)parsePcieBdfHexField(bdfView.substr(0, firstColon), bdf);
        bus = parsePcieBdfHexField(
            bdfView.substr(firstColon + 1, secondColon - firstColon - 1), bdf);
        device = parsePcieBdfHexField(
            bdfView.substr(secondColon + 1, dot - secondColon - 1), bdf);
        function = parsePcieBdfHexField(bdfView.substr(dot + 1), bdf);
    }

    if (bus > 0xFF || device > 0x1F || function > 0x7)
    {
        throw std::invalid_argument("BDF out of range: " + bdf);
    }

    auto devfn = static_cast<uint8_t>((device << 3) | function);
    return {static_cast<uint8_t>(bus), devfn};
}

std::shared_ptr<PCIeMCTPDDevice> PCIeMCTPDDevice::from(
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
        throw std::invalid_argument("Not a PCIe device");
    }

    auto mAddress = iface.find("Address");
    auto mInterface = iface.find("Interface");
    auto mName = iface.find("Name");
    auto mStaticEndpointID = iface.find("StaticEndpointID");
    auto mbridgePoolStartEid = iface.find("BridgePoolStartEID");
    auto mbridgePoolEndEid = iface.find("BridgePoolEndEID");
    if (mAddress == iface.end() || mInterface == iface.end() ||
        mName == iface.end())
    {
        throw std::invalid_argument(
            "Configuration object violates MCTPPCIeTarget schema");
    }

    std::vector<std::string> names = getDeviceNames(iface);
    std::string name = names[0];

    auto interface = std::visit(VariantToStringVisitor(), mInterface->second);

    auto sAddress = std::visit(VariantToStringVisitor(), mAddress->second);
    std::vector<uint8_t> address;
    try
    {
        address = parsePcieBdf(sAddress);
    }
    catch (const std::exception&)
    {
        throw std::invalid_argument("Bad PCIe device address (BDF)");
    }

    std::optional<std::uint8_t> staticEID{};
    if (mStaticEndpointID == iface.end())
    {
        info(
            "Info: Key 'StaticEndpointID' is not provided; skipping related processing.");
    }
    else
    {
        auto sStaticEndpointID =
            std::visit(VariantToStringVisitor(), mStaticEndpointID->second);
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
    if (mbridgePoolStartEid != iface.end())
    {
        auto sbridgePoolStartEid =
            std::visit(VariantToStringVisitor(), mbridgePoolStartEid->second);
        std::uint8_t parsedbridgePoolStartEid{};
        auto [dptr, dec] = std::from_chars(
            sbridgePoolStartEid.data(),
            sbridgePoolStartEid.data() + sbridgePoolStartEid.size(),
            parsedbridgePoolStartEid);
        if (dec != std::errc{})
        {
            throw std::invalid_argument("Bad BridgePool Start address");
        }
        bridgePoolStartEid = parsedbridgePoolStartEid;
    }

    std::optional<std::uint8_t> bridgePoolEndEid{};
    if (mbridgePoolEndEid != iface.end())
    {
        auto sbridgePoolEndEid =
            std::visit(VariantToStringVisitor(), mbridgePoolEndEid->second);
        std::uint8_t parsedbridgePoolEndEid{};
        auto [eptr, eec] =
            std::from_chars(sbridgePoolEndEid.data(),
                            sbridgePoolEndEid.data() + sbridgePoolEndEid.size(),
                            parsedbridgePoolEndEid);
        if (eec != std::errc{})
        {
            throw std::invalid_argument("Bad BridgePool End address");
        }
        bridgePoolEndEid = parsedbridgePoolEndEid;
    }

    auto pollingInterval = getPollingInterval(iface);

    try
    {
        if (staticEID.has_value() && bridgePoolStartEid.has_value())
        {
            return std::make_shared<PCIeMCTPDDevice>(
                connection, name, interface, address, staticEID.value(),
                bridgePoolStartEid.value(), bridgePoolEndEid, pollingInterval,
                names);
        }
        if (staticEID.has_value())
        {
            return std::make_shared<PCIeMCTPDDevice>(
                connection, name, interface, address, staticEID.value(),
                std::nullopt, bridgePoolEndEid, pollingInterval, names);
        }
        return std::make_shared<PCIeMCTPDDevice>(
            connection, name, interface, address, std::nullopt, std::nullopt,
            bridgePoolEndEid, pollingInterval, names);
    }
    catch (const MCTPException& ex)
    {
        warning(
            "Failed to create PCIeMCTPDDevice at [ interface: {PCIE_INTERFACE}, BDF: {PCIE_BDF} ]: {EXCEPTION}",
            "PCIE_INTERFACE", interface, "PCIE_BDF", sAddress, "EXCEPTION", ex);
        return {};
    }
}
