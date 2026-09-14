#include "LpuProcessorMetrics.hpp"

#include "LpuCommon.hpp"

#include <fcntl.h>
#include <lpu850/lpu850-uapi.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <boost/asio/steady_timer.hpp>
#include <boost/container/flat_map.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace lpu_metrics_detail
{

constexpr const char* kProcessorMetricsInterface = "com.nvidia.LPUProcessorMetrics";
constexpr const char* kInventoryDecoratorAsset =
    "xyz.openbmc_project.Inventory.Decorator.Asset";
constexpr const char* kPortItemInterface =
    "xyz.openbmc_project.Inventory.Item.Port";
constexpr const char* kPortInfoInterface =
    "xyz.openbmc_project.Inventory.Decorator.PortInfo";
constexpr const char* kPortStateInterface =
    "xyz.openbmc_project.Inventory.Decorator.PortState";
constexpr const char* kLpuPortMetricsInterface = "com.nvidia.LPUPortMetrics";
constexpr const char* kOperationalStatusInterface =
    "xyz.openbmc_project.State.Decorator.OperationalStatus";
constexpr const char* kPowerSequenceMonitorService = "com.Nvidia.PwrSeqMonitor";
constexpr const char* kDbusService = "org.freedesktop.DBus";
constexpr const char* kDbusPath = "/org/freedesktop/DBus";
constexpr const char* kDbusInterface = "org.freedesktop.DBus";
constexpr std::string_view kLpuInventoryPathPrefix =
    "/xyz/openbmc_project/inventory/system/accelerator/LPU_";
constexpr const char* kC2cClockAuxiliaryName = "C2CClockSpeedMHz";
constexpr const char* kSerdesLinkLockAuxiliaryName = "SerdesLinkLockStatus";
constexpr const char* kSerdesFecErrorAuxiliaryName = "SerdesFecErrorStatus";
constexpr const char* kDeprecatedVoltageBinAuxiliaryName = "VoltageBinNumber";
constexpr uint8_t kLpuC2cPortCount = 24;
constexpr uint8_t kLpuC2cLaneCount = 4;
constexpr unsigned kLpuCount = 16;
constexpr uint8_t kSerdesLinkLockReadSize = kLpuC2cPortCount + 1;
constexpr uint8_t kSerdesFecReadSize =
    kLpuC2cPortCount * kLpuC2cLaneCount + 1;
constexpr uint8_t kMaxLpuMetricReadSize = kSerdesFecReadSize;
constexpr float kFastMetricPollRateSec = 1.0F;
constexpr float kSlowMetricPollRateSec = 30.0F;
constexpr auto kKernelPollingReadySettleDelay = std::chrono::milliseconds(250);
constexpr auto kKernelPollingRetryInitialInterval = std::chrono::seconds(1);
constexpr auto kKernelPollingRetryIntervalCap = std::chrono::seconds(30);

const std::string kPortTypeBidirectional(
    "xyz.openbmc_project.Inventory.Decorator.PortInfo.PortType.BidirectionalPort");
const std::string kPortProtocolOem(
    "xyz.openbmc_project.Inventory.Decorator.PortInfo.PortProtocol.OEM");
const std::string kPortLinkStateEnabled(
    "xyz.openbmc_project.Inventory.Decorator.PortState.LinkStates.Enabled");
const std::string kPortLinkStatusUp(
    "xyz.openbmc_project.Inventory.Decorator.PortState.LinkStatusType.LinkUp");
const std::string kPortLinkStatusDown(
    "xyz.openbmc_project.Inventory.Decorator.PortState.LinkStatusType.LinkDown");

using PortLinkLockedPerLane =
    std::array<std::array<uint8_t, kLpuC2cLaneCount>, kLpuC2cPortCount>;
using PortFecErrorsPerLane =
    std::array<std::array<uint8_t, kLpuC2cLaneCount>, kLpuC2cPortCount>;

static std::optional<unsigned> lpuIndexFromInventoryPath(std::string_view path)
{
    if (!path.starts_with(kLpuInventoryPathPrefix))
    {
        return std::nullopt;
    }

    const std::string_view suffix = path.substr(kLpuInventoryPathPrefix.size());
    unsigned index = 0;
    const auto [end, ec] =
        std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
    if (ec != std::errc{} || end != suffix.data() + suffix.size() ||
        index >= kLpuCount)
    {
        return std::nullopt;
    }
    return index;
}

static uint64_t realtimeNowNs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void updateC2CPortLinkStatus(const std::string& processorPath,
                             const PortLinkLockedPerLane& linkLockedPerLane);

static std::chrono::milliseconds pollRateToDuration(float pollRateSec)
{
    if (!std::isfinite(pollRateSec) || pollRateSec <= 0.0F)
    {
        return std::chrono::milliseconds(0);
    }
    int64_t ms = static_cast<int64_t>(
        std::ceil(static_cast<double>(pollRateSec) * 1000.0));
    if (ms < 1)
    {
        ms = 1;
    }
    return std::chrono::milliseconds(ms);
}

static float defaultPollRateForOffset(uint16_t offset)
{
    if (offset == 0x0006U ||
        (offset >= 0x0008U && offset <= 0x0015U) || offset == 0x0017U ||
        offset == 0x0100U)
    {
        return kFastMetricPollRateSec;
    }
    if ((offset >= 0x0001U && offset <= 0x0005U) || offset == 0x0007U ||
        (offset >= 0x0018U && offset <= 0x001AU))
    {
        return kSlowMetricPollRateSec;
    }
    return 0.0F;
}

constexpr auto kOneShotRetryInitial = std::chrono::seconds(2);
constexpr auto kOneShotRetryMax = std::chrono::seconds(30);

static void scheduleOneShotRetry(MetricRow& row,
                                 std::chrono::steady_clock::time_point now)
{
    constexpr uint8_t maxShift = 4;
    const uint8_t shift = std::min(row.oneShotRetryCount, maxShift);
    const auto delay = std::min(kOneShotRetryInitial * (1U << shift),
                                kOneShotRetryMax);
    row.nextPollDue = now + delay;
    if (row.oneShotRetryCount < maxShift)
    {
        ++row.oneShotRetryCount;
    }
}

static bool configString(const lpu::SensorBaseConfigMap& cfg, const char* key,
                         std::string& out)
{
    auto it = cfg.find(key);
    if (it == cfg.end())
    {
        return false;
    }
    const BasicVariantType& v = it->second;
    if (const auto* ps = std::get_if<std::string>(&v))
    {
        out = *ps;
        return true;
    }
    if (const auto* u = std::get_if<uint64_t>(&v))
    {
        out = std::to_string(*u);
        return true;
    }
    if (const auto* i = std::get_if<int64_t>(&v))
    {
        out = std::to_string(*i);
        return true;
    }
    if (const auto* u32 = std::get_if<uint32_t>(&v))
    {
        out = std::to_string(*u32);
        return true;
    }
    if (const auto* i32 = std::get_if<int32_t>(&v))
    {
        out = std::to_string(*i32);
        return true;
    }
    if (const auto* u16 = std::get_if<uint16_t>(&v))
    {
        out = std::to_string(*u16);
        return true;
    }
    if (const auto* i16 = std::get_if<int16_t>(&v))
    {
        out = std::to_string(*i16);
        return true;
    }
    if (const auto* u8 = std::get_if<uint8_t>(&v))
    {
        out = std::to_string(*u8);
        return true;
    }
    return false;
}

static bool tryParseI2cFields(const lpu::SensorBaseConfigMap& cfg,
                              uint8_t& busId, uint8_t& addr, uint16_t& off,
                              uint8_t& width, std::string& errMsg)
{
    auto bus = lpu_common::parseBusId(cfg);
    if (!bus)
    {
        errMsg = "Bus or I2CBus missing or invalid";
        return false;
    }
    auto i2cAddr = lpu_common::parseI2cAddress(cfg);
    if (!i2cAddr)
    {
        errMsg = "Address missing or invalid";
        return false;
    }
    auto offset = lpu_common::parseOffsetValue(cfg);
    if (!offset)
    {
        errMsg = "OffsetValue missing or invalid";
        return false;
    }
    auto readWidth = lpu_common::parseReadWidthBytes(cfg);
    if (!readWidth)
    {
        errMsg = "ReadWidthBytes missing or invalid";
        return false;
    }
    busId = *bus;
    addr = *i2cAddr;
    off = *offset;
    width = *readWidth;
    return true;
}

/** RISC-V / core MHz (entity-manager ReadWidthBytes 3):
 *  - byte 0: status (0 = valid)
 *  - bytes 1..2: Uint16 LE MHz
 *  Non-zero status keeps the last published value. */
static bool decodeClockMhz(const uint8_t* buf, uint8_t len, uint16_t& outMHz,
                           const char* displayName, const char* logContext)
{
    if (len != 3)
    {
        lg2::error(
            "LPU_Metrics invalid read width for {NAME} ({CTX} clock): {LEN}",
            "NAME", displayName, "CTX", logContext, "LEN", len);
        return false;
    }
    if (buf[0] != 0)
    {
        lg2::debug(
            "LPU_Metrics stale {CTX} clock for {NAME}: status 0x{ST} (keeping last value)",
            "CTX", logContext, "NAME", displayName, "ST", lg2::hex, buf[0]);
        return false;
    }
    outMHz = static_cast<uint16_t>(buf[1]) |
        (static_cast<uint16_t>(buf[2]) << static_cast<unsigned>(8));
    return true;
}

/** Decode MHz from buf and publish. */
static bool applyClockMhzRow(
    const MetricRow& row, const uint8_t* buf, uint8_t len, uint16_t& slot,
    const std::shared_ptr<sdbusplus::asio::dbus_interface>& iface,
    const char* dbusPropertyName, const char* logContext)
{
    if (!iface)
    {
        lg2::error(
            "LPU_Metrics D-Bus interface missing for {NAME} ({CTX} clock {PROP})",
            "NAME", row.displayName, "CTX", logContext, "PROP",
            dbusPropertyName);
        return false;
    }
    uint16_t mhz = 0;
    if (!decodeClockMhz(buf, len, mhz, row.displayName.c_str(), logContext))
    {
        return false;
    }
    slot = mhz;
    iface->set_property(dbusPropertyName, slot);
    return true;
}

/** Per-direction C2C clocks from telemetry offset 0x0005 (Uint64 / four Uint16 LE). */
struct C2cDirectionClockMhz
{
    uint16_t ne{};
    uint16_t se{};
    uint16_t sw{};
    uint16_t nw{};
};

/** C2C (entity-manager ReadWidthBytes 9, OffsetValue 5):
 *  - byte 0: status (0 = valid)
 *  - bytes 1..8: four Uint16 LE lane MHz values (NE, SE, SW, NW).
 *  Non-zero status keeps the last published value. */
static bool decodeC2cDirectionClockMhz(const uint8_t* buf, uint8_t len,
                                     C2cDirectionClockMhz& out,
                                     const char* displayName)
{
    if (len != 9)
    {
        lg2::error(
            "LPU_Metrics invalid read width for {NAME}: {LEN} (expected 9)",
            "NAME", displayName, "LEN", len);
        return false;
    }
    if (buf[0] != 0)
    {
        lg2::debug(
            "LPU_Metrics stale C2C clock for {NAME}: status 0x{ST} (keeping last value)",
            "NAME", displayName, "ST", lg2::hex, buf[0]);
        return false;
    }
    const uint8_t* payload = buf + 1;
    auto readLeU16 = [payload](unsigned off) -> uint16_t {
        return static_cast<uint16_t>(payload[off]) |
               (static_cast<uint16_t>(payload[off + 1]) << 8U);
    };
    out.ne = readLeU16(0);
    out.se = readLeU16(2);
    out.sw = readLeU16(4);
    out.nw = readLeU16(6);
    return true;
}

/** 16-bit values such as AVS Vmin (entity-manager ReadWidthBytes 3):
 *  - byte 0: status (0 = valid)
 *  - bytes 1..2: Uint16 LE
 *  Non-zero status keeps the last published value. */
static bool decodeStatusLeU16(const uint8_t* buf, uint8_t len, uint16_t& out,
                              const char* displayName)
{
    if (len != 3)
    {
        lg2::error(
            "LPU_Metrics invalid read width for {NAME}: {LEN} (expected 3)",
            "NAME", displayName, "LEN", len);
        return false;
    }
    if (buf[0] != 0)
    {
        lg2::debug(
            "LPU_Metrics stale counter for {NAME}: status 0x{ST} (keeping last value)",
            "NAME", displayName, "ST", lg2::hex, buf[0]);
        return false;
    }
    out = static_cast<uint16_t>(buf[1]) |
          (static_cast<uint16_t>(buf[2]) << 8U);
    return true;
}

/** SPI CRC and tile error counters (entity-manager ReadWidthBytes 5):
 *  - byte 0: status (0 = valid)
 *  - bytes 1..4: Uint32 LE
 *  Non-zero status keeps the last published value. */
static bool decodeStatusLeU32(const uint8_t* buf, uint8_t len, uint32_t& out,
                              const char* displayName)
{
    if (len != 5)
    {
        lg2::error(
            "LPU_Metrics invalid read width for {NAME}: {LEN} (expected 5)",
            "NAME", displayName, "LEN", len);
        return false;
    }
    if (buf[0] != 0)
    {
        lg2::debug(
            "LPU_Metrics stale counter for {NAME}: status 0x{ST} (keeping last value)",
            "NAME", displayName, "ST", lg2::hex, buf[0]);
        return false;
    }
    out = static_cast<uint32_t>(buf[1]) |
          (static_cast<uint32_t>(buf[2]) << 8U) |
          (static_cast<uint32_t>(buf[3]) << 16U) |
          (static_cast<uint32_t>(buf[4]) << 24U);
    return true;
}

struct InventoryProcessorMetricsPublisher
    : std::enable_shared_from_this<InventoryProcessorMetricsPublisher>
{
    static std::shared_ptr<InventoryProcessorMetricsPublisher> create(
        boost::asio::io_context& ioctx,
        sdbusplus::asio::object_server& objServer,
        const std::shared_ptr<sdbusplus::asio::connection>& connection,
        std::string inventoryDbusPath, std::vector<MetricRow> rows)
    {
        auto pub = std::shared_ptr<InventoryProcessorMetricsPublisher>(
            new InventoryProcessorMetricsPublisher(
                ioctx, objServer, connection, std::move(inventoryDbusPath)));
        pub->finishSetup(std::move(rows));
        pub->startPowerStateGate();
        return pub;
    }

    InventoryProcessorMetricsPublisher(
        boost::asio::io_context& ioctx,
        sdbusplus::asio::object_server& objServer,
        std::shared_ptr<sdbusplus::asio::connection> connection,
        std::string inventoryDbusPath) :
        ioCtx(ioctx), objectServer(objServer),
        dbusConnection(std::move(connection)),
        inventoryPath(std::move(inventoryDbusPath))
    {
        metricsInterface = objectServer.add_interface(
            inventoryPath, kProcessorMetricsInterface);
        metricsInterface->register_property("RiscVClockSpeedMHz", riscvMHz);
        metricsInterface->register_property("CoreClockSpeedMHz", coreMHz);
        metricsInterface->register_property("C2CNEClockSpeedMHz", c2cNeMhz);
        metricsInterface->register_property("C2CSEClockSpeedMHz", c2cSeMhz);
        metricsInterface->register_property("C2CSWClockSpeedMHz", c2cSwMhz);
        metricsInterface->register_property("C2CNWClockSpeedMHz", c2cNwMhz);
        metricsInterface->register_property("VoltageBinNumber", voltageBinNumber);
        metricsInterface->register_property("Ticks", ticks);
        metricsInterface->register_property("AVSNominalVminMillivolts",
                                            avsNominalVminMillivolts);
        metricsInterface->register_property("AVSUnderdriveVminMillivolts",
                                            avsUnderdriveVminMillivolts);
        metricsInterface->register_property("AVSOverdriveVminMillivolts",
                                            avsOverdriveVminMillivolts);
        metricsInterface->register_property("SpiCrcErrorCount", spiCrcErrorCount);
        metricsInterface->register_property("SpiCorrectedCrcErrorCount",
                                            spiCorrectedCrcErrorCount);
        metricsInterface->register_property("ActivityMonitorState",
                                            activityMonitorState);
        metricsInterface->register_property("FpC2cTotalSBE", fpC2cTotalSBE);
        metricsInterface->register_property("FpIcuTileMisc0TotalSBE",
                                            fpIcuTileMisc0TotalSBE);
        metricsInterface->register_property("FpIcuTileVxmTotalSBE",
                                            fpIcuTileVxmTotalSBE);
        metricsInterface->register_property("FpNimTileTotalSBE", fpNimTileTotalSBE);
        metricsInterface->register_property("FpSxmTileTotalSBE", fpSxmTileTotalSBE);
        metricsInterface->register_property("FpVxmTileTotalSBE", fpVxmTileTotalSBE);
        metricsInterface->register_property("FpC2cTotalMBE", fpC2cTotalMBE);
        metricsInterface->register_property("FpIcuTileMisc0TotalMBE",
                                            fpIcuTileMisc0TotalMBE);
        metricsInterface->register_property("FpIcuTileVxmTotalMBE",
                                            fpIcuTileVxmTotalMBE);
        metricsInterface->register_property("FpNimTileTotalMBE", fpNimTileTotalMBE);
        metricsInterface->register_property("FpSxmTileTotalMBE", fpSxmTileTotalMBE);
        metricsInterface->register_property("FpVxmTileTotalMBE", fpVxmTileTotalMBE);
        // Source timestamp, not a value-change timestamp. It advances after a
        // valid poll even when all decoded metric values remain unchanged.
        metricsInterface->register_property("LastUpdated", lastUpdatedNs);
        metricsInterface->initialize();

        try
        {
            assetInterface =
                objectServer.add_interface(inventoryPath, kInventoryDecoratorAsset);
            assetInterface->register_property("Name", assetName);
            assetInterface->register_property("PartNumber", assetPartNumber);
            assetInterface->register_property("SKU", assetSku);
            assetInterface->register_property("SerialNumber", assetSerialNumber);
            assetInterface->initialize();
        }
        catch (const std::exception& e)
        {
            if (assetInterface)
            {
                objectServer.remove_interface(assetInterface);
                assetInterface.reset();
            }
            lg2::warning(
                "LPU_Metrics Inventory.Decorator.Asset failed for {PATH}: {ERR}; "
                "LPU processor metrics will still be published without asset fields",
                "PATH", inventoryPath, "ERR", e.what());
        }
        catch (...)
        {
            if (assetInterface)
            {
                objectServer.remove_interface(assetInterface);
                assetInterface.reset();
            }
            lg2::warning(
                "LPU_Metrics Inventory.Decorator.Asset failed for {PATH} with "
                "non-standard exception; LPU processor metrics will still be published "
                "without asset fields",
                "PATH", inventoryPath);
        }
    }

    InventoryProcessorMetricsPublisher(
        const InventoryProcessorMetricsPublisher&) = delete;
    InventoryProcessorMetricsPublisher&
        operator=(const InventoryProcessorMetricsPublisher&) = delete;

    ~InventoryProcessorMetricsPublisher()
    {
        if (pollTimer)
        {
            pollTimer->cancel();
        }
        if (kernelPollingRetryTimer)
        {
            kernelPollingRetryTimer->cancel();
        }
        removePortMetricsInterfaces();
        if (assetInterface)
        {
            objectServer.remove_interface(assetInterface);
        }
        if (metricsInterface)
        {
            objectServer.remove_interface(metricsInterface);
        }
    }

    void finishSetup(std::vector<MetricRow> configRows)
    {
        rows = std::move(configRows);
        if (!rows.empty())
        {
            pollTimer = std::make_unique<boost::asio::steady_timer>(ioCtx);
        }
        kernelPollingRetryTimer =
            std::make_unique<boost::asio::steady_timer>(ioCtx);
    }

    void startPowerStateGate()
    {
        if (!dbusConnection)
        {
            lg2::error(
                "LPU_Metrics cannot monitor power state for {PATH}: D-Bus connection missing",
                "PATH", inventoryPath);
            return;
        }

        namespace rules = sdbusplus::bus::match::rules;
        const std::string matchRule =
            rules::type::signal() + rules::member("PropertiesChanged") +
            rules::sender(kPowerSequenceMonitorService) +
            rules::interface("org.freedesktop.DBus.Properties") +
            rules::path(inventoryPath) +
            rules::argN(0, kOperationalStatusInterface);
        powerStateMatch = std::make_unique<sdbusplus::bus::match_t>(
            static_cast<sdbusplus::bus_t&>(*dbusConnection), matchRule,
            [weak = weak_from_this()](sdbusplus::message_t& msg) {
                auto self = weak.lock();
                if (!self)
                {
                    return;
                }

                try
                {
                    std::string interface;
                    boost::container::flat_map<std::string,
                                               std::variant<bool, std::string>>
                        changed;
                    msg.read(interface, changed);
                    const auto functional = changed.find("Functional");
                    if (interface != kOperationalStatusInterface ||
                        functional == changed.end())
                    {
                        return;
                    }
                    if (const bool* value =
                            std::get_if<bool>(&functional->second))
                    {
                        // Invalidate an older asynchronous Get reply. A signal
                        // is the authoritative state transition.
                        ++self->powerStateQueryGeneration;
                        self->setPowerFunctional(*value, true);
                    }
                }
                catch (const std::exception& e)
                {
                    lg2::error(
                        "LPU_Metrics failed to decode power state for {PATH}: {ERR}",
                        "PATH", self->inventoryPath, "ERR", e.what());
                }
            });

        const std::string ownerMatchRule =
            rules::type::signal() + rules::sender(kDbusService) +
            rules::interface(kDbusInterface) +
            rules::member("NameOwnerChanged") + rules::path(kDbusPath) +
            rules::argN(0, kPowerSequenceMonitorService);
        powerServiceOwnerMatch = std::make_unique<sdbusplus::bus::match_t>(
            static_cast<sdbusplus::bus_t&>(*dbusConnection), ownerMatchRule,
            [weak = weak_from_this()](sdbusplus::message_t& msg) {
                auto self = weak.lock();
                if (!self)
                {
                    return;
                }

                try
                {
                    std::string name;
                    std::string oldOwner;
                    std::string newOwner;
                    msg.read(name, oldOwner, newOwner);
                    if (name != kPowerSequenceMonitorService)
                    {
                        return;
                    }

                    ++self->powerStateQueryGeneration;
                    if (!oldOwner.empty())
                    {
                        // Fail closed while the daemon is absent or changing
                        // owners. Kernel polling ownership remains with pwrseq.
                        self->setPowerFunctional(false, false);
                    }
                    if (!newOwner.empty())
                    {
                        self->queryPowerFunctional();
                    }
                }
                catch (const std::exception& e)
                {
                    lg2::error(
                        "LPU_Metrics failed to decode pwrseq owner change: {ERR}",
                        "ERR", e.what());
                }
            });

        queryPowerFunctional();
    }

    void queryPowerFunctional()
    {
        const uint64_t queryGeneration = ++powerStateQueryGeneration;
        dbusConnection->async_method_call(
            [weak = weak_from_this(),
             queryGeneration](const boost::system::error_code& ec,
                              const std::variant<bool>& value) {
                auto self = weak.lock();
                if (!self || queryGeneration != self->powerStateQueryGeneration)
                {
                    return;
                }
                if (ec)
                {
                    // Fail closed. A later PropertiesChanged signal from the
                    // power-sequence monitor will activate this LPU.
                    lg2::debug(
                        "LPU_Metrics power state unavailable for {PATH}: {ERR}",
                        "PATH", self->inventoryPath, "ERR", ec.message());
                    return;
                }
                if (const bool* functional = std::get_if<bool>(&value))
                {
                    // This is a steady-state snapshot, not proof of a new
                    // Online edge. Accept current-cycle cached identity data
                    // when lputelemetry itself is merely restarted.
                    self->setPowerFunctional(*functional, false);
                }
            },
            kPowerSequenceMonitorService, inventoryPath,
            "org.freedesktop.DBus.Properties", "Get",
            kOperationalStatusInterface, "Functional");
    }

    void setPowerFunctional(bool functional, bool stateChangeSignal)
    {
        const bool onlineEdge = functional && stateChangeSignal;
        if (powerStateKnown && powerFunctional == functional && !onlineEdge)
        {
            return;
        }

        powerStateKnown = true;
        powerFunctional = functional;
        if (!functional)
        {
            // Polling is gated off, but the driver keeps the last
            // SerialNumber sample.
            for (MetricRow& row : rows)
            {
                if (row.auxiliaryName == "SerialNumber")
                {
                    applyRow(row);
                    break;
                }
            }
            stopPollingRows();
            kernelPollingRetryInterval = kKernelPollingRetryInitialInterval;
            if (kernelPollingRetryTimer)
            {
                kernelPollingRetryTimer->cancel();
            }
            onlineSinceNs = 0;
            lg2::debug("LPU_Metrics polling paused for {PATH}", "PATH",
                       inventoryPath);
            return;
        }

        if (onlineEdge)
        {
            // pwrseq publishes Functional immediately before it enables the
            // kernel workers. Record only a signalled Online edge, not an
            // initial Get while lputelemetry restarts on an already-online
            // LPU. This rejects samples cached in a previous power cycle
            // without permanently rejecting one-shot identity registers.
            onlineSinceNs = realtimeNowNs();
            stopPollingRows();
        }
        else
        {
            onlineSinceNs = 0;
        }
        kernelPollingRetryInterval = kKernelPollingRetryInitialInterval;
        scheduleKernelPollingCheck(kKernelPollingReadySettleDelay);
    }

    void scheduleKernelPollingCheck(std::chrono::milliseconds delay)
    {
        if (!powerFunctional || !kernelPollingRetryTimer)
        {
            return;
        }
        kernelPollingRetryTimer->expires_after(delay);
        kernelPollingRetryTimer->async_wait(
            [weak = weak_from_this()](const boost::system::error_code& ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    return;
                }
                if (ec)
                {
                    lg2::error(
                        "LPU_Metrics kernel polling gate timer error: {MSG}",
                        "MSG", ec.message());
                    return;
                }
                if (auto self = weak.lock())
                {
                    self->confirmKernelPolling();
                }
            });
    }

    void confirmKernelPolling()
    {
        if (!powerFunctional)
        {
            return;
        }

        if (kernelPollingEnabled())
        {
            startPollingRows();
            return;
        }

        // The module or its pwrseq-controlled workers disappeared after an
        // earlier successful check. Stop the 1 Hz sysfs work immediately and
        // fall back to an indefinite retry with a capped interval.
        stopPollingRows();
        const auto retryDelay = kernelPollingRetryInterval;
        // Cap only the interval, not the number of attempts. While pwrseq
        // still reports Functional, retry every 30 seconds until the ioctl
        // confirms that the kernel workers are active.
        kernelPollingRetryInterval =
            std::min(kernelPollingRetryInterval * 2,
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         kKernelPollingRetryIntervalCap));
        scheduleKernelPollingCheck(retryDelay);
    }

    void startPollingRows()
    {
        if (!powerFunctional || kernelPollingActive)
        {
            return;
        }

        kernelPollingActive = true;
        kernelPollingRetryInterval = kKernelPollingRetryInitialInterval;
        const auto now = std::chrono::steady_clock::now();
        for (MetricRow& row : rows)
        {
            row.lastPolledNs = 0;
            row.oneShotRetryCount = 0;
            const auto period = pollRateToDuration(row.pollRateSec);
            if (period.count() > 0)
            {
                row.nextPollDue = now + period;
            }
            else
            {
                // Re-read identity rows after every power cycle.
                row.initialized = false;
                scheduleOneShotRetry(row, now);
            }
        }
        lg2::debug("LPU_Metrics polling active for {PATH}", "PATH",
                   inventoryPath);
        schedulePollTimer();
    }

    void schedulePollTimer()
    {
        if (!powerFunctional || !kernelPollingActive || !pollTimer)
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        auto earliestDue = std::chrono::steady_clock::time_point::max();
        bool hasDueRows = false;
        for (const MetricRow& row : rows)
        {
            const auto period = pollRateToDuration(row.pollRateSec);
            if (period.count() == 0 && row.initialized)
            {
                continue;
            }
            hasDueRows = true;
            if (row.nextPollDue < earliestDue)
            {
                earliestDue = row.nextPollDue;
            }
        }
        if (!hasDueRows)
        {
            return;
        }

        auto delay = earliestDue - now;
        if (delay < std::chrono::steady_clock::duration::zero())
        {
            delay = std::chrono::steady_clock::duration::zero();
        }
        pollTimer->expires_after(delay);
        pollTimer->async_wait([weak = weak_from_this()](
                                  const boost::system::error_code& ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                return;
            }
            if (ec)
            {
                lg2::error("LPU_Metrics poll timer error: {MSG}", "MSG",
                           ec.message());
                return;
            }
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            self->applyPolledRows();
            self->schedulePollTimer();
        });
    }

    void applyPolledRows()
    {
        if (!powerFunctional || !kernelPollingActive)
        {
            return;
        }

        uint64_t batchLastUpdatedNs = 0;
        bool polledAnyRow = false;
        bool sawDriverTimestamp = false;
        for (MetricRow& row : rows)
        {
            const auto period = pollRateToDuration(row.pollRateSec);
            const bool retryOneShot = period.count() == 0 && !row.initialized;
            if (period.count() == 0 && !retryOneShot)
            {
                continue;
            }
            if (std::chrono::steady_clock::now() < row.nextPollDue)
            {
                continue;
            }
            polledAnyRow = true;
            if (applyRow(row))
            {
                row.initialized = true;
                batchLastUpdatedNs =
                    std::max(batchLastUpdatedNs, row.lastPolledNs);
            }
            else
            {
                lg2::debug(
                    "LPU_Metrics polled row {NAME} apply failed for inventory {PATH}",
                    "NAME", row.displayName, "PATH", inventoryPath);
            }
            sawDriverTimestamp = sawDriverTimestamp || row.lastPolledNs != 0;
            const auto completedAt = std::chrono::steady_clock::now();
            if (period.count() > 0)
            {
                do
                {
                    row.nextPollDue += period;
                } while (row.nextPollDue <= completedAt);
            }
            else if (!row.initialized)
            {
                scheduleOneShotRetry(row, completedAt);
            }
        }
        publishLastUpdated(batchLastUpdatedNs);
        if (polledAnyRow && !sawDriverTimestamp)
        {
            // A missing sysfs source can mean the module was removed while
            // Functional was still true. Re-check once per batch, then stop
            // all row timers if the read-only ioctl no longer confirms the
            // kernel workers.
            confirmKernelPolling();
        }
    }

    bool applyRow(MetricRow& row)
    {
        const bool isLinkLock =
            row.auxiliaryName == kSerdesLinkLockAuxiliaryName;
        const bool isFecError =
            row.auxiliaryName == kSerdesFecErrorAuxiliaryName;
        if (row.readWidthBytes == 0 ||
            (isLinkLock &&
             row.readWidthBytes != kSerdesLinkLockReadSize) ||
            (isFecError && row.readWidthBytes != kSerdesFecReadSize) ||
            (!isLinkLock && !isFecError && row.readWidthBytes > 9))
        {
            lg2::error(
                "LPU_Metrics invalid ReadWidthBytes {W} for {NAME}", "W",
                row.readWidthBytes, "NAME", row.displayName);
            return false;
        }

        std::array<uint8_t, kMaxLpuMetricReadSize> buf{};
        row.lastPolledNs = 0;
        if (lpu_common::i2cReadBytes(row.busId, row.i2cAddr, row.offset,
                                     buf.data(), row.readWidthBytes, false,
                                     std::chrono::nanoseconds::zero(),
                                     &row.lastPolledNs) != 0)
        {
            return false;
        }
        if (row.lastPolledNs < onlineSinceNs)
        {
            return false;
        }

        if (row.auxiliaryName == "RiscVClockSpeedMHz")
        {
            return applyClockMhzRow(row, buf.data(), row.readWidthBytes, riscvMHz,
                                    metricsInterface, "RiscVClockSpeedMHz",
                                    "RISC-V");
        }
        if (row.auxiliaryName == "CoreClockSpeedMHz")
        {
            return applyClockMhzRow(row, buf.data(), row.readWidthBytes, coreMHz,
                                    metricsInterface, "CoreClockSpeedMHz",
                                    "core");
        }
        if (row.auxiliaryName == "C2CClockSpeedMHz")
        {
            if (!metricsInterface)
            {
                lg2::error(
                    "LPU_Metrics D-Bus metrics interface missing for {NAME} inventory {PATH}",
                    "NAME", row.displayName, "PATH", inventoryPath);
                return false;
            }
            C2cDirectionClockMhz clocks{};
            if (!decodeC2cDirectionClockMhz(buf.data(), row.readWidthBytes, clocks,
                                            row.displayName.c_str()))
            {
                return false;
            }
            c2cNeMhz = clocks.ne;
            c2cSeMhz = clocks.se;
            c2cSwMhz = clocks.sw;
            c2cNwMhz = clocks.nw;
            metricsInterface->set_property("C2CNEClockSpeedMHz", c2cNeMhz);
            metricsInterface->set_property("C2CSEClockSpeedMHz", c2cSeMhz);
            metricsInterface->set_property("C2CSWClockSpeedMHz", c2cSwMhz);
            metricsInterface->set_property("C2CNWClockSpeedMHz", c2cNwMhz);
            return true;
        }
        if (row.auxiliaryName == "Ticks")
        {
            if (!metricsInterface)
            {
                return false;
            }
            uint64_t value = 0;
            if (row.readWidthBytes != 9 || buf[0] != 0)
            {
                lg2::debug(
                    "LPU_Metrics invalid Ticks sample for {NAME}: width {W}, status 0x{ST}",
                    "NAME", row.displayName, "W", row.readWidthBytes, "ST",
                    lg2::hex, buf[0]);
                return false;
            }
            for (size_t index = 0; index < sizeof(value); ++index)
            {
                value |= static_cast<uint64_t>(buf[index + 1]) << (index * 8U);
            }
            ticks = value;
            metricsInterface->set_property("Ticks", ticks);
            return true;
        }
        if (row.auxiliaryName == "AVSNominalVminMillivolts")
        {
            return applyVminRow(row, buf.data(), avsNominalVminMillivolts,
                                "AVSNominalVminMillivolts");
        }
        if (row.auxiliaryName == "AVSUnderdriveVminMillivolts")
        {
            return applyVminRow(row, buf.data(), avsUnderdriveVminMillivolts,
                                "AVSUnderdriveVminMillivolts");
        }
        if (row.auxiliaryName == "AVSOverdriveVminMillivolts")
        {
            return applyVminRow(row, buf.data(), avsOverdriveVminMillivolts,
                                "AVSOverdriveVminMillivolts");
        }
        if (isLinkLock)
        {
            if (buf[0] != 0)
            {
                lg2::debug(
                    "LPU_Metrics stale SerDes link lock sample for {NAME}: status 0x{ST} (keeping last value)",
                    "NAME", row.displayName, "ST", lg2::hex, buf[0]);
                return false;
            }
            PortLinkLockedPerLane nextLinkLockedPerLane{};
            for (size_t port = 0; port < kLpuC2cPortCount; ++port)
            {
                for (size_t lane = 0; lane < kLpuC2cLaneCount; ++lane)
                {
                    nextLinkLockedPerLane[port][lane] =
                        (buf[1U + port] & (1U << lane)) != 0 ? 1 : 0;
                }
            }
            if (linkLockValid &&
                nextLinkLockedPerLane == linkLockedPerLane)
            {
                return true;
            }
            linkLockedPerLane = nextLinkLockedPerLane;
            linkLockValid = true;
            return publishOrUpdatePortMetrics(true);
        }
        if (isFecError)
        {
            if (buf[0] != 0)
            {
                lg2::debug(
                    "LPU_Metrics stale SerDes FEC sample for {NAME}: status 0x{ST} (keeping last value)",
                    "NAME", row.displayName, "ST", lg2::hex, buf[0]);
                return false;
            }
            PortFecErrorsPerLane nextFecErrorsPerLane{};
            for (size_t port = 0; port < kLpuC2cPortCount; ++port)
            {
                for (size_t lane = 0; lane < kLpuC2cLaneCount; ++lane)
                {
                    nextFecErrorsPerLane[port][lane] =
                        buf[1U + port * kLpuC2cLaneCount + lane];
                }
            }
            if (fecErrorValid && nextFecErrorsPerLane == fecErrorsPerLane)
            {
                return true;
            }
            fecErrorsPerLane = nextFecErrorsPerLane;
            fecErrorValid = true;
            return publishOrUpdatePortMetrics(false);
        }
        if (row.auxiliaryName == "SerialNumber")
        {
            if (!assetInterface)
            {
                lg2::error(
                    "LPU_Metrics D-Bus asset interface missing for {NAME} inventory {PATH}",
                    "NAME", row.displayName, "PATH", inventoryPath);
                return false;
            }
            const std::string serialNumber =
                lpu_common::formatLpuAssetSerialString(buf.data(),
                                                       row.readWidthBytes);
            if (serialNumber == "NA")
            {
                return false;
            }
            assetSerialNumber = serialNumber;
            assetInterface->set_property("SerialNumber", assetSerialNumber);
            return true;
        }
        if (row.auxiliaryName == "VoltageBinNumber")
        {
            if (!metricsInterface)
            {
                lg2::error(
                    "LPU_Metrics D-Bus metrics interface missing for {NAME} inventory {PATH}",
                    "NAME", row.displayName, "PATH", inventoryPath);
                return false;
            }
            if (row.readWidthBytes != 1)
            {
                lg2::error(
                    "LPU_Metrics VoltageBinNumber invalid ReadWidthBytes {W} for {NAME}",
                    "W", row.readWidthBytes, "NAME", row.displayName);
                return false;
            }
            voltageBinNumber = buf[0];
            metricsInterface->set_property("VoltageBinNumber", voltageBinNumber);
            return true;
        }
        if (row.auxiliaryName == "SpiCrcErrorCount")
        {
            if (!metricsInterface)
            {
                lg2::error(
                    "LPU_Metrics D-Bus metrics interface missing for {NAME} inventory {PATH}",
                    "NAME", row.displayName, "PATH", inventoryPath);
                return false;
            }
            uint32_t value = 0;
            if (!decodeStatusLeU32(buf.data(), row.readWidthBytes, value,
                                   row.displayName.c_str()))
            {
                return false;
            }
            spiCrcErrorCount = value;
            metricsInterface->set_property("SpiCrcErrorCount", spiCrcErrorCount);
            return true;
        }
        bool fpMatched = false;
        if (applyFpErrorCounterRow(row, buf.data(), row.readWidthBytes,
                                   fpMatched))
        {
            return true;
        }
        if (fpMatched)
        {
            return false;
        }

        lg2::warning(
            "LPU_Metrics row {NAME} has unknown AuxiliaryName {AUX}; skipping",
            "NAME", row.displayName, "AUX", row.auxiliaryName);
        return false;
    }

    void refreshPortLinkStatus()
    {
        if (linkLockValid)
        {
            updateC2CPortLinkStatus(inventoryPath, linkLockedPerLane);
        }
    }

  private:
    void publishLastUpdated(uint64_t sourceTimestampNs)
    {
        if (sourceTimestampNs == 0 || sourceTimestampNs == lastUpdatedNs ||
            !metricsInterface)
        {
            return;
        }
        lastUpdatedNs = sourceTimestampNs;
        metricsInterface->set_property("LastUpdated", lastUpdatedNs);
    }

    bool kernelPollingEnabled() const
    {
        const std::optional<unsigned> lpuIndex =
            lpuIndexFromInventoryPath(inventoryPath);
        if (!lpuIndex)
        {
            lg2::error("LPU_Metrics cannot derive driver instance from {PATH}",
                       "PATH", inventoryPath);
            return false;
        }

        const std::string device =
            "/dev/lpu" + std::to_string(*lpuIndex + 1U) + "_log";
        const int fd = ::open(device.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
        {
            return false;
        }

        uint8_t mask = 0;
        const bool enabled = ::ioctl(fd, LPU_LOG_GET_POLLING, &mask) == 0 &&
                             (mask & (LPU_POLL_FAST | LPU_POLL_SLOW)) ==
                                 (LPU_POLL_FAST | LPU_POLL_SLOW);
        ::close(fd);
        return enabled;
    }

    void stopPollingRows()
    {
        kernelPollingActive = false;
        if (pollTimer)
        {
            pollTimer->cancel();
        }
    }

    void removePortMetricsInterfaces()
    {
        for (auto& portMetrics : portMetricsInterfaces)
        {
            if (portMetrics)
            {
                objectServer.remove_interface(portMetrics);
                portMetrics.reset();
            }
        }
    }

    bool publishPortMetrics()
    {
        try
        {
            for (size_t port = 0; port < kLpuC2cPortCount; ++port)
            {
                const std::string portPath =
                    inventoryPath + "/Ports/C2C_" + std::to_string(port);
                auto portMetrics = objectServer.add_interface(
                    portPath, kLpuPortMetricsInterface);
                try
                {
                    portMetrics->register_property(
                        "FecErrorsPerLane",
                        std::vector<uint8_t>(fecErrorsPerLane[port].begin(),
                                             fecErrorsPerLane[port].end()));
                    portMetrics->register_property(
                        "LinkLockedPerLane",
                        std::vector<uint8_t>(linkLockedPerLane[port].begin(),
                                             linkLockedPerLane[port].end()));
                    portMetrics->initialize();
                }
                catch (...)
                {
                    objectServer.remove_interface(portMetrics);
                    throw;
                }
                portMetricsInterfaces[port] = std::move(portMetrics);
            }
        }
        catch (const std::exception& e)
        {
            removePortMetricsInterfaces();
            lg2::error(
                "LPU_Metrics failed to publish C2C port metrics on {PATH}: {ERR}",
                "PATH", inventoryPath, "ERR", e.what());
            return false;
        }
        catch (...)
        {
            removePortMetricsInterfaces();
            lg2::error(
                "LPU_Metrics failed to publish C2C port metrics on {PATH}",
                "PATH", inventoryPath);
            return false;
        }
        return true;
    }

    bool publishOrUpdatePortMetrics(bool linkLockChanged)
    {
        if (linkLockChanged)
        {
            refreshPortLinkStatus();
        }
        if (!linkLockValid || !fecErrorValid)
        {
            return true;
        }
        if (!portMetricsInterfaces.front())
        {
            return publishPortMetrics();
        }

        for (size_t port = 0; port < kLpuC2cPortCount; ++port)
        {
            const auto& values = linkLockChanged ? linkLockedPerLane[port]
                                                 : fecErrorsPerLane[port];
            portMetricsInterfaces[port]->set_property(
                linkLockChanged ? "LinkLockedPerLane" : "FecErrorsPerLane",
                std::vector<uint8_t>(values.begin(), values.end()));
        }
        return true;
    }

    bool applyVminRow(const MetricRow& row, const uint8_t* buf,
                      uint16_t& slot, const char* dbusPropertyName)
    {
        uint16_t value = 0;
        if (!metricsInterface ||
            !decodeStatusLeU16(buf, row.readWidthBytes, value,
                               row.displayName.c_str()))
        {
            return false;
        }
        slot = value;
        metricsInterface->set_property(dbusPropertyName, slot);
        return true;
    }

    struct FpErrorCounterBinding
    {
        const char* auxiliaryName{};
        const char* dbusPropertyName{};
        uint32_t InventoryProcessorMetricsPublisher::* slot{};
    };

    boost::asio::io_context& ioCtx;
    sdbusplus::asio::object_server& objectServer;
    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;
    std::string inventoryPath;
    std::vector<MetricRow> rows;
    std::unique_ptr<boost::asio::steady_timer> pollTimer;
    std::unique_ptr<boost::asio::steady_timer> kernelPollingRetryTimer;
    std::unique_ptr<sdbusplus::bus::match_t> powerStateMatch;
    std::unique_ptr<sdbusplus::bus::match_t> powerServiceOwnerMatch;
    uint64_t powerStateQueryGeneration{0};
    std::chrono::milliseconds kernelPollingRetryInterval{
        kKernelPollingRetryInitialInterval};
    bool powerStateKnown{false};
    bool powerFunctional{false};
    bool kernelPollingActive{false};
    uint64_t onlineSinceNs{0};
    uint64_t lastUpdatedNs{0};
    uint16_t riscvMHz{0};
    uint16_t coreMHz{0};
    uint16_t c2cNeMhz{0};
    uint16_t c2cSeMhz{0};
    uint16_t c2cSwMhz{0};
    uint16_t c2cNwMhz{0};
    uint8_t voltageBinNumber{0};
    uint64_t ticks{0};
    uint16_t avsNominalVminMillivolts{0};
    uint16_t avsUnderdriveVminMillivolts{0};
    uint16_t avsOverdriveVminMillivolts{0};
    uint32_t spiCrcErrorCount{0};
    uint16_t spiCorrectedCrcErrorCount{0};
    std::string activityMonitorState{"Active"};
    uint32_t fpC2cTotalSBE{0};
    uint32_t fpIcuTileMisc0TotalSBE{0};
    uint32_t fpIcuTileVxmTotalSBE{0};
    uint32_t fpNimTileTotalSBE{0};
    uint32_t fpSxmTileTotalSBE{0};
    uint32_t fpVxmTileTotalSBE{0};
    uint32_t fpC2cTotalMBE{0};
    uint32_t fpIcuTileMisc0TotalMBE{0};
    uint32_t fpIcuTileVxmTotalMBE{0};
    uint32_t fpNimTileTotalMBE{0};
    uint32_t fpSxmTileTotalMBE{0};
    uint32_t fpVxmTileTotalMBE{0};
    std::shared_ptr<sdbusplus::asio::dbus_interface> metricsInterface;
    PortLinkLockedPerLane linkLockedPerLane{};
    PortFecErrorsPerLane fecErrorsPerLane{};
    bool linkLockValid{false};
    bool fecErrorValid{false};
    std::array<std::shared_ptr<sdbusplus::asio::dbus_interface>,
               kLpuC2cPortCount>
        portMetricsInterfaces{};

    static constexpr std::array<FpErrorCounterBinding, 12> fpErrorCounterBindings{{
        {"FpC2cTotalSBE", "FpC2cTotalSBE",
         &InventoryProcessorMetricsPublisher::fpC2cTotalSBE},
        {"FpIcuTileMisc0TotalSBE", "FpIcuTileMisc0TotalSBE",
         &InventoryProcessorMetricsPublisher::fpIcuTileMisc0TotalSBE},
        {"FpIcuTileVxmTotalSBE", "FpIcuTileVxmTotalSBE",
         &InventoryProcessorMetricsPublisher::fpIcuTileVxmTotalSBE},
        {"FpNimTileTotalSBE", "FpNimTileTotalSBE",
         &InventoryProcessorMetricsPublisher::fpNimTileTotalSBE},
        {"FpSxmTileTotalSBE", "FpSxmTileTotalSBE",
         &InventoryProcessorMetricsPublisher::fpSxmTileTotalSBE},
        {"FpVxmTileTotalSBE", "FpVxmTileTotalSBE",
         &InventoryProcessorMetricsPublisher::fpVxmTileTotalSBE},
        {"FpC2cTotalMBE", "FpC2cTotalMBE",
         &InventoryProcessorMetricsPublisher::fpC2cTotalMBE},
        {"FpIcuTileMisc0TotalMBE", "FpIcuTileMisc0TotalMBE",
         &InventoryProcessorMetricsPublisher::fpIcuTileMisc0TotalMBE},
        {"FpIcuTileVxmTotalMBE", "FpIcuTileVxmTotalMBE",
         &InventoryProcessorMetricsPublisher::fpIcuTileVxmTotalMBE},
        {"FpNimTileTotalMBE", "FpNimTileTotalMBE",
         &InventoryProcessorMetricsPublisher::fpNimTileTotalMBE},
        {"FpSxmTileTotalMBE", "FpSxmTileTotalMBE",
         &InventoryProcessorMetricsPublisher::fpSxmTileTotalMBE},
        {"FpVxmTileTotalMBE", "FpVxmTileTotalMBE",
         &InventoryProcessorMetricsPublisher::fpVxmTileTotalMBE},
    }};

    bool applyFpErrorCounterRow(const MetricRow& row, const uint8_t* buf,
                                uint8_t len, bool& matched)
    {
        matched = false;
        for (const FpErrorCounterBinding& binding : fpErrorCounterBindings)
        {
            if (row.auxiliaryName != binding.auxiliaryName)
            {
                continue;
            }
            matched = true;
            if (!metricsInterface)
            {
                lg2::error(
                    "LPU_Metrics D-Bus metrics interface missing for {NAME} inventory {PATH}",
                    "NAME", row.displayName, "PATH", inventoryPath);
                return false;
            }
            uint32_t value = 0;
            if (!decodeStatusLeU32(buf, len, value, row.displayName.c_str()))
            {
                return false;
            }
            this->*(binding.slot) = value;
            metricsInterface->set_property(binding.dbusPropertyName, value);
            return true;
        }
        return false;
    }

    std::shared_ptr<sdbusplus::asio::dbus_interface> assetInterface;
    std::string assetName{"NA"};
    std::string assetPartNumber{"NA"};
    std::string assetSku{"NA"};
    std::string assetSerialNumber{"NA"};
};

