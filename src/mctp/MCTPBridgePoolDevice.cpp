#include "MCTPBridgePoolDevice.hpp"

#include "MCTPEndpoint.hpp"
#include "Utils.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

PHOSPHOR_LOG2_USING;

namespace
{
constexpr const char* mctpdBusName = "au.com.codeconstruct.MCTP1";
constexpr const char* mctpdControlPath = "/au/com/codeconstruct/mctp1";
constexpr const char* mctpdEndpointControlInterface =
    "au.com.codeconstruct.MCTP.Endpoint1";
constexpr const char* mctpdBridgeInterface =
    "au.com.codeconstruct.MCTP.Bridge1";
constexpr const char* associationDefinitionsInterface =
    "xyz.openbmc_project.Association.Definitions";

// Parse the network id from a mctpd endpoint path of the form
// /au/com/codeconstruct/mctp1/networks/<N>/endpoints/<EID>.
std::optional<int> networkFromMctpdEndpointPath(const std::string& path)
{
    constexpr std::string_view networks = "/networks/";
    constexpr std::string_view endpoints = "/endpoints/";
    const auto netPos = path.find(networks);
    if (netPos == std::string::npos)
    {
        return std::nullopt;
    }
    const char* begin = path.data() + netPos + networks.size();
    const auto epPos = path.find(endpoints, netPos);
    if (epPos == std::string::npos)
    {
        return std::nullopt;
    }
    const char* end = path.data() + epPos;
    int v = 0;
    auto [ptr, ec] = std::from_chars(begin, end, v);
    if (ec != std::errc{} || ptr != end)
    {
        return std::nullopt;
    }
    return v;
}
} // namespace

BridgePoolMCTPDevice::BridgePoolMCTPDevice(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    std::string name, std::string bridgeName, uint8_t poolIndex) :
    connection(connection), name(std::move(name)),
    bridgeName(std::move(bridgeName)), poolIndex(poolIndex)
{
    info(
        "Creating BridgePoolMCTPDevice: {NAME}, bridge: {BRIDGE}, poolIndex: {INDEX}",
        "NAME", this->name, "BRIDGE", this->bridgeName, "INDEX",
        static_cast<int>(poolIndex));
}

bool BridgePoolMCTPDevice::match(const std::set<std::string>& interfaces)
{
    return interfaces.contains(configInterfaceName(configType));
}

std::optional<SensorBaseConfigMap> BridgePoolMCTPDevice::match(
    const SensorData& config)
{
    auto iface = config.find(configInterfaceName(configType));
    if (iface == config.end())
    {
        return std::nullopt;
    }
    return iface->second;
}

std::shared_ptr<BridgePoolMCTPDevice> BridgePoolMCTPDevice::from(
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
        throw std::invalid_argument("Not an MCTPBridgePoolDevice");
    }

    auto mName = iface.find("Name");
    auto mBridgeName = iface.find("BridgeName");
    auto mPoolIndex = iface.find("PoolIndex");
    if (mName == iface.end() || mBridgeName == iface.end() ||
        mPoolIndex == iface.end())
    {
        throw std::invalid_argument(
            "Configuration object violates MCTPBridgePoolDevice schema");
    }

    std::string name = std::visit(VariantToStringVisitor(), mName->second);
    std::string bridgeName =
        std::visit(VariantToStringVisitor(), mBridgeName->second);

    auto sPoolIndex = std::visit(VariantToStringVisitor(), mPoolIndex->second);
    std::uint8_t parsedPoolIndex{};
    auto [cptr, cec] = std::from_chars(
        sPoolIndex.data(), sPoolIndex.data() + sPoolIndex.size(),
        parsedPoolIndex);
    if (cec != std::errc{})
    {
        throw std::invalid_argument("Bad PoolIndex");
    }

    return std::make_shared<BridgePoolMCTPDevice>(connection, name, bridgeName,
                                                  parsedPoolIndex);
}

std::string BridgePoolMCTPDevice::predictedEndpointPath() const
{
    return std::format("{}/networks/{}/endpoints/{}", mctpdControlPath,
                       networkId.value_or(0),
                       static_cast<int>(bridgedEid.value_or(0)));
}

