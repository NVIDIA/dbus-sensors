#include "../Utils.hpp"
#include "LpuCommon.hpp"
#include "LpuOnBootFields.hpp"
#include "LpuVrSensors.hpp"
#include "LpuVrSource.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#ifdef NVIDIA_SHMEM
#include <boost/container/flat_map.hpp>
#include <nlohmann/json.hpp>
#include <tal.hpp>
#endif
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/asio/property.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace
{

#ifdef NVIDIA_SHMEM
using ShmemProperties =
    boost::container::flat_map<std::string,
                               nv::sensor_aggregation::DbusVariantType>;
using ShmemInterfaces =
    boost::container::flat_map<std::string, ShmemProperties>;

constexpr std::string_view lpuAcceleratorPath =
    "/xyz/openbmc_project/inventory/system/accelerator";
constexpr std::string_view lpuInventoryPrefix =
    "/xyz/openbmc_project/inventory/system/accelerator/LPU_";
constexpr std::string_view lpuOperationalStatePrefix =
    "xyz.openbmc_project.State.Decorator.OperationalStatus.StateType.";
constexpr std::string_view lpuOperationalStateInterface =
    "xyz.openbmc_project.State.Decorator.OperationalStatus";
constexpr std::string_view lpuProcessorMetricsInterface =
    "com.nvidia.LPUProcessorMetrics";
constexpr std::string_view lpuPortMetricsInterface =
    "com.nvidia.LPUPortMetrics";
constexpr std::string_view lpuPortStateInterface =
    "xyz.openbmc_project.Inventory.Decorator.PortState";
constexpr std::string_view pwrSeqMonitorService = "com.Nvidia.PwrSeqMonitor";
constexpr std::string_view gpioMonitorService =
    "xyz.openbmc_project.Gpio.Monitor";
constexpr std::string_view gpioMonitorPath =
    "/xyz/openbmc_project/gpio/monitor";
constexpr std::string_view gpioLinePathPrefix =
    "/xyz/openbmc_project/gpio/monitor/lines";
constexpr std::string_view gpioLineInterface =
    "xyz.openbmc_project.Gpio.Monitor.Line";
constexpr std::string_view bootStatusDescriptor =
    "/usr/share/lpu-pwrseq-monitor/platform.json";
constexpr std::string_view lpuElementPrefix = "HGX_LPU_";
constexpr unsigned lpuCount = 16;

bool isLpuTelemetryInterface(std::string_view interface)
{
    return interface == lpuProcessorMetricsInterface ||
           interface == lpuPortMetricsInterface ||
           interface == lpuPortStateInterface;
}

void forwardPropertiesToShmem(const std::string& path,
                              const std::string& interface,
                              ShmemProperties& properties)
{
    const uint64_t timestamp =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    for (auto& [propertyName, value] : properties)
    {
        std::vector<uint8_t> rawData;
        tal::TelemetryAggregator::updateTelemetry(path, interface, propertyName,
                                                  rawData, timestamp, 0, value);
    }
}

void forwardLpuPropertiesToShmem(sdbusplus::message::message& msg)
{
    try
    {
        std::string interface;
        ShmemProperties properties;
        std::vector<std::string> invalidated;
        msg.read(interface, properties, invalidated);

        if (!isLpuTelemetryInterface(interface))
        {
            return;
        }

        const char* path = msg.get_path();
        if (path == nullptr)
        {
            return;
        }
        forwardPropertiesToShmem(path, interface, properties);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to forward LPU telemetry to shared memory: {ERR}",
                   "ERR", e.what());
    }
}

void forwardLpuInterfacesToShmem(sdbusplus::message::message& msg)
{
    try
    {
        sdbusplus::message::object_path objectPath;
        ShmemInterfaces interfaces;
        msg.read(objectPath, interfaces);

        const std::string path = objectPath.str;
        if (!path.starts_with(lpuAcceleratorPath))
        {
            return;
        }
        for (auto& [interface, properties] : interfaces)
        {
            if (!isLpuTelemetryInterface(interface))
            {
                continue;
            }
            forwardPropertiesToShmem(path, interface, properties);
        }
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "Failed to seed LPU telemetry interfaces in shared memory: {ERR}",
            "ERR", e.what());
    }
}

std::optional<unsigned> lpuIndexFromInventoryPath(std::string_view path)
{
    if (!path.starts_with(lpuInventoryPrefix))
    {
        return std::nullopt;
    }

    const std::string_view suffix = path.substr(lpuInventoryPrefix.size());
    unsigned index = 0;
    const auto [end, ec] =
        std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
    if (ec != std::errc{} || end != suffix.data() + suffix.size() ||
        index >= lpuCount)
    {
        return std::nullopt;
    }
    return index;
}