struct LpuC2CPortInventoryPublisher
{
    LpuC2CPortInventoryPublisher(sdbusplus::asio::object_server& objServer,
                                 std::string processorPath) :
        objectServer(objServer),
        parentProcessorPath(std::move(processorPath))
    {
        try
        {
            for (uint8_t port = 0; port < kLpuC2cPortCount; ++port)
            {
                publishPort(port);
            }
        }
        catch (...)
        {
            removeInterfaces();
            throw;
        }
    }

    LpuC2CPortInventoryPublisher(const LpuC2CPortInventoryPublisher&) = delete;
    LpuC2CPortInventoryPublisher&
        operator=(const LpuC2CPortInventoryPublisher&) = delete;

    ~LpuC2CPortInventoryPublisher()
    {
        removeInterfaces();
    }

    bool updateLinkStatus(const std::string& processorPath,
                          const PortLinkLockedPerLane& linkLockedPerLane)
    {
        if (processorPath != parentProcessorPath)
        {
            return false;
        }

        for (size_t port = 0; port < kLpuC2cPortCount; ++port)
        {
            const bool linkUp =
                std::all_of(linkLockedPerLane[port].begin(),
                            linkLockedPerLane[port].end(),
                            [](uint8_t locked) { return locked != 0; });
            portStateInterfaces[port]->set_property(
                "LinkStatus", linkUp ? kPortLinkStatusUp : kPortLinkStatusDown);
        }
        return true;
    }

