// SPDX-License-Identifier: Apache-2.0
//
// hmcreadiness
//
// Publishes a persistent xyz.openbmc_project.State.FeatureReady object whose
// State reflects HMC readiness, so entity-manager's HMC_BRIDGE.json probe can
// gate the HMC bridge chassis (and their SPDMResponder interface) on the HMC
// actually being ready.
//
// Readiness is derived from the lifecycle of a "recovery" object that
// nvidia-code-mgmt's fw-status service maintains from the HMC_READY-I GPIO:
//
//     recovery object present  -> HMC NOT ready (StandbyOffline)
//     recovery object absent   -> HMC ready      (Enabled)
//
// fw-status deletes that object when the HMC is ready and (re)creates it when
// not. Neither entity-manager (no "absent" probe) nor CSM (no InterfacesRemoved
// watch, throws on absent reads) can consume that directly. This small service
// does what they can't: query current state at startup (boot-order
// independent) and watch BOTH InterfacesAdded and InterfacesRemoved,
// republishing the result as one always-present, non-latching FeatureReady
// property.
//
// Configuration (all optional; defaults target the vr-nvl-bmc HMC):
//   --recovery-path       <path>   object whose presence == "not ready"
//   --recovery-interface  <iface>  interface used for the initial presence
//   probe
//   --object-path         <path>   where we publish the FeatureReady object
//   --feature-type        <enum>   FeatureReady.FeatureType value to advertise

#include <boost/asio/io_context.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{

// What we publish (fixed: the bus name must match the unit's BusName=).
constexpr const char* busName = "xyz.openbmc_project.State.HMCReady";
constexpr const char* featureReadyIface =
    "xyz.openbmc_project.State.FeatureReady";
constexpr const char* stateEnabled =
    "xyz.openbmc_project.State.FeatureReady.States.Enabled";
constexpr const char* stateStandby =
    "xyz.openbmc_project.State.FeatureReady.States.StandbyOffline";

// ObjectMapper (for the initial-state query).
constexpr const char* mapperBus = "xyz.openbmc_project.ObjectMapper";
constexpr const char* mapperPath = "/xyz/openbmc_project/object_mapper";
constexpr const char* mapperIface = "xyz.openbmc_project.ObjectMapper";

struct Config
{
    std::string recoveryPath =
        "/xyz/openbmc_project/software/FW_RECOVERY_HGX_FW_BMC_0";
    std::string recoveryIface =
        "xyz.openbmc_project.State.Decorator.OperationalStatus";
    std::string objectPath = "/xyz/openbmc_project/state/hmc/FW_HMC_0";
    std::string featureType =
        "xyz.openbmc_project.State.FeatureReady.FeatureTypes.MC";
};

Config parseArgs(int argc, char* argv[])
{
    Config cfg;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string key = argv[i];
        const std::string val = argv[i + 1];
        if (key == "--recovery-path")
        {
            cfg.recoveryPath = val;
        }
        else if (key == "--recovery-interface")
        {
            cfg.recoveryIface = val;
        }
        else if (key == "--object-path")
        {
            cfg.objectPath = val;
        }
        else if (key == "--feature-type")
        {
            cfg.featureType = val;
        }
        else
        {
            lg2::warning("Ignoring unknown argument {ARG}", "ARG", key);
        }
    }
    return cfg;
}

} // namespace

int main(int argc, char* argv[])
{
    const Config cfg = parseArgs(argc, argv);
    lg2::info(
        "hmcreadiness: watching {RECOVERY} ({IFACE}); publishing {OBJ} ({FT})",
        "RECOVERY", cfg.recoveryPath, "IFACE", cfg.recoveryIface, "OBJ",
        cfg.objectPath, "FT", cfg.featureType);

    boost::asio::io_context io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    systemBus->request_name(busName);
    sdbusplus::asio::object_server objectServer(systemBus);

    auto iface = objectServer.add_interface(cfg.objectPath, featureReadyIface);
    iface->register_property("FeatureType", cfg.featureType);
    // Default to not-ready: fail safe until we positively determine readiness.
    iface->register_property("State", std::string(stateStandby));
    iface->initialize();

    // Track the last applied value so we only log/signal on real transitions
    // (avoids depending on any get_property() read-back API).
    auto last = std::make_shared<std::optional<bool>>(std::nullopt);
    auto applyReadiness = [iface, last](bool ready) {
        if (last->has_value() && **last == ready)
        {
            return;
        }
        *last = ready;
        lg2::info("HMC readiness -> {STATE}", "STATE",
                  ready ? "Enabled" : "StandbyOffline");
        iface->set_property("State",
                            std::string(ready ? stateEnabled : stateStandby));
    };

    // Initial state from the live bus (boot-order independent): ask the mapper
    // whether the recovery object exists right now.
    systemBus->async_method_call(
        [applyReadiness](
            const boost::system::error_code& ec,
            const std::vector<std::pair<std::string, std::vector<std::string>>>&
                owners) {
            // ec set / empty owners => object not present => HMC ready.
            applyReadiness(ec || owners.empty());
        },
        mapperBus, mapperPath, mapperIface, "GetObject", cfg.recoveryPath,
        std::vector<std::string>{cfg.recoveryIface});

    // Live updates: recovery object appears -> not ready; disappears -> ready.
    const std::string watchPath = cfg.recoveryPath;
    auto onAdded = [applyReadiness, watchPath](sdbusplus::message_t& msg) {
        sdbusplus::object_path path;
        msg.read(path);
        if (path.str == watchPath)
        {
            applyReadiness(false);
        }
    };
    auto onRemoved = [applyReadiness, watchPath](sdbusplus::message_t& msg) {
        sdbusplus::object_path path;
        msg.read(path);
        if (path.str == watchPath)
        {
            applyReadiness(true);
        }
    };

    sdbusplus::bus::match_t addedMatch(
        static_cast<sdbusplus::bus_t&>(*systemBus),
        sdbusplus::bus::match::rules::interfacesAdded(), std::move(onAdded));
    sdbusplus::bus::match_t removedMatch(
        static_cast<sdbusplus::bus_t&>(*systemBus),
        sdbusplus::bus::match::rules::interfacesRemoved(),
        std::move(onRemoved));

    io.run();
    return 0;
}