void forwardPwrSeqStateToShmem(const std::string& path,
                               std::string_view operationalState)
{
    const std::optional<unsigned> lpuIndex = lpuIndexFromInventoryPath(path);
    if (!lpuIndex || !operationalState.starts_with(lpuOperationalStatePrefix))
    {
        return;
    }

    const std::string_view state =
        operationalState.substr(lpuOperationalStatePrefix.size());
    if (state.empty())
    {
        return;
    }

    ShmemProperties properties;
    properties.emplace("State", std::string(state));
    properties.emplace("PowerState",
                       std::string(state == "Enabled" ? "On" : "Off"));
    forwardPropertiesToShmem(path, std::string(lpuProcessorMetricsInterface),
                             properties);
}

void forwardPwrSeqPropertiesToShmem(sdbusplus::message::message& msg)
{
    try
    {
        std::string interface;
        ShmemProperties properties;
        std::vector<std::string> invalidated;
        msg.read(interface, properties, invalidated);
        if (interface != lpuOperationalStateInterface)
        {
            return;
        }

        const auto stateIt = properties.find("State");
        if (stateIt == properties.end())
        {
            return;
        }
        const auto* state = std::get_if<std::string>(&stateIt->second);
        const char* path = msg.get_path();
        if (state == nullptr || path == nullptr)
        {
            return;
        }
        forwardPwrSeqStateToShmem(path, *state);
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "Failed to forward LPU power-sequence state to shared memory: {ERR}",
            "ERR", e.what());
    }
}

void seedPwrSeqStateToShmem(
    const std::shared_ptr<sdbusplus::asio::connection>& bus)
{
    for (unsigned index = 0; index < lpuCount; ++index)
    {
        const std::string path =
            std::string(lpuInventoryPrefix) + std::to_string(index);
        sdbusplus::asio::getProperty<std::string>(
            *bus, std::string(pwrSeqMonitorService), path,
            std::string(lpuOperationalStateInterface), "State",
            [path](const boost::system::error_code& ec,
                   const std::string& operationalState) {
                if (!ec)
                {
                    forwardPwrSeqStateToShmem(path, operationalState);
                }
            });
    }
}

struct BootStatusSource
{
    unsigned lpuIndex{};
    uint8_t mask{};
    bool activeLow{};
};

using BootStatusSources = std::unordered_map<std::string, BootStatusSource>;

std::optional<unsigned> lpuIndexFromElementId(std::string_view id)
{
    if (!id.starts_with(lpuElementPrefix))
    {
        return std::nullopt;
    }
    return lpuIndexFromInventoryPath(
        std::string(lpuInventoryPrefix) +
        std::string(id.substr(lpuElementPrefix.size())));
}

std::string gpioLinePath(std::string_view name)
{
    std::string path(gpioLinePathPrefix);
    path.push_back('/');
    for (const unsigned char character : name)
    {
        const bool valid = std::isalnum(character) != 0 || character == '_';
        path.push_back(valid ? static_cast<char>(character) : '_');
    }
    return path;
}

void addBootStatusSource(BootStatusSources& sources,
                         const nlohmann::json& reference, unsigned lpuIndex,
                         uint8_t mask)
{
    const auto name = reference.find("line");
    if (name == reference.end() || !name->is_string())
    {
        return;
    }
    sources.insert_or_assign(
        gpioLinePath(name->get<std::string>()),
        BootStatusSource{lpuIndex, mask,
                         reference.value("active_low", false)});
}

BootStatusSources loadBootStatusSources()
{
    std::ifstream input{std::string(bootStatusDescriptor)};
    nlohmann::json descriptor =
        nlohmann::json::parse(input, nullptr, /*allow_exceptions=*/false);
    const auto modules = descriptor.find("modules");
    if (!input || descriptor.is_discarded() || modules == descriptor.end() ||
        !modules->is_array())
    {
        lg2::error("Cannot load boot-status descriptor {PATH}", "PATH",
                   bootStatusDescriptor);
        return {};
    }

    BootStatusSources sources;
    for (const auto& module : *modules)
    {
        const auto elements = module.find("elements");
        if (elements == module.end() || !elements->is_array())
        {
            continue;
        }
        for (const auto& element : *elements)
        {
            const auto id = element.find("id");
            const auto bits = element.find("boot_status_bits");
            if (id == element.end() || !id->is_string() ||
                bits == element.end() || !bits->is_array())
            {
                continue;
            }
            const std::optional<unsigned> index =
                lpuIndexFromElementId(id->get<std::string>());
            if (!index)
            {
                continue;
            }

            const size_t count = std::min<size_t>(bits->size(), 4);
            for (size_t bit = 0; bit < count; ++bit)
            {
                addBootStatusSource(sources, (*bits)[bit], *index,
                                    static_cast<uint8_t>(1U << bit));
            }
            const auto bootComplete = element.find("bc");
            if (bootComplete != element.end() && bootComplete->is_object())
            {
                addBootStatusSource(sources, *bootComplete, *index, 0x10U);
            }
        }
    }
    return sources;
}