bool BridgePoolMCTPDevice::resolveBridge()
{
    // Find every mctpd endpoint implementing Bridge1 via the ObjectMapper, then
    // pick the one whose configured_by association resolves to an EM object
    // named bridgeName. Read its Bridge1.PoolStart/PoolEnd and the network id
    // from its path; compute bridgedEid = PoolStart + poolIndex.
    //
    // Properties are read with explicit, narrow types (uint8_t for the pool
    // bounds, std::vector<Association> for the association list) rather than via
    // GetManagedObjects, because the project-wide BasicVariantType used by
    // ManagedObjectType cannot represent the a(sss) Associations property.
    GetSubTreeType subtree;
    try
    {
        auto method = connection->new_method_call(mapper::busName, mapper::path,
                                                  mapper::interface,
                                                  mapper::subtree);
        method.append(std::string(mctpdControlPath), 0,
                      std::vector<std::string>{mctpdBridgeInterface});
        auto reply = connection->call(method);
        reply.read(subtree);
    }
    catch (const std::exception& e)
    {
        // mctpd has not published any bridge endpoint yet — defer.
        debug(
            "BridgePoolMCTPDevice {NAME}: no Bridge1 endpoints found, deferring: {ERROR}",
            "NAME", name, "ERROR", e.what());
        return false;
    }

    for (const auto& [bridgePath, services] : subtree)
    {
        // Confirm this bridge endpoint belongs to the configured bridge by
        // resolving its configured_by association to the EM object and
        // comparing the trailing Name segment to bridgeName.
        std::vector<Association> associations;
        try
        {
            auto method = connection->new_method_call(
                mctpdBusName, bridgePath.c_str(), properties::interface,
                properties::get);
            method.append(associationDefinitionsInterface, "Associations");
            auto reply = connection->call(method);
            std::variant<std::vector<Association>> value;
            reply.read(value);
            associations = std::get<std::vector<Association>>(value);
        }
        catch (const std::exception&)
        {
            // No configured_by on the bridge endpoint yet — cannot match it to
            // an EM name; try the next candidate.
            continue;
        }

        bool bridgeMatches = false;
        for (const auto& [forward, backward, endpoint] : associations)
        {
            if (forward != "configured_by")
            {
                continue;
            }
            // endpoint is the EM object path; its trailing segment is the
            // bridge's Name.
            const auto slash = endpoint.find_last_of('/');
            std::string emName = (slash == std::string::npos)
                                     ? endpoint
                                     : endpoint.substr(slash + 1);
            bridgeMatches = (emName == bridgeName);
            break;
        }
        if (!bridgeMatches)
        {
            continue;
        }

        // Read PoolStart / PoolEnd off the Bridge1 interface.
        std::optional<uint8_t> poolStart;
        std::optional<uint8_t> poolEnd;
        try
        {
            auto method = connection->new_method_call(
                mctpdBusName, bridgePath.c_str(), properties::interface,
                properties::get);
            method.append(mctpdBridgeInterface, "PoolStart");
            auto reply = connection->call(method);
            std::variant<uint8_t> value;
            reply.read(value);
            poolStart = std::get<uint8_t>(value);
        }
        catch (const std::exception&)
        {
            // Bridge1 present but PoolStart not yet populated
            // (endpoint_allocate_eids still in flight) — defer.
            debug(
                "BridgePoolMCTPDevice {NAME}: Bridge1.PoolStart not yet present, deferring",
                "NAME", name);
            return false;
        }

        try
        {
            auto method = connection->new_method_call(
                mctpdBusName, bridgePath.c_str(), properties::interface,
                properties::get);
            method.append(mctpdBridgeInterface, "PoolEnd");
            auto reply = connection->call(method);
            std::variant<uint8_t> value;
            reply.read(value);
            poolEnd = std::get<uint8_t>(value);
        }
        catch (const std::exception&)
        {
            // PoolEnd is optional for resolution; only used for bounds check.
        }

        auto net = networkFromMctpdEndpointPath(bridgePath);
        if (!net)
        {
            error(
                "BridgePoolMCTPDevice {NAME}: could not parse network from bridge path '{PATH}'",
                "NAME", name, "PATH", bridgePath);
            return false;
        }

        // Bounds check: PoolIndex must fall within [0, PoolEnd - PoolStart].
        if (poolEnd)
        {
            if (*poolEnd < *poolStart || poolIndex > (*poolEnd - *poolStart))
            {
                error(
                    "BridgePoolMCTPDevice {NAME}: PoolIndex {INDEX} out of range for bridge {BRIDGE} (PoolStart {START}, PoolEnd {END}); skipping",
                    "NAME", name, "INDEX", static_cast<int>(poolIndex),
                    "BRIDGE", bridgeName, "START",
                    static_cast<int>(*poolStart), "END",
                    static_cast<int>(*poolEnd));
                return false;
            }
        }

        networkId = *net;
        bridgedEid = static_cast<uint8_t>(*poolStart + poolIndex);
        resolved = true;
        info(
            "BridgePoolMCTPDevice {NAME}: resolved bridge {BRIDGE} -> network {NET}, bridgedEid {EID} (PoolStart {START} + PoolIndex {INDEX})",
            "NAME", name, "BRIDGE", bridgeName, "NET", *net, "EID",
            static_cast<int>(*bridgedEid), "START",
            static_cast<int>(*poolStart), "INDEX",
            static_cast<int>(poolIndex));
        return true;
    }

    // Bridge endpoint / Bridge1 not present yet — defer.
    debug(
        "BridgePoolMCTPDevice {NAME}: bridge {BRIDGE} not set up yet, deferring",
        "NAME", name, "BRIDGE", bridgeName);
    return false;
}