  private:
    void removeInterfaces()
    {
        for (auto it = interfaces.rbegin(); it != interfaces.rend(); ++it)
        {
            objectServer.remove_interface(*it);
        }
        interfaces.clear();
        portStateInterfaces.fill(nullptr);
    }

    void addInitializedInterface(
        std::shared_ptr<sdbusplus::asio::dbus_interface> iface)
    {
        iface->initialize();
        interfaces.push_back(std::move(iface));
    }

    void publishPort(uint8_t portIndex)
    {
        const std::string portPath = parentProcessorPath + "/Ports/C2C_" +
                                     std::to_string(portIndex);

        addInitializedInterface(
            objectServer.add_interface(portPath, kPortItemInterface));

        auto portInfo = objectServer.add_interface(portPath, kPortInfoInterface);
        portInfo->register_property("Type", kPortTypeBidirectional);
        portInfo->register_property("Protocol", kPortProtocolOem);
        addInitializedInterface(std::move(portInfo));

        auto portState =
            objectServer.add_interface(portPath, kPortStateInterface);
        portState->register_property("LinkState", kPortLinkStateEnabled);
        portState->register_property("LinkStatus", kPortLinkStatusUp);
        portStateInterfaces[portIndex] = portState;
        addInitializedInterface(std::move(portState));

        auto associations =
            objectServer.add_interface(portPath, association::interface);
        std::vector<Association> assocs;
        assocs.emplace_back("parent_processor", "all_states",
                            parentProcessorPath);
        associations->register_property("Associations", std::move(assocs));
        addInitializedInterface(std::move(associations));
    }

