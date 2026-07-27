/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Discovery daemon for Intel CPU temperature sensors reached over in-kernel
// MCTP (PECI-over-MCTP). Discovery model mirrors nvidia-gpu:
//   1. entity-manager provides the existing "XeonCPU" configuration. All we
//      take from it is the sensor Name (and optional CpuID for DIMM naming).
//   2. MCTP endpoints are discovered from mctpd via the ObjectMapper, and we
//      also watch InterfacesAdded so sockets that enumerate later are picked
//      up live.
//   3. Each endpoint that advertises the vendor-defined message type is probed
//      with a PECI ping; only endpoints that answer as Intel become sensors.

#include "IntelCPUMctpSensor.hpp"
#include "MctpRequester.hpp"
#include "PeciMctp.hpp"
#include "Thresholds.hpp"
#include "Utils.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/container/flat_map.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace
{

constexpr const char* sensorType = "XeonCPU";
constexpr const char* mctpEndpointIface = "xyz.openbmc_project.MCTP.Endpoint";
// mctpd (codeconstruct) exposes its endpoints under this path.
constexpr const char* mctpdSearchPath = "/au/com/codeconstruct/";

// Base configuration pulled from entity-manager. Only the name is required;
// cpuId is used purely for DIMM sensor naming and defaults to 0.
struct CpuConfig
{
    std::string name;
    std::string path;
    int cpuId{0};
};

// One sensor per CPU, keyed by the entity-manager config path.
using CpuSensorMap =
    boost::container::flat_map<std::string,
                               std::shared_ptr<IntelCPUMctpSensor>>;

// The long-lived objects every discovery step needs. Cheap to copy into
// asynchronous callbacks; all referents outlive the io_context.
struct DiscoveryContext
{
    boost::asio::io_context& io;
    sdbusplus::asio::object_server& objectServer;
    std::shared_ptr<sdbusplus::asio::connection> conn;
    std::weak_ptr<mctp::MctpRequester> requester;
    CpuSensorMap& sensors;
};

std::string sensorKey(const CpuConfig& config)
{
    return "cpu-" + config.path;
}

// Extract the EID from an MCTP.Endpoint's properties, but only if it advertises
// the vendor-defined message type PECI-over-MCTP uses.
std::optional<uint8_t> peciEid(const SensorBaseConfigMap& endpoint)
{
    auto eidIt = endpoint.find("EID");
    if (eidIt == endpoint.end())
    {
        return std::nullopt;
    }
    const auto* eidPtr = std::get_if<uint8_t>(&eidIt->second);
    if (eidPtr == nullptr)
    {
        return std::nullopt;
    }

    auto typesIt = endpoint.find("SupportedMessageTypes");
    if (typesIt == endpoint.end())
    {
        return std::nullopt;
    }
    const auto* typesPtr = std::get_if<std::vector<uint8_t>>(&typesIt->second);
    if (typesPtr == nullptr)
    {
        return std::nullopt;
    }
    if (std::find(typesPtr->begin(), typesPtr->end(), peci_mctp::messageType) ==
        typesPtr->end())
    {
        return std::nullopt;
    }

    return *eidPtr;
}

void probeCandidates(const DiscoveryContext& ctx, const CpuConfig& config,
                     const std::shared_ptr<std::vector<uint8_t>>& eids,
                     size_t idx);

void handlePingResponse(DiscoveryContext ctx, const CpuConfig& config,
                        const std::shared_ptr<std::vector<uint8_t>>& eids,
                        size_t idx, uint8_t eid, const std::error_code& ec,
                        std::span<const uint8_t> response)
{
    if (ec || !peci_mctp::deserializePing(response))
    {
        // Off, unreachable, or not an Intel die: try the next EID.
        lg2::debug("eid {EID} is not the primary Intel die: {ERR}", "EID", eid,
                   "ERR", ec ? ec.message() : "bad reply");
        probeCandidates(ctx, config, eids, idx + 1);
        return;
    }

    const std::string key = sensorKey(config);
    if (ctx.sensors.contains(key))
    {
        return;
    }

    lg2::info("Discovered Intel CPU '{NAME}' on MCTP eid {EID}", "NAME",
              config.name, "EID", eid);

    auto sensor = std::make_shared<IntelCPUMctpSensor>(
        std::string(sensorType), ctx.objectServer, ctx.conn, ctx.io,
        config.name, std::vector<thresholds::Threshold>{}, config.path,
        config.cpuId, true, 0.0, ctx.requester, eid);
    ctx.sensors[key] = sensor;
    sensor->setupRead();
}

// A multi-die CPU exposes one MCTP endpoint (EID) per die, but only the first
// die answers PECI usefully. Probe the candidate EIDs in ascending order and
// lock onto the first one that answers as Intel, creating a single sensor.
void probeCandidates(const DiscoveryContext& ctx, const CpuConfig& config,
                     const std::shared_ptr<std::vector<uint8_t>>& eids,
                     size_t idx)
{
    if (!eids)
    {
        return;
    }
    if (ctx.sensors.contains(sensorKey(config)))
    {
        return; // already discovered on a prior pass
    }
    if (idx >= eids->size())
    {
        lg2::info("No Intel CPU answered PECI for config '{NAME}'", "NAME",
                  config.name);
        return;
    }

    auto req = ctx.requester.lock();
    if (!req)
    {
        return; // requester torn down (e.g. re-instantiated mid-rescan)
    }

    uint8_t eid = (*eids)[idx];
    auto pingBuf =
        std::make_shared<std::array<uint8_t, sizeof(peci_mctp::PingRequest)>>();
    if (!peci_mctp::serializePing(*pingBuf))
    {
        lg2::error("Failed to serialize PECI ping for eid {EID}", "EID", eid);
        return;
    }

    req->sendRecvMsg(
        eid, std::span<const uint8_t>(pingBuf->data(), pingBuf->size()),
        [ctx, config, eids, idx, eid, pingBuf](
            const std::error_code& ec, std::span<const uint8_t> response) {
            handlePingResponse(ctx, config, eids, idx, eid, ec, response);
        });
}

// Called once per MCTP endpoint's GetAll reply. The last one in starts the
// probe, so we only ping after the full candidate list is known.
void handleEndpointProperties(
    const DiscoveryContext& ctx, const CpuConfig& config,
    const std::shared_ptr<std::vector<uint8_t>>& eids,
    const std::shared_ptr<size_t>& pending, const boost::system::error_code& ec,
    const SensorBaseConfigMap& props)
{
    if (!ec)
    {
        if (auto eid = peciEid(props))
        {
            eids->push_back(*eid);
        }
    }
    if (--(*pending) != 0)
    {
        return; // wait for the rest
    }

    // All endpoints read; probe lowest EID first.
    std::sort(eids->begin(), eids->end());
    probeCandidates(ctx, config, eids, 0);
}

void handleMctpSubtree(const DiscoveryContext& ctx, const CpuConfig& config,
                       const boost::system::error_code& ec,
                       const GetSubTreeType& subtree)
{
    if (ec)
    {
        lg2::error("Failed to query mctpd endpoints: {ERR}", "ERR",
                   ec.message());
        return;
    }

    // Collect endpoint (service, path) pairs first so we know how many
    // GetAll replies to wait for before probing.
    std::vector<std::pair<std::string, std::string>> endpoints;
    for (const auto& [objPath, services] : subtree)
    {
        for (const auto& [service, ifaces] : services)
        {
            if (std::find(ifaces.begin(), ifaces.end(), mctpEndpointIface) !=
                ifaces.end())
            {
                endpoints.emplace_back(service, objPath);
            }
        }
    }
    if (endpoints.empty())
    {
        return;
    }

    auto eids = std::make_shared<std::vector<uint8_t>>();
    auto pending = std::make_shared<size_t>(endpoints.size());

    for (const auto& [service, objPath] : endpoints)
    {
        ctx.conn->async_method_call(
            [ctx, config, eids, pending](const boost::system::error_code& ec2,
                                         const SensorBaseConfigMap& props) {
                handleEndpointProperties(ctx, config, eids, pending, ec2,
                                         props);
            },
            service, objPath, "org.freedesktop.DBus.Properties", "GetAll",
            mctpEndpointIface);
    }
}

// Gather every PECI-capable MCTP endpoint mctpd knows about, then probe them in
// ascending EID order so we always bind the CPU's first die. The sensor is
// created once and kept; power-cycle recovery is handled by re-opening the MCTP
// socket, not by tearing the sensor down.
void discoverEndpoints(const DiscoveryContext& ctx, const CpuConfig& config)
{
    if (ctx.sensors.contains(sensorKey(config)))
    {
        return; // already have this CPU
    }

    ctx.conn->async_method_call(
        [ctx, config](const boost::system::error_code& ec,
                      const GetSubTreeType& subtree) {
            handleMctpSubtree(ctx, config, ec, subtree);
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTree", mctpdSearchPath, 0,
        std::vector<std::string>{mctpEndpointIface});
}

std::optional<CpuConfig> parseCpuConfig(const std::string& path,
                                        const SensorBaseConfigMap& cfg)
{
    CpuConfig config;
    config.path = path;
    try
    {
        config.name = loadVariant<std::string>(cfg, "Name");
    }
    catch (const std::invalid_argument&)
    {
        lg2::error("XeonCPU config at {PATH} has no Name", "PATH", path);
        return std::nullopt;
    }
    // CpuID is optional; only used for DIMM naming (defaults to 0).
    auto findCpuId = cfg.find("CpuID");
    if (findCpuId != cfg.end())
    {
        try
        {
            config.cpuId = static_cast<int>(
                std::visit(VariantToUnsignedIntVisitor(), findCpuId->second));
        }
        catch (const std::invalid_argument&)
        {
            lg2::error("XeonCPU config at {PATH} has invalid CpuID", "PATH",
                       path);
        }
    }

    return config;
}

void handleManagedObjects(const DiscoveryContext& ctx,
                          const boost::system::error_code& ec,
                          const ManagedObjectType& resp)
{
    if (ec)
    {
        lg2::error("Failed to get entity-manager configuration: {ERR}", "ERR",
                   ec.message());
        return;
    }

    const std::string cfgIface = configInterfaceName(std::string(sensorType));

    for (const auto& [path, interfaces] : resp)
    {
        auto it = interfaces.find(cfgIface);
        if (it == interfaces.end())
        {
            continue;
        }

        auto config = parseCpuConfig(path.str, it->second);
        if (!config)
        {
            continue;
        }

        lg2::info("Detected XeonCPU config '{NAME}' at {PATH}", "NAME",
                  config->name, "PATH", path.str);

        discoverEndpoints(ctx, *config);
    }
}

// Load the XeonCPU configuration from entity-manager, then kick off endpoint
// discovery. If no XeonCPU config is present the daemon stays idle.
void createSensors(const DiscoveryContext& ctx)
{
    ctx.conn->async_method_call(
        [ctx](const boost::system::error_code& ec,
              const ManagedObjectType& resp) {
            handleManagedObjects(ctx, ec, resp);
        },
        "xyz.openbmc_project.EntityManager", "/xyz/openbmc_project/inventory",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
}

// MCTP endpoints come and go across host power cycles, which leaves the
// AF_MCTP socket bound to stale routing. Re-instantiate the MctpRequester so it
// opens a fresh socket, then re-point the (persistent) sensors at it — they
// show Available=false while the host is off and resume on the new socket when
// it returns.
void rescan(boost::asio::io_context& io,
            sdbusplus::asio::object_server& objectServer,
            const std::shared_ptr<sdbusplus::asio::connection>& conn,
            CpuSensorMap& sensors,
            std::shared_ptr<mctp::MctpRequester>& requester)
{
    requester =
        std::make_shared<mctp::MctpRequester>(io, peci_mctp::messageType);
    requester->start();

    for (auto& [key, sensor] : sensors)
    {
        sensor->setRequester(requester);
        sensor->restart();
    }

    createSensors({io, objectServer, conn, requester, sensors});
}

} // namespace

int main()
{
    boost::asio::io_context io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::asio::object_server objectServer(systemBus, true);
    objectServer.add_manager("/xyz/openbmc_project/sensors");
    // Same bus name as the legacy PECI daemon: this is the same CPU telemetry
    // over a different transport, and only one of the two runs on a platform.
    systemBus->request_name("xyz.openbmc_project.IntelCpuMctpSensor");

    CpuSensorMap sensors;

    auto mctpRequester =
        std::make_shared<mctp::MctpRequester>(io, peci_mctp::messageType);
    mctpRequester->start();

    boost::asio::post(io, [&]() {
        createSensors({io, objectServer, systemBus, mctpRequester, sensors});
    });

    // Debounce bursts of configuration / endpoint changes into one rescan.
    boost::asio::steady_timer rescanTimer(io);
    auto scheduleRescan = [&]() {
        rescanTimer.expires_after(std::chrono::seconds(1));
        rescanTimer.async_wait([&](const boost::system::error_code& ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                return;
            }
            rescan(io, objectServer, systemBus, sensors, mctpRequester);
        });
    };

    // A powered-off CPU drops its MCTP endpoint and a powered-on one gets it
    // back; re-run discovery (and reopen the socket) on any host power change.
    // Registered before any sensor is created so this callback (not a sensor's
    // empty setupPowerMatch) owns the single power match.
    setupPowerMatchCallback(systemBus, [&scheduleRescan](PowerState, bool) {
        scheduleRescan();
    });

    // Re-run discovery when the XeonCPU entity-manager config changes.
    std::function<void(sdbusplus::message_t&)> eventHandler =
        [&scheduleRescan](sdbusplus::message_t&) { scheduleRescan(); };
    static constexpr auto deviceTypes =
        std::to_array<const char*>({sensorType});
    std::vector<std::unique_ptr<sdbusplus::bus::match_t>> matches =
        setupPropertiesChangedMatches(*systemBus, deviceTypes, eventHandler);

    // Live-watch mctpd: pick up CPUs whose MCTP endpoint appears after startup.
    matches.emplace_back(std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*systemBus),
        "type='signal',interface='org.freedesktop.DBus.ObjectManager',"
        "member='InterfacesAdded'",
        [&scheduleRescan](sdbusplus::message_t& msg) {
            sdbusplus::message::object_path objPath;
            SensorData interfaces;
            msg.read(objPath, interfaces);
            if (interfaces.find(mctpEndpointIface) != interfaces.end())
            {
                scheduleRescan();
            }
        }));

    // An MCTP endpoint going away (power off) also triggers a rescan, which
    // reopens the socket so the daemon recovers when it returns.
    matches.emplace_back(std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*systemBus),
        "type='signal',interface='org.freedesktop.DBus.ObjectManager',"
        "member='InterfacesRemoved'",
        [&scheduleRescan](sdbusplus::message_t& msg) {
            sdbusplus::message::object_path objPath;
            std::vector<std::string> interfaces;
            msg.read(objPath, interfaces);
            if (std::find(interfaces.begin(), interfaces.end(),
                          mctpEndpointIface) != interfaces.end())
            {
                scheduleRescan();
            }
        }));

    io.run();
    return 0;
}
