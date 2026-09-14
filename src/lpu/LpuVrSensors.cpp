#include "LpuVrSensors.hpp"

#include "../SensorPaths.hpp"
#include "../Thresholds.hpp"
#include "../sensor.hpp"
#include "LpuCommon.hpp"
#include "LpuVrSource.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lpu_vr_detail
{
namespace
{

constexpr uint8_t lpuSmbusAddress = 0x67U;
constexpr std::string_view vrConfigType = lpu_vr_source::configType;

enum class Rail
{
    vddCore,
    vddC2c,
    vddm,
    vddhC2c,
    vddlC2c,
};

enum class Field
{
    outputVoltage,
    outputCurrent,
    inputVoltage,
    temperature,
    outputPower,
};

struct SensorDefinition
{
    std::string sensorName;
    std::string label;
    Rail rail{};
    Field field{};
    uint16_t offset{};
    std::string unit;
    double minValue{};
    double maxValue{};
};

std::optional<std::string> configString(const SensorBaseConfigMap& config,
                                        std::string_view key)
{
    const auto value = config.find(std::string(key));
    if (value == config.end())
    {
        return std::nullopt;
    }
    const auto* str = std::get_if<std::string>(&value->second);
    if (str == nullptr || str->empty())
    {
        return std::nullopt;
    }
    return *str;
}

std::optional<double> configDouble(const SensorBaseConfigMap& config,
                                   const std::string& key)
{
    const auto value = config.find(key);
    if (value == config.end())
    {
        return std::nullopt;
    }
    try
    {
        const double result =
            std::visit(VariantToDoubleVisitor(), value->second);
        if (!std::isfinite(result))
        {
            return std::nullopt;
        }
        return result;
    }
    catch (const std::invalid_argument&)
    {
        return std::nullopt;
    }
}

std::optional<Rail> railFromName(std::string_view name)
{
    if (name.find("VDDH_C2C") != std::string_view::npos ||
        name.find("VDDHC2C") != std::string_view::npos)
    {
        return Rail::vddhC2c;
    }
    if (name.find("VDDL_C2C") != std::string_view::npos ||
        name.find("VDDLC2C") != std::string_view::npos)
    {
        return Rail::vddlC2c;
    }
    if (name.find("VDD_CORE") != std::string_view::npos ||
        name.find("VDDCore") != std::string_view::npos)
    {
        return Rail::vddCore;
    }
    if (name.find("VDD_C2C") != std::string_view::npos ||
        name.find("VDDC2C") != std::string_view::npos)
    {
        return Rail::vddC2c;
    }
    if (name.find("VDDM") != std::string_view::npos)
    {
        return Rail::vddm;
    }
    return std::nullopt;
}

std::optional<Field> fieldFromLabel(std::string_view label)
{
    if (label.starts_with("vout"))
    {
        return Field::outputVoltage;
    }
    if (label.starts_with("iout"))
    {
        return Field::outputCurrent;
    }
    if (label.starts_with("vin"))
    {
        return Field::inputVoltage;
    }
    if (label.starts_with("temp"))
    {
        return Field::temperature;
    }
    if (label.starts_with("pout"))
    {
        return Field::outputPower;
    }
    return std::nullopt;
}

std::optional<unsigned> labelRailIndex(std::string_view label)
{
    size_t digitStart = label.size();
    while (digitStart > 0 && label[digitStart - 1] >= '0' &&
           label[digitStart - 1] <= '9')
    {
        --digitStart;
    }
    if (digitStart == label.size())
    {
        return 1U;
    }

    unsigned index = 0;
    const char* begin = label.data() + digitStart;
    const char* end = label.data() + label.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, index);
    if (parsed.ec != std::errc{} || parsed.ptr != end || index == 0U)
    {
        return std::nullopt;
    }
    return index;
}

std::optional<Rail> railForLabel(const SensorBaseConfigMap& config,
                                 Rail primaryRail, std::string_view label)
{
    const std::optional<unsigned> index = labelRailIndex(label);
    if (!index)
    {
        return std::nullopt;
    }
    if (*index == 1U)
    {
        return primaryRail;
    }

    const std::string nameKey = "Name" + std::to_string(*index - 1U);
    if (const auto secondaryName = configString(config, nameKey))
    {
        return railFromName(*secondaryName);
    }

    // The existing MP29816-shaped C00 row names the second rail VDD_C2C.
    if (*index == 2U && primaryRail == Rail::vddCore)
    {
        return Rail::vddC2c;
    }
    return std::nullopt;
}

uint16_t baseOffset(Rail rail)
{
    switch (rail)
    {
        case Rail::vddCore:
            return lpu_pmbus::vddCoreBaseOffset;
        case Rail::vddC2c:
            return lpu_pmbus::vddC2cBaseOffset;
        case Rail::vddm:
            return lpu_pmbus::vddmBaseOffset;
        case Rail::vddhC2c:
            return lpu_pmbus::vddhC2cBaseOffset;
        case Rail::vddlC2c:
            return lpu_pmbus::vddlC2cBaseOffset;
    }
    return 0U;
}

uint16_t fieldOffset(Field field)
{
    switch (field)
    {
        case Field::outputVoltage:
            return lpu_pmbus::voutSubOffset;
        case Field::outputCurrent:
            return lpu_pmbus::ioutSubOffset;
        case Field::inputVoltage:
            return lpu_pmbus::vinSubOffset;
        case Field::temperature:
            return lpu_pmbus::temperatureSubOffset;
        case Field::outputPower:
            return lpu_pmbus::poutSubOffset;
    }
    return 0U;
}

std::string unitForField(Field field)
{
    switch (field)
    {
        case Field::outputVoltage:
        case Field::inputVoltage:
            return sensor_paths::unitVolts;
        case Field::outputCurrent:
            return sensor_paths::unitAmperes;
        case Field::temperature:
            return sensor_paths::unitDegreesC;
        case Field::outputPower:
            return sensor_paths::unitWatts;
    }
    return {};
}

std::pair<double, double> defaultLimits(Field field)
{
    switch (field)
    {
        case Field::inputVoltage:
            return {0.0, 300.0};
        case Field::outputVoltage:
            return {0.0, 255.0};
        case Field::outputCurrent:
            return {0.0, 255.0};
        case Field::temperature:
            return {-128.0, 127.0};
        case Field::outputPower:
            return {0.0, 3000.0};
    }
    return {0.0, 1.0};
}

AssociationList collectLabelAssociations(const SensorData& sensorData,
                                         std::string_view configType,
                                         std::string_view label)
{
    const std::string associationInterfacePrefix =
        configInterfaceName(std::string(configType)) + ".Associations";
    AssociationList associations;
    for (const auto& [interface, config] : sensorData)
    {
        if (!interface.starts_with(associationInterfacePrefix))
        {
            continue;
        }

        const auto configuredLabel = configString(config, "Label");
        if (!configuredLabel || *configuredLabel != label)
        {
            continue;
        }

        const auto forward = configString(config, "Forward");
        const auto backward = configString(config, "Backward");
        const auto absolutePath = configString(config, "AbsolutePath");
        if (!forward || !backward || !absolutePath)
        {
            lg2::error(
                "Malformed LPU VR association on configuration interface {INTERFACE}",
                "INTERFACE", interface);
            continue;
        }

        associations.emplace_back(*forward, *backward, *absolutePath);
    }
    return associations;
}

std::vector<SensorDefinition> buildSensorDefinitions(
    const SensorBaseConfigMap& config)
{
    const auto rowName = configString(config, lpu_em::kName);
    if (!rowName)
    {
        return {};
    }
    const auto primaryRail = railFromName(*rowName);
    if (!primaryRail)
    {
        return {};
    }

    const auto labelsEntry = config.find("Labels");
    if (labelsEntry == config.end())
    {
        return {};
    }
    const auto* labels =
        std::get_if<std::vector<std::string>>(&labelsEntry->second);
    if (labels == nullptr)
    {
        return {};
    }

    std::vector<SensorDefinition> definitions;
    definitions.reserve(labels->size());
    for (const std::string& label : *labels)
    {
        const auto field = fieldFromLabel(label);
        const auto rail = railForLabel(config, *primaryRail, label);
        const auto sensorName = configString(config, label + "_Name");
        if (!field || !rail || !sensorName)
        {
            continue;
        }

        // virtual-sensor owns VDDC2C InputVoltage on every board revision.
        // Keep that stable alias and avoid two services owning one D-Bus path.
        if (*field == Field::inputVoltage && *rail == Rail::vddC2c)
        {
            continue;
        }

        // Only VDD_CORE and VDD_C2C implement POUT.  LPU firmware returns
        // SMBUS_CMD_NO_SUPPORT for VDDM, VDDH_C2C and VDDL_C2C, and those
        // three readings are not part of the canonical Redfish mockup.
        // Avoid publishing permanently unavailable sensor objects for them.
        if (*field == Field::outputPower && *rail != Rail::vddCore &&
            *rail != Rail::vddC2c)
        {
            continue;
        }

        auto [minValue, maxValue] = defaultLimits(*field);
        minValue = configDouble(config, label + "_Min").value_or(minValue);
        maxValue = configDouble(config, label + "_Max").value_or(maxValue);
        if (!(minValue < maxValue))
        {
            continue;
        }

        definitions.push_back({
            .sensorName = *sensorName,
            .label = label,
            .rail = *rail,
            .field = *field,
            .offset =
                static_cast<uint16_t>(baseOffset(*rail) + fieldOffset(*field)),
            .unit = unitForField(*field),
            .minValue = minValue,
            .maxValue = maxValue,
        });
    }
    return definitions;
}

bool decodePmbusValue(const uint8_t* bytes, size_t length, Field field,
                      double& value)
{
    if (bytes == nullptr || length != 3U || bytes[0] != 0U)
    {
        return false;
    }

    const uint16_t raw = static_cast<uint16_t>(bytes[1]) |
                         (static_cast<uint16_t>(bytes[2]) << 8U);
    // Firmware versions before the PMBus offsets were implemented can return
    // an all-ones placeholder with a success status.  Never publish that
    // sentinel as a real voltage/current/power/temperature reading.
    if (raw == std::numeric_limits<uint16_t>::max())
    {
        return false;
    }
    switch (field)
    {
        case Field::inputVoltage:
        case Field::outputVoltage:
            value = static_cast<double>(raw) / 1000.0;
            break;
        case Field::outputCurrent:
        case Field::temperature:
        case Field::outputPower:
            value = static_cast<double>(raw);
            break;
    }
    return true;
}

class LpuVrSensor :
    public Sensor,
    public std::enable_shared_from_this<LpuVrSensor>
{
  public:
    LpuVrSensor(sdbusplus::asio::object_server& objectServer,
                std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
                boost::asio::io_context& io, const SensorDefinition& definition,
                std::vector<thresholds::Threshold>&& thresholdData,
                AssociationList&& sensorAssociations,
                const std::string& configurationPath,
                const std::string& objectType, PowerState powerState,
                size_t thresholdConfigurationSize) :
        Sensor(definition.sensorName, std::move(thresholdData),
               configurationPath, objectType, false, false, definition.maxValue,
               definition.minValue, dbusConnection, powerState),
        objectServer(objectServer), thresholdTimer(io)
    {
        const std::string dbusPath =
            "/xyz/openbmc_project/sensors/" +
            sensor_paths::getPathForUnits(definition.unit) + "/" + name;
        sensorInterface =
            objectServer.add_interface(dbusPath, sensorValueInterface);
        for (const thresholds::Threshold& threshold : thresholds)
        {
            const size_t index = static_cast<size_t>(threshold.level);
            if (index >= thresholdInterfaces.size() ||
                thresholdInterfaces[index])
            {
                continue;
            }
            thresholdInterfaces[index] = objectServer.add_interface(
                dbusPath, thresholds::getInterface(threshold.level));
        }

        if (definition.label.empty() ||
            thresholdConfigurationSize == thresholds.size())
        {
            setInitialProperties(definition.unit);
        }
        else
        {
            setInitialProperties(definition.unit, definition.label,
                                 thresholdConfigurationSize);
        }

        association =
            objectServer.add_interface(dbusPath, association::interface);
        lpu_common::createInventoryAssoc(dbusConnection, association,
                                        configurationPath,
                                        std::move(sensorAssociations));
        markAvailable(false);
    }

    ~LpuVrSensor() override
    {
        for (const auto& interface : thresholdInterfaces)
        {
            objectServer.remove_interface(interface);
        }
        objectServer.remove_interface(sensorInterface);
        objectServer.remove_interface(association);
    }

    void updateReading(std::optional<double> reading)
    {
        if (!reading)
        {
            markAvailable(false);
            updateValue(std::numeric_limits<double>::quiet_NaN());
            return;
        }
        rawValue = *reading;
        updateValue(*reading);
    }

  private:
    void checkThresholds() override
    {
        if (!readingStateGood())
        {
            return;
        }
        [[maybe_unused]] const bool status =
            thresholds::checkThresholdsPowerDelay(weak_from_this(),
                                                  thresholdTimer);
    }

    sdbusplus::asio::object_server& objectServer;
    thresholds::ThresholdTimer thresholdTimer;
};

struct PendingSensor
{
    SensorDefinition definition;
    std::vector<thresholds::Threshold> thresholds;
    AssociationList associations;
    std::string configurationPath;
    std::string objectType;
    PowerState powerState{PowerState::always};
    size_t thresholdConfigurationSize{};
};

struct SensorBinding
{
    SensorDefinition definition;
    std::shared_ptr<LpuVrSensor> sensor;
    bool wasAvailable{false};
};

class LpuVrPublisher : public std::enable_shared_from_this<LpuVrPublisher>
{
  public:
    static std::shared_ptr<LpuVrPublisher> create(
        boost::asio::io_context& io,
        sdbusplus::asio::object_server& objectServer,
        std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
        uint64_t lpuInstance, uint64_t bus,
        std::vector<PendingSensor>&& pendingSensors)
    {
        auto publisher = std::shared_ptr<LpuVrPublisher>(
            new LpuVrPublisher(io, objectServer, dbusConnection, lpuInstance,
                               bus, std::move(pendingSensors)));
        boost::asio::post(io,
                          [weak = std::weak_ptr<LpuVrPublisher>(publisher)] {
                              if (const auto self = weak.lock())
                              {
                                  self->poll();
                              }
                          });
        return publisher;
    }

    ~LpuVrPublisher()
    {
        timer.cancel();
    }

  private:
    LpuVrPublisher(boost::asio::io_context& io,
                   sdbusplus::asio::object_server& objectServer,
                   std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
                   uint64_t lpuInstance, uint64_t bus,
                   std::vector<PendingSensor>&& pendingSensors) :
        timer(io), lpuInstance(lpuInstance), bus(bus)
    {
        sensors.reserve(pendingSensors.size());
        for (PendingSensor& pending : pendingSensors)
        {
            try
            {
                auto sensor = std::make_shared<LpuVrSensor>(
                    objectServer, dbusConnection, io, pending.definition,
                    std::move(pending.thresholds),
                    std::move(pending.associations), pending.configurationPath,
                    pending.objectType, pending.powerState,
                    pending.thresholdConfigurationSize);
                sensors.push_back(
                    {std::move(pending.definition), std::move(sensor), false});
            }
            catch (const std::exception& error)
            {
                lg2::error(
                    "LPU_VR_Metrics failed to publish sensor {NAME}: {ERROR}",
                    "NAME", pending.definition.sensorName, "ERROR",
                    error.what());
            }
        }
    }

    void poll()
    {
        static constexpr auto maxSampleAge = std::chrono::seconds(3);
        for (SensorBinding& binding : sensors)
        {
            std::array<uint8_t, 3> bytes{};
            double value = 0.0;
            const bool valid =
                lpu_common::i2cReadBytes(
                    bus, lpuSmbusAddress, binding.definition.offset,
                    bytes.data(), bytes.size(), true, maxSampleAge) == 0 &&
                decodePmbusValue(bytes.data(), bytes.size(),
                                 binding.definition.field, value);
            if (!valid)
            {
                if (binding.wasAvailable)
                {
                    lg2::warning(
                        "LPU_VR_Metrics sensor {NAME} became unavailable on LPU {LPU}",
                        "NAME", binding.definition.sensorName, "LPU",
                        lpuInstance);
                }
                binding.wasAvailable = false;
                binding.sensor->updateReading(std::nullopt);
                continue;
            }

            if (!binding.wasAvailable)
            {
                lg2::info(
                    "LPU_VR_Metrics sensor {NAME} is available on LPU {LPU}",
                    "NAME", binding.definition.sensorName, "LPU", lpuInstance);
            }
            binding.wasAvailable = true;
            binding.sensor->updateReading(value);
        }

        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait(
            [weak = weak_from_this()](const boost::system::error_code& error) {
                if (error == boost::asio::error::operation_aborted)
                {
                    return;
                }
                if (error)
                {
                    lg2::error("LPU_VR_Metrics poll timer failed: {ERROR}",
                               "ERROR", error.message());
                }
                if (const auto self = weak.lock())
                {
                    self->poll();
                }
            });
    }

    boost::asio::steady_timer timer;
    uint64_t lpuInstance{};
    uint64_t bus{};
    std::vector<SensorBinding> sensors;
};

std::vector<std::shared_ptr<LpuVrPublisher>> publishers;

} // namespace
} // namespace lpu_vr_detail