    sdbusplus::asio::object_server& objectServer;
    std::string parentProcessorPath;
    std::vector<std::shared_ptr<sdbusplus::asio::dbus_interface>> interfaces;
    std::array<std::shared_ptr<sdbusplus::asio::dbus_interface>,
               kLpuC2cPortCount>
        portStateInterfaces{};
};

std::vector<std::shared_ptr<InventoryProcessorMetricsPublisher>> metricsPublishers;
std::vector<std::shared_ptr<LpuC2CPortInventoryPublisher>>
    c2cPortInventoryPublishers;

void updateC2CPortLinkStatus(const std::string& processorPath,
                             const PortLinkLockedPerLane& linkLockedPerLane)
{
    for (const auto& publisher : c2cPortInventoryPublishers)
    {
        if (publisher->updateLinkStatus(processorPath, linkLockedPerLane))
        {
            return;
        }
    }
}

/** Stable summary of grouped LPU_Metrics rows; skip EM refresh when unchanged. */
std::string metricsConfigFingerprint(
    const std::unordered_map<std::string, std::vector<MetricRow>>& groups,
    const std::set<std::string>& c2cPortParents)
{
    std::vector<std::string> lines;
    for (const auto& kv : groups)
    {
        for (const MetricRow& row : kv.second)
        {
            lines.push_back(kv.first + '\t' + row.displayName + '\t' +
                            std::to_string(row.busId) + '\t' +
                            std::to_string(row.i2cAddr) + '\t' +
                            std::to_string(row.offset) + '\t' +
                            std::to_string(row.readWidthBytes) + '\t' +
                            std::to_string(row.pollRateSec) + '\t' +
                            row.auxiliaryName);
        }
    }
    for (const std::string& parentPath : c2cPortParents)
    {
        lines.push_back(parentPath + "\tC2CPorts\t" +
                        std::to_string(kLpuC2cPortCount));
    }
    std::sort(lines.begin(), lines.end());
    std::string fp;
    fp.reserve(lines.size() * 48);
    for (const std::string& line : lines)
    {
        fp += line;
        fp += '\n';
    }
    return fp;
}

} // namespace lpu_metrics_detail