void BridgePoolMCTPDevice::notifyEndpointPresent()
{
    if (setupCallback)
    {
        auto cb = std::exchange(setupCallback, nullptr);
        cb(std::error_code{}, shared_from_this());
    }
}

void BridgePoolMCTPDevice::armWatches()
{
    using namespace sdbusplus::bus::match;

    const std::string predicted = predictedEndpointPath();

    const std::string addedMatchSpec =
        rules::sender(mctpdBusName) + rules::interfacesAdded() +
        rules::argNpath(0, predicted);

    endpointAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*connection), addedMatchSpec,
        [weak{weak_from_this()}](sdbusplus::message_t& msg) {
            if (auto self = weak.lock())
            {
                self->onEndpointAdded(msg);
            }
        });

    const std::string removedMatchSpec =
        rules::sender(mctpdBusName) + rules::interfacesRemoved() +
        rules::argNpath(0, predicted);

    endpointRemovedMatch = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*connection), removedMatchSpec,
        [weak{weak_from_this()}](sdbusplus::message_t& msg) {
            if (auto self = weak.lock())
            {
                self->onEndpointRemoved(msg);
            }
        });
}

void BridgePoolMCTPDevice::setup(
    std::function<void(const std::error_code& ec,
                       const std::shared_ptr<MCTPEndpoint>& ep)>&& added)
{
    // Note: this consumer does NOT call AssignEndpoint. mctpd's
    // peer_endpoint_poll already creates the pool-member endpoint behind the
    // bridge; we only resolve the bridged EID and publish configured_by on it.
    setupCallback = std::move(added);

    if (!resolveBridge())
    {
        // Defer: report failure so the reactor retries on the next tick /
        // subsequent EM/mctpd InterfacesAdded cycle (SADD §3.3.3).
        auto cb = std::exchange(setupCallback, nullptr);
        if (cb)
        {
            cb(std::make_error_code(std::errc::host_unreachable), nullptr);
        }
        return;
    }

    // Watch the predicted endpoint for hot (re)appearance / removal.
    armWatches();

    // Warm boot: the pool-member endpoint may already exist. Probe mctpd for
    // the predicted path's Endpoint1 interface and publish configured_by
    // immediately if present.
    const std::string predicted = predictedEndpointPath();
    bool present = false;
    try
    {
        auto method = connection->new_method_call(
            mctpdBusName, predicted.c_str(), "org.freedesktop.DBus.Introspectable",
            "Introspect");
        auto reply = connection->call(method);
        std::string xml;
        reply.read(xml);
        present =
            xml.find(mctpdEndpointControlInterface) != std::string::npos;
    }
    catch (const std::exception& e)
    {
        // Object does not exist yet — rely on the InterfacesAdded watch.
        debug(
            "BridgePoolMCTPDevice {NAME}: predicted endpoint not present yet, relying on watch: {ERROR}",
            "NAME", name, "ERROR", e.what());
    }

    if (present)
    {
        info(
            "BridgePoolMCTPDevice {NAME}: pool-member endpoint {PATH} already present; publishing configured_by",
            "NAME", name, "PATH", predicted);
        notifyEndpointPresent();
        return;
    }

    info(
        "BridgePoolMCTPDevice {NAME}: waiting for pool-member endpoint {PATH} to appear",
        "NAME", name, "PATH", predicted);
}

