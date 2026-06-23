#pragma once

#include "Domain.hpp"
#include "EntityManagerInterface.hpp"
#include "HardwareWriteProtectedControl.hpp"
#include "Protectors/Gpio.hpp"
#include "Protectors/Protector.hpp"
#include "ReactiveGraph.hpp"
#include "SoftwareComponent.hpp"

#include <sdbusplus/async.hpp>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nvidia::write_protect
{

/// Entity Manager configuration interface for write-protect groups.
inline constexpr auto writeProtectGroupConfigIntf =
    "xyz.openbmc_project.Configuration.WriteProtectGroup";

/// Entity Manager configuration interface for hardware write-protect controls.
inline constexpr auto hwWpCtrlConfigIntf =
    "xyz.openbmc_project.Configuration.HardwareWriteProtectedControl";

/// Entity Manager configuration interface for write-protect domains (with
/// chassis association).
inline constexpr auto writeProtectDomainConfigIntf =
    "xyz.openbmc_project.Configuration.WriteProtectDomain";

inline constexpr auto writeProtectInputIntf =
    "xyz.openbmc_project.Configuration.WriteProtectInput";

/**
 * @brief Orchestrates write-protection groups using a reactive graph.
 *
 * Listens for Entity Manager inventory events.  Each group configuration
 * (@c WriteProtectGroup, @c HardwareWriteProtectedControl, or
 * @c WriteProtectDomain) defines a set of sources combined with a boolean
 * operator (OR or AND) to compute an effective write-protection value.
 * That value is pushed to D-Bus facades (@c Domain,
 * @c HardwareWriteProtectedControl, @c SoftwareComponent) through graph
 * observers.  The EM Type selects which facade is created:
 *
 *  - @c WriteProtectGroup — no D-Bus facade; a reusable building block
 *    that other groups can reference by name
 *  - @c HardwareWriteProtectedControl — creates a
 *    @c HardwareWriteProtectedControl on the chassis path
 *  - @c WriteProtectDomain — creates a settable @c Domain at
 *    @c /xyz/openbmc_project/state/<Name> with a chassis association
 *
 * GPIO sources are configured via @c WriteProtectInput configs, which
 * create both a source node and a named group entry.
 *
 * Sources and groups are stored as @c shared_ptr so that coroutines
 * operating across async boundaries keep objects alive independently of
 * the owning maps.
 */
class DomainManager
{
  public:
    explicit DomainManager(sdbusplus::async::context& ctx) : ctx(ctx) {}

    /**
     * @brief Begin listening for Entity Manager configuration and create
     *        groups for any existing inventory.
     */
    auto start() -> sdbusplus::async::task<>;

    /**
     * @brief Set the write-protection value of the source identified by
     *        @p nodeId, then update the reactive graph and propagate.
     *
     * Looks up the source registered for @p nodeId via weak_ptr (O(1)),
     * co_awaits @c protector->set(value), and on success updates the
     * graph.
     *
     * Called by Domain::method_call(SetWriteProtected).
     *
     * @return Success, or an Error describing the failure.
     */
    auto setWriteProtected(SourceNodeId nodeId, bool value)
        -> sdbusplus::async::task<Result<void>>;

  private:
    using EntityManager = entity_manager::EntityManagerInterface;
    using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;

    static constexpr auto sourceChangedCoolOffInterval =
        std::chrono::milliseconds(1000);

    struct Source
    {
        SourceNodeId nodeId;
        std::unique_ptr<Protector> protector;
        std::optional<TimePoint> lastChanged = {};
    };

    /// A write-protection group with its graph node, D-Bus facades, and
    /// software inventory objects.
    struct Group
    {
        std::string name;
        NodeId outputNode;

        std::unique_ptr<Domain> domain;
        std::unique_ptr<HardwareWriteProtectedControl> hwWpCtrl;
        std::map<std::string, std::unique_ptr<SoftwareComponent>> software;
    };

    /// Tracks an entity source waiting for resolution as either a local
    /// group or a remote D-Bus proxy.
    struct PendingEntityLink
    {
        NodeId targetNode;
        SourceNodeId sourceNode;
    };

    Graph graph;
    std::map<std::string, std::shared_ptr<Source>> sources;
    std::map<std::string, std::shared_ptr<Group>> groups;

    /// Entity sources waiting for resolution: maps entity name to the list
    /// of compute nodes that depend on it and the placeholder source nodes.
    std::map<std::string, std::vector<PendingEntityLink>> pendingEntityLinks;

    /// Per-entity async scopes for cancellable entity source initialization.
    /// Each scope provides a stop token to the spawned init task; calling
    /// request_stop() on the scope cancels the in-flight DbusProxy::create.
    std::map<std::string, exec::async_scope> entityInitScopes;

    /// Routing table for SetWriteProtected: maps a source node to its
    /// owning Source.  Stored as weak_ptr so a removed source is detected
    /// rather than left dangling.
    std::unordered_map<SourceNodeId, std::weak_ptr<Source>> sourcesByNodeId;

    sdbusplus::async::context& ctx;
    std::unique_ptr<EntityManager> entityManager;

    /// @brief Dispatch an Entity Manager inventory-added event.
    void processInventoryAdded(const sdbusplus::object_path& objectPath,
                               const std::string& interfaceName);

    /// @brief Handle an Entity Manager inventory-removed event.
    void processInventoryRemoved(const sdbusplus::object_path& objectPath,
                                 const std::string& interfaceName);

    /// @brief Read a group config, build graph topology, and create
    ///        D-Bus facades based on the EM config type.
    auto addGroup(sdbusplus::object_path objectPath, std::string interface)
        -> sdbusplus::async::task<>;

    auto addGpioGroup(sdbusplus::object_path objectPath, std::string interface)
        -> sdbusplus::async::task<>;

    /// @brief Return the Source for @p identifier, creating a graph node
    ///        if one does not already exist.
    /// @return The source (shared) and @c true if it was newly created.
    auto getOrCreateSource(const std::string& identifier)
        -> std::pair<std::shared_ptr<Source>, bool>;

    /// @brief Build a GPIO protector for @p gpioLine and activate it.
    auto initializeGpioSource(std::shared_ptr<Source> source, std::string name,
                              GpioConfig config) -> sdbusplus::async::task<>;

    /// @brief Wait for the D-Bus object to appear, build a DbusProxy
    ///        protector for @p entityName, and activate it.
    auto initializeEntitySource(std::shared_ptr<Source> source,
                                std::string entityName)
        -> sdbusplus::async::task<>;

    /// @brief Read a source's initial value, register it in the routing
    ///        table, propagate through the graph, and start the monitor
    ///        coroutine.
    auto activateSource(std::shared_ptr<Source> source, std::string name)
        -> sdbusplus::async::task<>;

    /// @brief Continuously await protector changes and propagate through
    ///        the graph.
    auto monitorSource(std::shared_ptr<Source> source, std::string name)
        -> sdbusplus::async::task<>;

    /// @brief Create a SoftwareComponent, subscribe it to the group's
    ///        output node, and set its initial value.
    void addSoftwareToGroup(Group& group, const std::string& name);

    /// @brief Resolve any pending entity links that reference this group by
    /// name.
    /// When a group appears, it takes priority: its output node is
    /// connected directly and the placeholder D-Bus proxy source node is
    /// deactivated.
    void resolveGroupLinks(const Group& group, const std::string& name);

    void spawnInit(const std::string& id, sdbusplus::async::task<> task);

    /// @brief Spawn a cancellable entity source initialization.
    ///
    /// Creates a per-entity async_scope and delegates to spawnInit with a
    /// wrapper coroutine.  The scope's stop token is propagated to the
    /// spawned task so that resolveGroupLinks can cancel it.
    void spawnEntityInit(const std::string& name,
                         std::shared_ptr<Source> source);

    /// @brief Wrapper coroutine that spawns initializeEntitySource into
    ///        the entity's async_scope and waits for completion.
    auto scopedEntityInit(std::shared_ptr<Source> source,
                          std::string entityName) -> sdbusplus::async::task<>;
};

} // namespace nvidia::write_protect