void lpuProcessorMetricsApplyManagedObjects(
    boost::asio::io_context& io, sdbusplus::asio::object_server& objectServer,
    const std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const ManagedObjectType& resp)
{
    std::unordered_map<std::string, std::vector<lpu_metrics_detail::MetricRow>>
        groups;
    std::set<std::string> c2cPortParents;

    for (const auto& pathPair : resp)
    {
        for (const auto& entry : pathPair.second)
        {
            if (!entry.first.starts_with(configInterfacePrefix))
            {
                continue;
            }
            const lpu::SensorBaseConfigMap& cfg = entry.second;
            if (cfg.find(lpu_em::kType) == cfg.end())
            {
                continue;
            }

            std::string recordType;
            if (!lpu_metrics_detail::configString(cfg, lpu_em::kType, recordType))
            {
                continue;
            }
            if (entry.first != configInterfaceName(recordType))
            {
                continue;
            }
            if (recordType != lpu_em::kRecordTypeLpuMetrics)
            {
                continue;
            }

            std::string displayName;
            if (!lpu_metrics_detail::configString(cfg, lpu_em::kName, displayName))
            {
                lg2::error("LPU_Metrics row missing Name at path {PATH}", "PATH",
                           std::string(pathPair.first));
                continue;
            }

            std::string auxiliaryName;
            if (!lpu_metrics_detail::configString(cfg, lpu_em::kAuxiliaryName,
                                                   auxiliaryName))
            {
                lg2::error("LPU_Metrics row {NAME} missing AuxiliaryName", "NAME",
                           displayName);
                continue;
            }

            // Entity Manager persists its composed configuration across image
            // updates. Ignore the retired Part Bin row even when it remains in
            // that cache after the source JSON has removed it.
            if (auxiliaryName ==
                lpu_metrics_detail::kDeprecatedVoltageBinAuxiliaryName)
            {
                continue;
            }

            std::string invPath;
            if (!lpu_common::parseLpuInstanceToInventoryPath(cfg, invPath,
                                                            displayName))
            {
                continue;
            }

            if (auxiliaryName == lpu_metrics_detail::kC2cClockAuxiliaryName ||
                auxiliaryName ==
                    lpu_metrics_detail::kSerdesLinkLockAuxiliaryName ||
                auxiliaryName ==
                    lpu_metrics_detail::kSerdesFecErrorAuxiliaryName)
            {
                c2cPortParents.insert(invPath);
            }

            std::string parseErr;
            lpu_metrics_detail::MetricRow row;
            row.displayName = displayName;
            row.auxiliaryName = std::move(auxiliaryName);
            if (!lpu_metrics_detail::tryParseI2cFields(
                    cfg, row.busId, row.i2cAddr, row.offset, row.readWidthBytes,
                    parseErr))
            {
                lg2::error(
                    "LPU_Metrics row {NAME} I2C config error: {ERR}", "NAME",
                    displayName, "ERR", parseErr);
                continue;
            }
            row.pollRateSec = getPollRate(
                cfg, lpu_metrics_detail::defaultPollRateForOffset(row.offset));

            groups[std::move(invPath)].push_back(std::move(row));
        }
    }

    static std::string lastMetricsConfigFingerprint;
    const std::string fingerprint =
        lpu_metrics_detail::metricsConfigFingerprint(groups, c2cPortParents);
    if (fingerprint == lastMetricsConfigFingerprint)
    {
        return;
    }
    lastMetricsConfigFingerprint = fingerprint;

    // Drop previous publishers so sd_bus paths are freed before we register
    // again (otherwise refresh hits FileExists on same connection).
    lpu_metrics_detail::c2cPortInventoryPublishers.clear();
    lpu_metrics_detail::metricsPublishers.clear();

    std::vector<std::shared_ptr<lpu_metrics_detail::InventoryProcessorMetricsPublisher>>
        nextPublishers;
    nextPublishers.reserve(groups.size());
    std::vector<std::shared_ptr<lpu_metrics_detail::LpuC2CPortInventoryPublisher>>
        nextC2CPortPublishers;
    nextC2CPortPublishers.reserve(c2cPortParents.size());

    if (groups.empty())
    {
        lg2::info(
            "LPU_Metrics: no configuration rows grouped (check EM exposes "
            "xyz.openbmc_project.Configuration.LPU_Metrics and LPUInstance)");
    }

    for (auto& kv : groups)
    {
        try
        {
            nextPublishers.push_back(
                lpu_metrics_detail::InventoryProcessorMetricsPublisher::create(
                    io, objectServer, dbusConnection, kv.first,
                    std::move(kv.second)));
        }
        catch (const std::exception& e)
        {
            lg2::error(
                "LPU_Metrics failed to publish interface on {PATH}: {ERR}", "PATH",
                kv.first, "ERR", e.what());
        }
    }

    for (const std::string& parentPath : c2cPortParents)
    {
        try
        {
            nextC2CPortPublishers.push_back(
                std::make_shared<lpu_metrics_detail::LpuC2CPortInventoryPublisher>(
                    objectServer, parentPath));
        }
        catch (const std::exception& e)
        {
            lg2::error(
                "LPU_Metrics failed to publish C2C port inventory on {PATH}: {ERR}",
                "PATH", parentPath, "ERR", e.what());
        }
    }

    if (!nextPublishers.empty())
    {
        lg2::info(
            "LPU_Metrics: published {COUNT} inventory object(s) with "
            "com.nvidia.LPUProcessorMetrics and Inventory.Decorator.Asset",
            "COUNT", nextPublishers.size());
    }

    if (!nextC2CPortPublishers.empty())
    {
        lg2::info("LPU_Metrics: published {COUNT} C2C port inventory object(s)",
                  "COUNT",
                  nextC2CPortPublishers.size() *
                      lpu_metrics_detail::kLpuC2cPortCount);
    }

    nextPublishers.swap(lpu_metrics_detail::metricsPublishers);
    nextC2CPortPublishers.swap(
        lpu_metrics_detail::c2cPortInventoryPublishers);
    for (const auto& publisher : lpu_metrics_detail::metricsPublishers)
    {
        publisher->refreshPortLinkStatus();
    }
}

bool lpuProcessorMetricsPublishersEmpty()
{
    return lpu_metrics_detail::metricsPublishers.empty() &&
           lpu_metrics_detail::c2cPortInventoryPublishers.empty();
}