const BootStatusSources& bootStatusSources()
{
    static const BootStatusSources sources = loadBootStatusSources();
    return sources;
}

std::optional<bool> getBoolProperty(const SensorBaseConfigMap& properties,
                                    std::string_view name)
{
    const auto property = properties.find(std::string(name));
    if (property == properties.end())
    {
        return std::nullopt;
    }
    const bool* value = std::get_if<bool>(&property->second);
    return value == nullptr ? std::nullopt : std::optional<bool>(*value);
}

void publishBootStatusSnapshot(const ManagedObjectType& objects)
{
    std::array<uint8_t, lpuCount> codes{};
    std::array<uint8_t, lpuCount> expectedMasks{};
    std::array<uint8_t, lpuCount> readMasks{};

    for (const auto& sourceEntry : bootStatusSources())
    {
        const BootStatusSource& source = sourceEntry.second;
        expectedMasks[source.lpuIndex] = static_cast<uint8_t>(
            expectedMasks[source.lpuIndex] | source.mask);
    }

    for (const auto& [path, interfaces] : objects)
    {
        const auto source = bootStatusSources().find(path.str);
        const auto lineInterface =
            interfaces.find(std::string(gpioLineInterface));
        if (source == bootStatusSources().end() ||
            lineInterface == interfaces.end())
        {
            continue;
        }

        const std::optional<bool> bound =
            getBoolProperty(lineInterface->second, "Bound");
        const std::optional<bool> value =
            getBoolProperty(lineInterface->second, "Value");
        if (!bound.value_or(false) || !value)
        {
            continue;
        }

        const BootStatusSource& line = source->second;
        readMasks[line.lpuIndex] =
            static_cast<uint8_t>(readMasks[line.lpuIndex] | line.mask);
        if (line.activeLow ? !*value : *value)
        {
            codes[line.lpuIndex] =
                static_cast<uint8_t>(codes[line.lpuIndex] | line.mask);
        }
    }

    for (unsigned index = 0; index < lpuCount; ++index)
    {
        if (expectedMasks[index] == 0 ||
            readMasks[index] != expectedMasks[index])
        {
            continue;
        }
        ShmemProperties properties;
        properties.emplace("BootStatusCode", codes[index]);
        forwardPropertiesToShmem(
            std::string(lpuInventoryPrefix) + std::to_string(index),
            std::string(lpuProcessorMetricsInterface), properties);
    }
}

bool bootStatusReadPending = false;
bool bootStatusReadAgain = false;