void lpuVrSensorsApplyManagedObjects(
    boost::asio::io_context& io, sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const ManagedObjectType& managedObjects)
{
    using namespace lpu_vr_detail;

    const lpu_vr_source::InstanceBusMap buses = lpu_vr_source::instanceBuses();
    std::map<uint64_t, std::vector<PendingSensor>> groupedSensors;

    for (const auto& [path, interfaces] : managedObjects)
    {
        const SensorBaseConfigMap* config = nullptr;
        std::string configType;
        const auto row =
            interfaces.find(configInterfaceName(std::string(vrConfigType)));
        if (row != interfaces.end() &&
            lpu_vr_source::isSelectableConfig(row->second))
        {
            config = &row->second;
            configType = vrConfigType;
        }
        if (config == nullptr)
        {
            continue;
        }

        const auto rowName = configString(*config, lpu_em::kName);
        if (!rowName)
        {
            continue;
        }
        const std::optional<uint64_t> instance =
            lpu_vr_source::instanceFromName(*rowName);
        if (!instance)
        {
            lg2::error(
                "LPU_VR_Metrics cannot infer LPU instance from row {NAME}",
                "NAME", *rowName);
            continue;
        }
        const auto bus = buses.find(*instance);
        if (bus == buses.end())
        {
            lg2::error(
                "LPU_VR_Metrics has no lpu850 device for LPU {LPU}; row {NAME} ignored",
                "LPU", *instance, "NAME", *rowName);
            continue;
        }
        std::vector<thresholds::Threshold> allThresholds;
        if (!thresholds::parseThresholdsFromConfig(interfaces, allThresholds))
        {
            lg2::error(
                "LPU_VR_Metrics row {NAME} contains malformed thresholds",
                "NAME", *rowName);
        }
        const size_t thresholdConfigurationSize = allThresholds.size();

        for (SensorDefinition& definition : buildSensorDefinitions(*config))
        {
            std::vector<thresholds::Threshold> sensorThresholds;
            if (!thresholds::parseThresholdsFromConfig(
                    interfaces, sensorThresholds, &definition.label))
            {
                lg2::error(
                    "LPU_VR_Metrics failed to parse thresholds for {NAME}",
                    "NAME", definition.sensorName);
            }

            PowerState powerState = getPowerState(*config);
            const auto labelPowerState =
                configString(*config, definition.label + "_PowerState");
            if (labelPowerState)
            {
                setReadState(*labelPowerState, powerState);
            }
            AssociationList sensorAssociations = collectLabelAssociations(
                interfaces, configType, definition.label);

            groupedSensors[*instance].push_back({
                .definition = std::move(definition),
                .thresholds = std::move(sensorThresholds),
                .associations = std::move(sensorAssociations),
                .configurationPath = std::string(path),
                .objectType = configType,
                .powerState = powerState,
                .thresholdConfigurationSize = thresholdConfigurationSize,
            });
        }
    }

    // Destroy old interfaces before recreating paths after an EM refresh.
    lpu_vr_detail::publishers.clear();
    std::vector<std::shared_ptr<LpuVrPublisher>> nextPublishers;
    nextPublishers.reserve(groupedSensors.size());
    size_t sensorCount = 0;
    for (auto& [instance, pendingSensors] : groupedSensors)
    {
        const auto bus = buses.find(instance);
        if (bus == buses.end() || pendingSensors.empty())
        {
            continue;
        }
        sensorCount += pendingSensors.size();
        nextPublishers.push_back(
            LpuVrPublisher::create(io, objectServer, dbusConnection, instance,
                                   bus->second, std::move(pendingSensors)));
    }

    if (!nextPublishers.empty())
    {
        lg2::info(
            "LPU_VR_Metrics published {SENSORS} sensors for {LPUS} LPUs from the lpu850 cache",
            "SENSORS", sensorCount, "LPUS", nextPublishers.size());
    }
    nextPublishers.swap(lpu_vr_detail::publishers);
}

bool lpuVrSensorsPublishersEmpty()
{
    return lpu_vr_detail::publishers.empty();
}

void lpuVrSensorsClearPublishers()
{
    lpu_vr_detail::publishers.clear();
}