void BridgePoolMCTPDevice::onEndpointAdded(sdbusplus::message_t& msg)
{
    auto [path, interfaces] =
        msg.unpack<sdbusplus::message::object_path, SensorData>();

    if (!interfaces.contains(mctpdEndpointControlInterface))
    {
        return;
    }
    if (path.str != predictedEndpointPath())
    {
        return;
    }

    info(
        "BridgePoolMCTPDevice {NAME}: pool-member endpoint {PATH} appeared; publishing configured_by",
        "NAME", name, "PATH", path.str);
    notifyEndpointPresent();
}

void BridgePoolMCTPDevice::onEndpointRemoved(sdbusplus::message_t& msg)
{
    auto [path, interfaces] =
        msg.unpack<sdbusplus::message::object_path, std::set<std::string>>();

    if (!interfaces.contains(mctpdEndpointControlInterface))
    {
        return;
    }
    if (path.str != predictedEndpointPath())
    {
        return;
    }

    info(
        "BridgePoolMCTPDevice {NAME}: pool-member endpoint {PATH} removed; dropping configured_by and re-arming watch",
        "NAME", name, "PATH", path.str);

    // Notify the reactor so it disassociates configured_by; the watch stays
    // armed for re-publication when the endpoint reappears (SADD §3.3.3).
    if (notifyRemoved)
    {
        notifyRemoved(shared_from_this());
    }
}

void BridgePoolMCTPDevice::remove()
{
    info("Removing BridgePoolMCTPDevice: {NAME}", "NAME", name);

    endpointAddedMatch.reset();
    endpointRemovedMatch.reset();

    if (notifyRemoved)
    {
        notifyRemoved(shared_from_this());
    }
}

std::string BridgePoolMCTPDevice::describe() const
{
    return std::format("BridgePool[{}, bridge={}, poolIndex={}, eid={}]", name,
                       bridgeName, static_cast<int>(poolIndex),
                       bridgedEid ? static_cast<int>(*bridgedEid) : -1);
}

std::optional<std::string> BridgePoolMCTPDevice::getNameForEid(
    uint8_t queryEid) const
{
    if (bridgedEid && *bridgedEid == queryEid)
    {
        return name;
    }
    return std::nullopt;
}

std::size_t BridgePoolMCTPDevice::id() const
{
    std::size_t h1 = std::hash<std::string>{}(name);
    std::size_t h2 = std::hash<std::string>{}(bridgeName);
    return h1 ^ (h2 << 1) ^ (static_cast<std::size_t>(poolIndex) << 2);
}

int BridgePoolMCTPDevice::network() const
{
    return networkId.value_or(0);
}

uint8_t BridgePoolMCTPDevice::eid() const
{
    return bridgedEid.value_or(0);
}

void BridgePoolMCTPDevice::subscribe([[maybe_unused]] Event&& degraded,
                                     [[maybe_unused]] Event&& available,
                                     Event&& removed)
{
    notifyRemoved = std::move(removed);
}