void refreshBootStatus(
    const std::shared_ptr<sdbusplus::asio::connection>& connection)
{
    if (bootStatusSources().empty())
    {
        return;
    }
    if (bootStatusReadPending)
    {
        bootStatusReadAgain = true;
        return;
    }

    bootStatusReadPending = true;
    connection->async_method_call(
        [connection](const boost::system::error_code& ec,
                     const ManagedObjectType& objects) {
            bootStatusReadPending = false;
            if (ec)
            {
                lg2::warning("Cannot read BootStatusCode GPIOs: {ERROR}",
                             "ERROR", ec.message());
            }
            else
            {
                publishBootStatusSnapshot(objects);
            }

            if (bootStatusReadAgain)
            {
                bootStatusReadAgain = false;
                refreshBootStatus(connection);
            }
        },
        std::string(gpioMonitorService), std::string(gpioMonitorPath),
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
}
#endif

constexpr std::string_view pwrSeqManagerService = "com.Nvidia.PwrSeqMonitor";
constexpr std::string_view pwrSeqManagerPath = "/com/Nvidia/PwrSeqMonitor";
constexpr std::string_view pwrSeqManagerInterface =
    "com.Nvidia.PwrSeqMonitor.Manager";
constexpr unsigned lp30LpuCount = 16;

std::set<uint64_t> configuredVrInstances;

void syncVrPollingConfiguration(
    const std::shared_ptr<sdbusplus::asio::connection>& connection)
{
    for (unsigned instance = 0; instance < lp30LpuCount; ++instance)
    {
        const bool enabled = configuredVrInstances.contains(instance);
        const std::string target = "HGX_LPU_" + std::to_string(instance);
        connection->async_method_call(
            [target, enabled](const boost::system::error_code& ec,
                              const std::string& result) {
                if (ec)
                {
                    lg2::error(
                        "Failed to configure VR telemetry for {TARGET}: {ERROR}",
                        "TARGET", target, "ERROR", ec.message());
                    return;
                }
                if (result != "Ok" && result != "NotPresent")
                {
                    lg2::warning(
                        "pwrseq rejected VR telemetry configuration for {TARGET}: {RESULT}",
                        "TARGET", target, "RESULT", result);
                    return;
                }
                lg2::debug(
                    "VR telemetry configuration for {TARGET}: {STATE} ({RESULT})",
                    "TARGET", target, "STATE", enabled ? "lpu" : "direct",
                    "RESULT", result);
            },
            std::string(pwrSeqManagerService), std::string(pwrSeqManagerPath),
            std::string(pwrSeqManagerInterface), "SetVrTelemetryEnabled",
            target, enabled);
    }
}

void refreshFromEntityManager(
    boost::asio::io_context& io, sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection)
{
    if (!dbusConnection)
    {
        lg2::error("Connection not created");
        return;
    }

    dbusConnection->async_method_call(
        [&io, &objectServer,
         connection = dbusConnection](boost::system::error_code ec,
                                      const ManagedObjectType& resp) mutable {
            if (ec)
            {
                lg2::error("Error contacting entity manager");
                return;
            }
            lpuOnBootFieldsApplyManagedObjects(io, objectServer, connection,
                                               resp);
            configuredVrInstances = lpu_vr_source::configuredInstances(resp);
            syncVrPollingConfiguration(connection);
            lpuVrSensorsApplyManagedObjects(io, objectServer, connection, resp);
        },
        entityManagerName, inventoryPath, "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects");
}

} // namespace

int main()
{
    boost::asio::io_context io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    try
    {
        systemBus->request_name("xyz.openbmc_project.LPUTelemetry");
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "Failed to request D-Bus name xyz.openbmc_project.LPUTelemetry: {ERR}",
            "ERR", e);
        return 1;
    }

    sdbusplus::asio::object_server objectServer(systemBus, true);
    objectServer.add_manager("/xyz/openbmc_project/software");
    objectServer.add_manager("/xyz/openbmc_project/inventory");
    objectServer.add_manager("/xyz/openbmc_project/inventory/system");
    objectServer.add_manager(
        "/xyz/openbmc_project/inventory/system/accelerator");
    objectServer.add_manager("/xyz/openbmc_project/sensors");

