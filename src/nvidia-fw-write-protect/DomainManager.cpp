#include "DomainManager.hpp"

#include "GpioInputConfig.hpp"
#include "GroupConfig.hpp"
#include "Protectors/DbusProxy.hpp"
#include "Utility.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/async.hpp>

#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

PHOSPHOR_LOG2_USING;

namespace nvidia::write_protect
{

auto DomainManager::start() -> sdbusplus::async::task<>
{
    if (entityManager)
    {
        throw std::runtime_error("DomainManager already started");
    }

    entityManager = std::make_unique<EntityManager>(
        ctx,
        EntityManager::interface_list_t{
            writeProtectGroupConfigIntf, hwWpCtrlConfigIntf,
            writeProtectDomainConfigIntf, writeProtectInputIntf},
        std::bind_front(&DomainManager::processInventoryAdded, this),
        std::bind_front(&DomainManager::processInventoryRemoved, this));

    co_await entityManager->handleInventoryGet();
}

void DomainManager::processInventoryAdded(
    const sdbusplus::message::object_path& objectPath,
    const std::string& interfaceName)
{
    info("Inventory added {PATH} ({INTF})", "PATH", objectPath, "INTF",
         interfaceName);

    auto errorHandler = [objectPath](std::exception_ptr ep) {
        try
        {
            if (ep)
            {
                std::rethrow_exception(ep);
            }
        }
        catch (const std::exception& e)
        {
            error("Error processing {PATH}: {ERROR}", "PATH", objectPath,
                  "ERROR", e.what());
        }
    };

    if (interfaceName == writeProtectInputIntf)
    {
        ctx.spawn(addGpioGroup(objectPath, interfaceName) |
                  sdbusplus::async::execution::upon_error(errorHandler));
    }
    else
    {
        ctx.spawn(addGroup(objectPath, interfaceName) |
                  sdbusplus::async::execution::upon_error(errorHandler));
    }
}

void DomainManager::processInventoryRemoved(
    const sdbusplus::message::object_path& objectPath, const std::string&)
{
    info("Inventory removed {OBJECT} - ignoring", "OBJECT", objectPath);
}

auto DomainManager::getOrCreateSource(const std::string& identifier)
    -> std::pair<std::shared_ptr<Source>, bool>
{
    if (auto it = sources.find(identifier); it != sources.end())
    {
        return {it->second, false};
    }

    info("Creating source node for {ID}", "ID", identifier);

    auto nodeId = graph.addSource();
    auto source = std::make_shared<Source>(Source{nodeId, nullptr});
    sources.emplace(identifier, source);
    sourcesByNodeId[nodeId] = source;
    return {source, true};
}

auto DomainManager::initializeGpioSource(std::shared_ptr<Source> source,
                                         std::string name, GpioConfig config)
    -> sdbusplus::async::task<>
{
    if (source->protector)
    {
        co_return;
    }

    info("Initializing GPIO source {ID}", "ID", name);

    auto result = co_await Gpio::create(ctx, config);
    if (!result)
    {
        warning("GPIO source {ID} unavailable", "ID", name);
        co_return;
    }

    source->protector = std::make_unique<Gpio>(std::move(*result));
    co_await activateSource(source, std::move(name));
}

auto DomainManager::initializeEntitySource(std::shared_ptr<Source> source,
                                           std::string entityName)
    -> sdbusplus::async::task<>
{
    if (source->protector || !pendingEntityLinks.contains(entityName))
    {
        co_return;
    }

    info("Initializing entity source {ID}", "ID", entityName);

    auto path = softwarePath(entityName);
    auto result = co_await DbusProxy::create(ctx, path);

    // Re-check after co_await: the entity may have been resolved as a
    // local group while we were waiting for the D-Bus object.
    if (!pendingEntityLinks.contains(entityName))
    {
        info(
            "Entity {ID} resolved as group while waiting for D-Bus object {PATH}",
            "ID", entityName, "PATH", path);
        co_return;
    }

    if (!result)
    {
        warning("Entity source {ID} unavailable", "ID", entityName);
        co_return;
    }

    source->protector = std::make_unique<DbusProxy>(std::move(*result));
    pendingEntityLinks.erase(entityName);
    co_await activateSource(source, std::move(entityName));
}

auto DomainManager::activateSource(std::shared_ptr<Source> source,
                                   std::string name) -> sdbusplus::async::task<>
{
    if (!source->protector)
    {
        co_return;
    }

    auto initialValue = co_await source->protector->get();
    if (initialValue)
    {
        info("Source {ID} initial value={VAL}", "ID", name, "VAL",
             *initialValue);
        graph.set(source->nodeId, *initialValue);
        graph.propagate();
    }
    else
    {
        warning("Source {ID} initial get failed", "ID", name);
    }

    info("Source {ID} online", "ID", name);

    auto errorHandler = [name](std::exception_ptr ep) {
        try
        {
            if (ep)
            {
                std::rethrow_exception(ep);
            }
        }
        catch (const std::exception& e)
        {
            error("Source monitor error {NAME}: {ERROR}", "NAME", name, "ERROR",
                  e.what());
        }
    };

    ctx.spawn(monitorSource(source, std::move(name)) |
              sdbusplus::async::execution::upon_error(errorHandler));
}

auto DomainManager::monitorSource(std::shared_ptr<Source> source,
                                  std::string name) -> sdbusplus::async::task<>
{
    info("Monitor started for source {NAME}", "NAME", name);

    auto token = co_await stdexec::get_stop_token();
    while (!token.stop_requested())
    {
        if (!source->protector)
        {
            co_return;
        }

        auto changed = co_await source->protector->changed();
        if (!changed)
        {
            if (changed.error() == Error::Unavailable)
            {
                error(
                    "Source {NAME} is no longer available, stop requested={STOPPED}",
                    "NAME", name, "STOPPED", token.stop_requested());
                break;
            }
            error("Source {NAME} change error: {ERROR}", "NAME", name, "ERROR",
                  tostr(changed.error()));
        }
        else
        {
            info("Source {NAME} changed to {VAL}", "NAME", name, "VAL",
                 *changed);
            graph.set(source->nodeId, *changed);
            graph.propagate();
        }

        auto changedTime = std::chrono::steady_clock::now();
        auto lastChanged = source->lastChanged;
        source->lastChanged = changedTime;
        if (!lastChanged)
        {
            continue;
        }

        auto duration = changedTime - lastChanged.value();
        if (duration < sourceChangedCoolOffInterval)
        {
            auto sleepDuration = sourceChangedCoolOffInterval - duration;
            co_await sdbusplus::async::sleep_for(ctx, sleepDuration);
        }
    }
}

auto DomainManager::addGroup(sdbusplus::message::object_path objectPath,
                             std::string interface) -> sdbusplus::async::task<>
{
    auto config = co_await GroupConfig::tryFrom(ctx, EntityManager::serviceName,
                                                objectPath.str, interface);
    if (!config)
    {
        error("Invalid group configuration for {PATH}", "PATH", objectPath);
        co_return;
    }
    if (config->name.empty())
    {
        error("Group config missing Name for {PATH}", "PATH", objectPath);
        co_return;
    }
    if (groups.contains(config->name))
    {
        warning("Group {NAME} already exists, ignoring duplicate at {PATH}",
                "NAME", config->name, "PATH", objectPath);
        co_return;
    }

    info("Adding group {NAME}", "NAME", config->name);

    bool isDomain = interface == writeProtectDomainConfigIntf;
    auto node = graph.addNode(config->sourceMode);
    SourceNodeId settableSource;

    for (const auto& sourceName : config->sources)
    {
        if (auto it = groups.find(sourceName); it != groups.end())
        {
            graph.connect(it->second->outputNode, node);
            info("Group {NAME}: linked to group {REF}", "NAME", config->name,
                 "REF", sourceName);
        }
        else
        {
            auto [source, created] = getOrCreateSource(sourceName);
            graph.connect(source->nodeId, node);
            if (created)
            {
                spawnEntityInit(sourceName, source);
            }
            pendingEntityLinks[sourceName].push_back({node, source->nodeId});
            if (isDomain && settableSource == SourceNodeId{})
            {
                settableSource = source->nodeId;
            }
            info("Group {NAME}: waiting for entity {REF} (D-Bus or group)",
                 "NAME", config->name, "REF", sourceName);
        }
    }

    auto group = std::make_shared<Group>();
    group->name = config->name;
    group->outputNode = node;

    auto chassisPath = objectPath.parent_path();

    if (isDomain)
    {
        info("Group {NAME}: creating Domain at {PATH} with chassis "
             "association to {CHASSIS}",
             "NAME", config->name, "PATH", objectPath, "CHASSIS", chassisPath);
        auto domainPath =
            sdbusplus::message::object_path("/xyz/openbmc_project/state");
        domainPath /= objectPath.filename();
        group->domain = std::make_unique<Domain>(ctx, domainPath, this,
                                                 settableSource, chassisPath);

        graph.subscribe(node, [d = group->domain.get()](bool v) {
            d->write_protected(v);
        });
    }
    else if (interface == hwWpCtrlConfigIntf)
    {
        info("Input {NAME}: creating HW WP control at {PATH}", "NAME",
             config->name, "PATH", chassisPath);
        group->hwWpCtrl =
            std::make_unique<HardwareWriteProtectedControl>(ctx, chassisPath);

        graph.subscribe(node, [h = group->hwWpCtrl.get()](bool v) {
            h->write_protected_control(v);
        });
    }

    for (const auto& name : config->flashProtectedComponents)
    {
        info("Group {NAME}: adding software {SW}", "NAME", config->name, "SW",
             name);
        addSoftwareToGroup(*group, name);
    }

    if (group->domain)
    {
        emitAdded(*group->domain);
    }

    auto groupName = config->name;
    groups.emplace(groupName, group);
    resolveGroupLinks(*group, groupName);
    info("Successfully added group {NAME}", "NAME", groupName);
}

auto DomainManager::addGpioGroup(sdbusplus::message::object_path objectPath,
                                 std::string interface)
    -> sdbusplus::async::task<>
{
    auto config = co_await GpioInputConfig::tryFrom(
        ctx, EntityManager::serviceName, objectPath.str, interface);
    if (!config)
    {
        error("Invalid input configuration for {PATH}", "PATH", objectPath);
        co_return;
    }
    if (config->name.empty())
    {
        error("Input config missing Name for {PATH}", "PATH", objectPath);
        co_return;
    }
    if (config->lineName.empty())
    {
        error("Input config missing LineName for {PATH}", "PATH", objectPath);
        co_return;
    }
    if (groups.contains(config->name))
    {
        warning("Input {NAME} already exists, ignoring duplicate at {PATH}",
                "NAME", config->name, "PATH", objectPath);
        co_return;
    }

    auto [source, created] = getOrCreateSource(config->name);
    if (!created)
    {
        warning("Input {NAME} already exists, ignoring duplicate at {PATH}",
                "NAME", config->name, "PATH", objectPath);
        co_return;
    }

    GpioConfig gpio;
    gpio.pollInterval = config->pollInterval;
    gpio.lineName = config->lineName;
    gpio.activeLow = config->activeLow;
    spawnInit(config->name,
              initializeGpioSource(source, config->name, std::move(gpio)));

    info("Adding input {NAME}", "NAME", config->name);
    auto group = std::make_shared<Group>();
    group->name = config->name;
    group->outputNode = source->nodeId;

    auto chassisPath = objectPath.parent_path();

    for (const auto& name : config->flashProtectedComponents)
    {
        info("Input {NAME}: adding software {SW}", "NAME", config->name, "SW",
             name);
        addSoftwareToGroup(*group, name);
    }

    auto groupName = config->name;
    groups.emplace(groupName, group);
    resolveGroupLinks(*group, groupName);
    info("Successfully added input {NAME}", "NAME", groupName);
}

void DomainManager::addSoftwareToGroup(Group& group, const std::string& name)
{
    auto sw = std::make_unique<SoftwareComponent>(ctx, softwarePath(name));
    auto* ptr = sw.get();

    graph.subscribe(group.outputNode,
                    [ptr](bool v) { ptr->write_protected(v); });

    ptr->write_protected(graph.output(group.outputNode));
    group.software.emplace(name, std::move(sw));
}

auto DomainManager::setWriteProtected(SourceNodeId nodeId, bool value)
    -> sdbusplus::async::task<Result<void>>
{
    auto it = sourcesByNodeId.find(nodeId);
    if (it == sourcesByNodeId.end())
    {
        co_return std::unexpected(Error::Unavailable);
    }

    auto source = it->second.lock();
    if (!source || !source->protector)
    {
        co_return std::unexpected(Error::Unavailable);
    }

    auto setResult = co_await source->protector->set(value);
    if (!setResult)
    {
        error("Failed to set write protection value={VALUE}, error={ERROR}",
              "VALUE", value, "ERROR", tostr(setResult.error()));
        co_return std::unexpected(setResult.error());
    }

    auto getResult = co_await source->protector->get();
    if (!getResult)
    {
        error("Failed to get write protection error={ERROR}", "ERROR",
              tostr(getResult.error()));
        co_return std::unexpected(getResult.error());
    }

    bool actual = getResult.value();
    graph.set(nodeId, actual);
    graph.propagate();

    if (actual != value)
    {
        error("Failed to set write protection value={VALUE}, actual={ACTUAL}",
              "VALUE", value, "ACTUAL", actual);
        co_return std::unexpected(Error::InternalError);
    }

    co_return {};
}

void DomainManager::resolveGroupLinks(const Group& group,
                                      const std::string& name)
{
    if (auto pending = pendingEntityLinks.find(name);
        pending != pendingEntityLinks.end())
    {
        if (auto scopeIt = entityInitScopes.find(name);
            scopeIt != entityInitScopes.end())
        {
            scopeIt->second.request_stop();
        }

        for (auto& link : pending->second)
        {
            graph.connect(group.outputNode, link.targetNode);
            graph.deactivate(link.sourceNode);
            info("Group {NAME}: resolved pending entity link", "NAME", name);
        }
        pendingEntityLinks.erase(pending);
        graph.propagate();
    }
}
void DomainManager::spawnInit(const std::string& id,
                              sdbusplus::async::task<> task)
{
    auto errorHandler = [id](std::exception_ptr ep) {
        try
        {
            if (ep)
            {
                std::rethrow_exception(ep);
            }
        }
        catch (const std::exception& e)
        {
            error("Source init error {NAME}: {ERROR}", "NAME", id, "ERROR",
                  e.what());
        }
    };

    ctx.spawn(std::move(task) |
              sdbusplus::async::execution::upon_error(errorHandler));
};

void DomainManager::spawnEntityInit(const std::string& name,
                                    std::shared_ptr<Source> source)
{
    entityInitScopes.try_emplace(name);
    spawnInit(name, scopedEntityInit(std::move(source), name));
}

auto DomainManager::scopedEntityInit(std::shared_ptr<Source> source,
                                     std::string entityName)
    -> sdbusplus::async::task<>
{
    auto it = entityInitScopes.find(entityName);
    if (it == entityInitScopes.end())
    {
        co_return;
    }

    it->second.spawn(
        stdexec::just() |
            stdexec::let_value([this, source = std::move(source),
                                entityName]() -> sdbusplus::async::task<> {
                co_await initializeEntitySource(std::move(source), entityName);
            }),
        exec::default_task_context<void>(stdexec::inline_scheduler{}));

    co_await it->second.on_empty();
    entityInitScopes.erase(entityName);
}

} // namespace nvidia::write_protect