#ifdef NVIDIA_SHMEM
    tal::TelemetryAggregator::namespaceInit(tal::ProcessType::Producer,
                                            "lputelemetry");
    // Use a separate connection so the producer receives the signals emitted
    // by its object-server connection.  InterfacesAdded seeds immutable and
    // initial values; PropertiesChanged keeps later samples current.
    auto telemetryBus = std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::bus::match::match telemetryMatch(
        static_cast<sdbusplus::bus::bus&>(*telemetryBus),
        "type='signal',member='PropertiesChanged',interface='org.freedesktop."
        "DBus.Properties',sender='xyz.openbmc_project.LPUTelemetry',"
        "path_namespace='/xyz/openbmc_project/inventory/system/accelerator'",
        forwardLpuPropertiesToShmem);
    sdbusplus::bus::match::match telemetryInterfacesMatch(
        static_cast<sdbusplus::bus::bus&>(*telemetryBus),
        "type='signal',member='InterfacesAdded',interface='org.freedesktop."
        "DBus.ObjectManager',sender='xyz.openbmc_project.LPUTelemetry'",
        forwardLpuInterfacesToShmem);
    sdbusplus::bus::match::match pwrSeqTelemetryMatch(
        static_cast<sdbusplus::bus::bus&>(*telemetryBus),
        "type='signal',member='PropertiesChanged',interface='org.freedesktop."
        "DBus.Properties',sender='com.Nvidia.PwrSeqMonitor',"
        "path_namespace='/xyz/openbmc_project/inventory/system/accelerator'",
        forwardPwrSeqPropertiesToShmem);
    sdbusplus::bus::match::match pwrSeqOwnerMatch(
        static_cast<sdbusplus::bus::bus&>(*telemetryBus),
        "type='signal',sender='org.freedesktop.DBus',interface='org."
        "freedesktop.DBus',member='NameOwnerChanged',"
        "arg0='com.Nvidia.PwrSeqMonitor'",
        [telemetryBus](sdbusplus::message::message& msg) {
            std::string name;
            std::string oldOwner;
            std::string newOwner;
            msg.read(name, oldOwner, newOwner);
            if (!newOwner.empty())
            {
                seedPwrSeqStateToShmem(telemetryBus);
            }
        });
    seedPwrSeqStateToShmem(telemetryBus);
    sdbusplus::bus::match::match bootStatusPropertiesMatch(
        static_cast<sdbusplus::bus::bus&>(*telemetryBus),
        "type='signal',member='PropertiesChanged',interface='org.freedesktop."
        "DBus.Properties',sender='xyz.openbmc_project.Gpio.Monitor',"
        "path_namespace='/xyz/openbmc_project/gpio/monitor/lines',"
        "arg0='xyz.openbmc_project.Gpio.Monitor.Line'",
        [telemetryBus](sdbusplus::message::message&) {
            refreshBootStatus(telemetryBus);
        });
    sdbusplus::bus::match::match bootStatusInterfacesMatch(
        static_cast<sdbusplus::bus::bus&>(*telemetryBus),
        "type='signal',member='InterfacesAdded',interface='org.freedesktop."
        "DBus.ObjectManager',sender='xyz.openbmc_project.Gpio.Monitor',"
        "path='/xyz/openbmc_project/gpio/monitor'",
        [telemetryBus](sdbusplus::message::message&) {
            refreshBootStatus(telemetryBus);
        });
    sdbusplus::bus::match::match bootStatusOwnerMatch(
        static_cast<sdbusplus::bus::bus&>(*telemetryBus),
        "type='signal',sender='org.freedesktop.DBus',interface='org."
        "freedesktop.DBus',member='NameOwnerChanged',"
        "arg0='xyz.openbmc_project.Gpio.Monitor'",
        [telemetryBus](sdbusplus::message::message& msg) {
            std::string name;
            std::string oldOwner;
            std::string newOwner;
            msg.read(name, oldOwner, newOwner);
            if (!newOwner.empty())
            {
                refreshBootStatus(telemetryBus);
            }
        });
    refreshBootStatus(telemetryBus);
#endif

    boost::asio::post(io, [&]() {
        refreshFromEntityManager(io, objectServer, systemBus);
    });

    // Entity Manager is the source of truth for C00/C02 ownership. Resend
    // that static choice whenever pwrseq restarts so it can restore the VR
    // poll bit after reopening the lpu850 character devices.
    sdbusplus::bus::match::match pwrSeqConfigurationOwnerMatch(
        static_cast<sdbusplus::bus::bus&>(*systemBus),
        "type='signal',sender='org.freedesktop.DBus',interface='org."
        "freedesktop.DBus',member='NameOwnerChanged',"
        "arg0='com.Nvidia.PwrSeqMonitor'",
        [systemBus](sdbusplus::message::message& msg) {
            std::string name;
            std::string oldOwner;
            std::string newOwner;
            msg.read(name, oldOwner, newOwner);
            if (!newOwner.empty())
            {
                syncVrPollingConfiguration(systemBus);
            }
        });

    boost::asio::steady_timer configTimer(io);

    std::function<void(sdbusplus::message::message&)> eventHandler =
        [&](sdbusplus::message::message&) {
            configTimer.expires_after(std::chrono::seconds(1));
            configTimer.async_wait([&](const boost::system::error_code& ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    return;
                }
                if (ec)
                {
                    lg2::error("config refresh timer error: {MSG}", "MSG",
                               ec.message());
                    return;
                }
                refreshFromEntityManager(io, objectServer, systemBus);
                if (lpuOnBootFieldsPublishersEmpty() &&
                    lpuVrSensorsPublishersEmpty())
                {
                    lg2::debug("No matching configuration records detected");
                }
            });
        };

    // Rebuilding all publishers is only required for EntityManager LPU
    // configuration changes, not for values published below inventory.
    const std::array<const char*, 3> configTypes = {
        lpu_em::kRecordTypeLpu, lpu_em::kRecordTypeLpuMetrics,
        lpu_em::kRecordTypeLpuVrMetrics};
    auto configMatches =
        setupPropertiesChangedMatches(*systemBus, configTypes, eventHandler);

    io.run();
    return 0;
}
