#include "LpuCommon.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lpu_common
{

namespace
{

constexpr uint16_t spiCrcErrorCountOffset = 0x0006U;
constexpr uint16_t faultTelemetryFirstOffset = 0x0008U;
constexpr uint16_t faultTelemetryLastOffset = 0x0015U;
constexpr uint16_t activityMonitorOffset = 0x0100U;
constexpr uint16_t ticksOffset = 0x0017U;

std::optional<uint8_t> narrowUint8(uint64_t v, const char* errMsg)
{
    if (v > std::numeric_limits<uint8_t>::max())
    {
        lg2::error("{MSG}", "MSG", errMsg);
        return std::nullopt;
    }
    return static_cast<uint8_t>(v);
}

std::optional<uint8_t> narrowUint8(int64_t v, const char* errMsg)
{
    if (v < 0 || static_cast<uint64_t>(v) > std::numeric_limits<uint8_t>::max())
    {
        lg2::error("{MSG}", "MSG", errMsg);
        return std::nullopt;
    }
    return static_cast<uint8_t>(v);
}

std::optional<uint8_t> narrowUint8(double v, const char* errMsg)
{
    if (!std::isfinite(v) || v < 0.0 ||
        v > static_cast<double>(std::numeric_limits<uint8_t>::max()) ||
        v != std::trunc(v))
    {
        lg2::error("{MSG}", "MSG", errMsg);
        return std::nullopt;
    }
    return static_cast<uint8_t>(v);
}

std::optional<uint16_t> narrowUint16(uint64_t v, const char* errMsg)
{
    if (v > std::numeric_limits<uint16_t>::max())
    {
        lg2::error("{MSG}", "MSG", errMsg);
        return std::nullopt;
    }
    return static_cast<uint16_t>(v);
}

std::optional<uint16_t> narrowUint16(int64_t v, const char* errMsg)
{
    if (v < 0 ||
        static_cast<uint64_t>(v) > std::numeric_limits<uint16_t>::max())
    {
        lg2::error("{MSG}", "MSG", errMsg);
        return std::nullopt;
    }
    return static_cast<uint16_t>(v);
}

std::optional<uint16_t> narrowUint16(double v, const char* errMsg)
{
    if (!std::isfinite(v) || v < 0.0 ||
        v > static_cast<double>(std::numeric_limits<uint16_t>::max()) ||
        v != std::trunc(v))
    {
        lg2::error("{MSG}", "MSG", errMsg);
        return std::nullopt;
    }
    return static_cast<uint16_t>(v);
}

/* 8-byte little-endian ATE OTP ECID decoded. */
std::string formatLpuEcidSerialStringLe(const uint8_t* buf)
{
    if (buf == nullptr)
    {
        return "NA";
    }
    const uint32_t ecid0 =
        static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8U) |
        (static_cast<uint32_t>(buf[2]) << 16U) |
        (static_cast<uint32_t>(buf[3]) << 24U);
    const uint32_t ecid1 =
        static_cast<uint32_t>(buf[4]) | (static_cast<uint32_t>(buf[5]) << 8U) |
        (static_cast<uint32_t>(buf[6]) << 16U) |
        (static_cast<uint32_t>(buf[7]) << 24U);

    const auto lotIdVal = static_cast<uint32_t>(
        ((ecid0 >> 24U) & 0xFFU) | ((ecid1 & 0x1FFFU) << 8U));
    const uint32_t waferNum = (ecid0 >> 19U) & 0x1FU;
    const uint32_t xPos = (ecid0 >> 11U) & 0xFFU;
    const uint32_t yPos = (ecid0 >> 3U) & 0xFFU;

    static constexpr std::string_view lotIdChars =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string lotId;
    uint32_t value = lotIdVal;
    while (value > 0U)
    {
        lotId.insert(lotId.begin(), lotIdChars[value % 36U]);
        value /= 36U;
    }
    lotId = "F" + lotId;

    std::array<char, 128> out{};
    if (std::snprintf(out.data(), out.size(), "%s-W%u-%u-%u", lotId.c_str(),
                      static_cast<unsigned>(waferNum),
                      static_cast<unsigned>(xPos),
                      static_cast<unsigned>(yPos)) <= 0)
    {
        return "NA";
    }
    return std::string(out.data());
}
} // namespace

std::optional<uint8_t> parseI2cAddress(const lpu::PropertyBaseConfigMap& cfg)
{
    auto it = cfg.find("Address");
    if (it == cfg.end())
    {
        it = cfg.find("I2CAddress");
    }
    if (it == cfg.end())
    {
        lg2::error("I2CAddress missing");
        return std::nullopt;
    }
    const BasicVariantType& v = it->second;
    if (const auto* s = std::get_if<std::string>(&v))
    {
        errno = 0;
        char* end = nullptr;
        const unsigned long ul = strtoul(s->c_str(), &end, 0);
        if (end == s->c_str())
        {
            lg2::error("Address string is invalid");
            return std::nullopt;
        }
        if (*end != '\0')
        {
            lg2::error("Address string has invalid trailing characters");
            return std::nullopt;
        }
        if (errno == ERANGE || ul > std::numeric_limits<uint8_t>::max())
        {
            lg2::error("Address string is out of range");
            return std::nullopt;
        }
        return static_cast<uint8_t>(ul);
    }
    if (const auto* u = std::get_if<uint64_t>(&v))
    {
        return narrowUint8(*u, "Address value out of uint8_t range");
    }
    if (const auto* i = std::get_if<int64_t>(&v))
    {
        return narrowUint8(*i, "Address value out of uint8_t range");
    }
    if (const auto* u8 = std::get_if<uint8_t>(&v))
    {
        return *u8;
    }
    if (const auto* u32 = std::get_if<uint32_t>(&v))
    {
        return narrowUint8(static_cast<uint64_t>(*u32),
                           "Address value out of uint8_t range");
    }
    if (const auto* u16 = std::get_if<uint16_t>(&v))
    {
        return narrowUint8(static_cast<uint64_t>(*u16),
                           "Address value out of uint8_t range");
    }
    lg2::error("Address has unsupported type");
    return std::nullopt;
}

std::optional<uint8_t> parseReadWidthBytes(
    const lpu::PropertyBaseConfigMap& cfg)
{
    auto it = cfg.find("ReadWidthBytes");
    if (it == cfg.end())
    {
        lg2::error("ReadWidthBytes missing");
        return std::nullopt;
    }
    const BasicVariantType& v = it->second;
    if (const auto* u = std::get_if<uint64_t>(&v))
    {
        return narrowUint8(*u, "ReadWidthBytes value out of uint8_t range");
    }
    if (const auto* i = std::get_if<int64_t>(&v))
    {
        return narrowUint8(*i, "ReadWidthBytes value out of uint8_t range");
    }
    if (const auto* u8 = std::get_if<uint8_t>(&v))
    {
        return *u8;
    }
    if (const auto* u32 = std::get_if<uint32_t>(&v))
    {
        return narrowUint8(static_cast<uint64_t>(*u32),
                           "ReadWidthBytes value out of uint8_t range");
    }
    if (const auto* u16 = std::get_if<uint16_t>(&v))
    {
        return narrowUint8(static_cast<uint64_t>(*u16),
                           "ReadWidthBytes value out of uint8_t range");
    }
    if (const auto* d = std::get_if<double>(&v))
    {
        return narrowUint8(*d, "ReadWidthBytes double out of uint8_t range");
    }
    lg2::error("ReadWidthBytes has unsupported type");
    return std::nullopt;
}

std::optional<uint16_t> parseOffsetValue(const lpu::PropertyBaseConfigMap& cfg)
{
    auto it = cfg.find("OffsetValue");
    if (it == cfg.end())
    {
        lg2::error("OffsetValue missing");
        return std::nullopt;
    }
    const BasicVariantType& v = it->second;
    if (const auto* u = std::get_if<uint64_t>(&v))
    {
        return narrowUint16(*u, "OffsetValue out of uint16_t range");
    }
    if (const auto* i = std::get_if<int64_t>(&v))
    {
        return narrowUint16(*i, "OffsetValue out of uint16_t range");
    }
    if (const auto* u16 = std::get_if<uint16_t>(&v))
    {
        return *u16;
    }
    if (const auto* u32 = std::get_if<uint32_t>(&v))
    {
        return narrowUint16(static_cast<uint64_t>(*u32),
                            "OffsetValue out of uint16_t range");
    }
    if (const auto* d = std::get_if<double>(&v))
    {
        return narrowUint16(*d, "OffsetValue double out of uint16_t range");
    }
    lg2::error("OffsetValue has unsupported type");
    return std::nullopt;
}

std::optional<uint8_t> parseBusId(const lpu::PropertyBaseConfigMap& cfg)
{
    auto it = cfg.find("Bus");
    if (it == cfg.end())
    {
        it = cfg.find("I2CBus");
    }
    if (it == cfg.end())
    {
        lg2::error("Bus or I2CBus missing");
        return std::nullopt;
    }
    const BasicVariantType& v = it->second;
    if (const auto* u = std::get_if<uint64_t>(&v))
    {
        return narrowUint8(*u, "Bus value out of uint8_t range");
    }
    if (const auto* i = std::get_if<int64_t>(&v))
    {
        return narrowUint8(*i, "Bus value out of uint8_t range");
    }
    if (const auto* u8 = std::get_if<uint8_t>(&v))
    {
        return *u8;
    }
    if (const auto* u32 = std::get_if<uint32_t>(&v))
    {
        return narrowUint8(static_cast<uint64_t>(*u32),
                           "Bus value out of uint8_t range");
    }
    if (const auto* u16 = std::get_if<uint16_t>(&v))
    {
        return narrowUint8(static_cast<uint64_t>(*u16),
                           "Bus value out of uint8_t range");
    }
    if (const auto* d = std::get_if<double>(&v))
    {
        return narrowUint8(*d, "Bus double out of uint8_t range");
    }
    lg2::error("Bus has unsupported type");
    return std::nullopt;
}

std::optional<uint64_t> parseLpuInstance(const lpu::PropertyBaseConfigMap& cfg,
                                         const std::string& displayName)
{
    auto it = cfg.find(lpu_em::kLPUInstance);
    if (it == cfg.end())
    {
        lg2::error("LPU_Metrics row {NAME} missing LPUInstance", "NAME",
                   displayName);
        return std::nullopt;
    }

    uint64_t inst = 0;
    const BasicVariantType& v = it->second;
    if (const auto* u = std::get_if<uint64_t>(&v))
    {
        inst = *u;
    }
    else if (const auto* i = std::get_if<int64_t>(&v))
    {
        if (*i < 0)
        {
            lg2::error("LPU_Metrics row {NAME} has negative LPUInstance",
                       "NAME", displayName);
            return std::nullopt;
        }
        inst = static_cast<uint64_t>(*i);
    }
    else if (const auto* u32 = std::get_if<uint32_t>(&v))
    {
        inst = *u32;
    }
    else if (const auto* i32 = std::get_if<int32_t>(&v))
    {
        if (*i32 < 0)
        {
            lg2::error("LPU_Metrics row {NAME} has negative LPUInstance",
                       "NAME", displayName);
            return std::nullopt;
        }
        inst = static_cast<uint64_t>(*i32);
    }
    else if (const auto* u16 = std::get_if<uint16_t>(&v))
    {
        inst = *u16;
    }
    else if (const auto* i16 = std::get_if<int16_t>(&v))
    {
        if (*i16 < 0)
        {
            lg2::error("LPU_Metrics row {NAME} has negative LPUInstance",
                       "NAME", displayName);
            return std::nullopt;
        }
        inst = static_cast<uint64_t>(*i16);
    }
    else if (const auto* u8 = std::get_if<uint8_t>(&v))
    {
        inst = *u8;
    }
    else if (const auto* s = std::get_if<std::string>(&v))
    {
        if (s->empty() || (*s)[0] == '+' || (*s)[0] == '-' ||
            std::isspace(static_cast<unsigned char>((*s)[0])) != 0)
        {
            lg2::error(
                "LPU_Metrics row {NAME} LPUInstance string invalid",
                "NAME", displayName);
            return std::nullopt;
        }
        try
        {
            size_t parsedLength = 0;
            inst = std::stoull(*s, &parsedLength, 0);
            if (parsedLength != s->size())
            {
                throw std::invalid_argument("trailing characters");
            }
        }
        catch (const std::exception& e)
        {
            lg2::error(
                "LPU_Metrics row {NAME} LPUInstance string invalid: {ERR}",
                "NAME", displayName, "ERR", e.what());
            return std::nullopt;
        }
    }
    else if (const auto* d = std::get_if<double>(&v))
    {
        if (!std::isfinite(*d) || *d < 0.0 ||
            *d >= static_cast<double>(
                      std::numeric_limits<uint64_t>::max()) ||
            *d != std::trunc(*d))
        {
            lg2::error("LPU_Metrics row {NAME} LPUInstance double invalid",
                       "NAME", displayName);
            return std::nullopt;
        }
        inst = static_cast<uint64_t>(*d);
    }
    else
    {
        lg2::error("LPU_Metrics row {NAME} LPUInstance unsupported type",
                   "NAME", displayName);
        return std::nullopt;
    }

    if (inst > 255U)
    {
        lg2::error(
            "LPU_Metrics row {NAME} LPUInstance {INST} out of supported range",
            "NAME", displayName, "INST", inst);
        return std::nullopt;
    }

    return inst;
}

bool parseLpuInstanceToInventoryPath(const lpu::PropertyBaseConfigMap& cfg,
                                     std::string& outPath,
                                     const std::string& displayName)
{
    const std::optional<uint64_t> inst = parseLpuInstance(cfg, displayName);
    if (!inst)
    {
        return false;
    }

    outPath = "/xyz/openbmc_project/inventory/system/accelerator/LPU_" +
              std::to_string(*inst);
    return true;
}

std::optional<std::string> optionalConfigString(
    const lpu::PropertyBaseConfigMap& cfg, const char* key)
{
    auto it = cfg.find(key);
    if (it == cfg.end())
    {
        return std::nullopt;
    }
    if (const auto* s = std::get_if<std::string>(&it->second))
    {
        if (s->empty())
        {
            return std::nullopt;
        }
        return *s;
    }
    return std::nullopt;
}

namespace
{
constexpr size_t kMaxLpu850ReadLength = 97;

std::optional<std::string> findContainingChassisForLpu(
    std::string_view configParent, const GetSubTreeType& subtree)
{
    // A parent that is a chassis takes precedence.
    for (const auto& [obj, services] : subtree)
    {
        if (obj == configParent)
        {
            return obj;
        }
    }

    // Otherwise associate the sensor with the system chassis.
    for (const auto& [obj, services] : subtree)
    {
        for (const auto& [service, interfaces] : services)
        {
            if (std::find(interfaces.begin(), interfaces.end(),
                          "xyz.openbmc_project.Inventory.Item.System") !=
                interfaces.end())
            {
                return obj;
            }
        }
    }
    return std::nullopt;
}

template <typename T>
bool parseUnsignedField(std::string_view line, std::string_view prefix,
                        int base, T& value)
{
    const size_t field = line.find(prefix);
    if (field == std::string_view::npos)
    {
        return false;
    }

    const size_t first = field + prefix.size();
    const std::string_view digits =
        base == 16 ? "0123456789abcdefABCDEF" : "0123456789";
    size_t last = line.find_first_not_of(digits, first);
    if (last == std::string_view::npos)
    {
        last = line.size();
    }
    if (last == first)
    {
        return false;
    }

    const auto result =
        std::from_chars(line.data() + first, line.data() + last, value, base);
    return result.ec == std::errc{} && result.ptr == line.data() + last;
}

/** SMBus telemetry with leading status byte; sysfs value=0x is wire-order hex.
 */
bool sysfsRegisterUsesWireHex(unsigned width)
{
    constexpr unsigned statusByteWidth = sizeof(uint8_t);
    return width == statusByteWidth + sizeof(uint16_t) ||
           width == statusByteWidth + sizeof(uint32_t) ||
           width == statusByteWidth + sizeof(uint64_t) ||
           width > sizeof(uint64_t);
}

bool isFastRegister(uint16_t offset)
{
    return offset == spiCrcErrorCountOffset ||
           (offset >= faultTelemetryFirstOffset &&
            offset <= faultTelemetryLastOffset) ||
           offset == activityMonitorOffset || offset == ticksOffset;
}

/** Generated register dumps exposed by the lpu850 driver for one device. */
enum class SysfsDump
{
    fastRegs,
    slowRegs,
    pmbusVddCore,
    pmbusVddC2c,
    pmbusVddm,
    pmbusVddhC2c,
    pmbusVddlC2c,
};

constexpr std::array<std::string_view, 7> sysfsDumpNames = {
    "fast_regs",           "slow_regs",       "pmbus_vdd_core_regs",
    "pmbus_vdd_c2c_regs",  "pmbus_vddm_regs", "pmbus_vddh_c2c_regs",
    "pmbus_vddl_c2c_regs",
};

std::optional<SysfsDump> pmbusDumpForOffset(uint16_t offset)
{
    struct Range
    {
        uint16_t base;
        SysfsDump dump;
    };
    static constexpr std::array<Range, 5> ranges = {{
        {lpu_pmbus::vddCoreBaseOffset, SysfsDump::pmbusVddCore},
        {lpu_pmbus::vddC2cBaseOffset, SysfsDump::pmbusVddC2c},
        {lpu_pmbus::vddmBaseOffset, SysfsDump::pmbusVddm},
        {lpu_pmbus::vddhC2cBaseOffset, SysfsDump::pmbusVddhC2c},
        {lpu_pmbus::vddlC2cBaseOffset, SysfsDump::pmbusVddlC2c},
    }};

    for (const Range& range : ranges)
    {
        if (offset >= range.base &&
            offset <= range.base + lpu_pmbus::lastRegisterSubOffset)
        {
            return range.dump;
        }
    }
    return std::nullopt;
}

/** One decoded dump line. The wire bytes live in SysfsSnapshot::bytes. */
struct SysfsRegister
{
    uint16_t offset{};
    size_t bytesBegin{};
    unsigned width{};
    unsigned long long value{};
    unsigned long long lastPolled{};
};

/** Parsed contents of one dump, refreshed at most once per cache lifetime.
 *
 *  The descriptor stays open between samples: opening a sysfs attribute costs
 *  a full kernfs path walk, and the kernel regenerates the dump on each
 *  pread() at offset zero, so one read per cycle returns fresh data without
 *  the open/fstat/close round trip that previously dominated system time.
 */
struct SysfsSnapshot
{
    int fd{-1};
    bool valid{false};
    /** Start of the sample pass that last read the dump; selects whether the
     *  cached copy is still within its lifetime. */
    std::chrono::steady_clock::time_point attemptedAt{};
    /** Wall clock right after the dump was read; every register stamp in the
     *  dump precedes it, so sample ages measured against it are never
     *  negative. */
    uint64_t readRealtimeNs{};
    /** Monotonic time taken with readRealtimeNs, so the time a snapshot has
     *  been reused since the read can be added to the sample age. */
    std::chrono::steady_clock::time_point readSteady{};
    std::vector<SysfsRegister> registers;
    std::vector<uint8_t> bytes;

    SysfsSnapshot() = default;
    SysfsSnapshot(const SysfsSnapshot&) = delete;
    SysfsSnapshot& operator=(const SysfsSnapshot&) = delete;
    SysfsSnapshot(SysfsSnapshot&& other) noexcept :
        fd(std::exchange(other.fd, -1)), valid(other.valid),
        attemptedAt(other.attemptedAt), readRealtimeNs(other.readRealtimeNs),
        readSteady(other.readSteady), registers(std::move(other.registers)),
        bytes(std::move(other.bytes))
    {}
    SysfsSnapshot& operator=(SysfsSnapshot&&) = delete;
    ~SysfsSnapshot()
    {
        closeDescriptor();
    }

    void closeDescriptor()
    {
        if (fd >= 0)
        {
            ::close(fd);
            fd = -1;
        }
    }

    const SysfsRegister* find(uint16_t offset) const
    {
        // A dump holds at most sixteen registers; a linear scan over this
        // contiguous array beats hashing for that size.
        for (const SysfsRegister& reg : registers)
        {
            if (reg.offset == offset)
            {
                return &reg;
            }
        }
        return nullptr;
    }
};

using DeviceSnapshots = std::array<SysfsSnapshot, sysfsDumpNames.size()>;

/** Decode one "<name> off=0x.. width=.. value=0x.. last_polled_ns=.." line. */
void parseSysfsDumpLine(SysfsSnapshot& snapshot, std::string_view line)
{
    const size_t fieldStart = line.find("off=0x");
    if (fieldStart == std::string_view::npos)
    {
        return;
    }
    line.remove_prefix(fieldStart);

    SysfsRegister reg;
    unsigned parsedOffset = 0;
    if (!parseUnsignedField(line, "off=0x", 16, parsedOffset) ||
        parsedOffset > std::numeric_limits<uint16_t>::max() ||
        !parseUnsignedField(line, "width=", 10, reg.width) ||
        !parseUnsignedField(line, "last_polled_ns=", 10, reg.lastPolled) ||
        reg.width == 0 || reg.width > kMaxLpu850ReadLength)
    {
        return;
    }
    reg.offset = static_cast<uint16_t>(parsedOffset);

    const size_t valueField = line.find("value=0x");
    if (valueField == std::string_view::npos)
    {
        return;
    }
    const size_t hexBegin = valueField + 8;
    size_t hexEnd = hexBegin;
    while (hexEnd < line.size() &&
           std::isxdigit(static_cast<unsigned char>(line[hexEnd])) != 0)
    {
        ++hexEnd;
    }
    std::string_view hex = line.substr(hexBegin, hexEnd - hexBegin);
    if (hex.empty())
    {
        return;
    }
    if (reg.width <= sizeof(reg.value) &&
        !parseUnsignedField(line, "value=0x", 16, reg.value))
    {
        return;
    }

    // The driver prints exactly width bytes. Tolerate legacy dumps by
    // zero-padding short values and keeping the trailing bytes of long ones,
    // which keeps the decoded payload identical to the previous parser.
    const size_t needDigits = static_cast<size_t>(reg.width) * 2U;
    if (reg.width > sizeof(uint64_t) && hex.size() != needDigits)
    {
        return;
    }
    if (hex.size() > needDigits)
    {
        hex.remove_prefix(hex.size() - needDigits);
    }
    const size_t padDigits = needDigits - hex.size();

    reg.bytesBegin = snapshot.bytes.size();
    for (size_t digit = 0; digit < needDigits; digit += 2)
    {
        unsigned byte = 0;
        if (digit + 2 > padDigits)
        {
            const size_t first = digit >= padDigits ? digit - padDigits : 0;
            const size_t count = digit >= padDigits ? 2 : 1;
            const auto result = std::from_chars(
                hex.data() + first, hex.data() + first + count, byte, 16);
            if (result.ec != std::errc{} ||
                result.ptr != hex.data() + first + count)
            {
                snapshot.bytes.resize(reg.bytesBegin);
                return;
            }
        }
        snapshot.bytes.push_back(static_cast<uint8_t>(byte));
    }

    for (SysfsRegister& existing : snapshot.registers)
    {
        if (existing.offset == reg.offset)
        {
            existing = reg;
            return;
        }
    }
    snapshot.registers.push_back(reg);
}

/** Current sample cycle, if one is active on this (single) thread. */
const SampleCycle* activeSampleCycle = nullptr;

std::chrono::steady_clock::time_point cycleSteadyNow()
{
    return activeSampleCycle != nullptr ? activeSampleCycle->steady()
                                        : std::chrono::steady_clock::now();
}

uint64_t realtimeNowNs()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto nowNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return nowNs > 0 ? static_cast<uint64_t>(nowNs) : 0U;
}

/** Return the parsed dump for one device, re-reading it after the cache
 *  lifetime. Returns nullptr while the attribute is absent or unreadable. */
const SysfsSnapshot* sysfsSnapshot(uint64_t bus, SysfsDump dump)
{
    static std::unordered_map<uint64_t, DeviceSnapshots> devices;
    // One page bounds every kernfs attribute read, so this buffer always
    // receives the complete dump. It is shared across all devices.
    static std::array<char, 4096> buffer;
    constexpr auto cacheLifetime = std::chrono::milliseconds(500);

    SysfsSnapshot& snapshot = devices[bus][static_cast<size_t>(dump)];
    const auto now = cycleSteadyNow();
    if (now - snapshot.attemptedAt < cacheLifetime)
    {
        return snapshot.valid ? &snapshot : nullptr;
    }

    snapshot.attemptedAt = now;
    snapshot.valid = false;
    snapshot.registers.clear();
    snapshot.bytes.clear();

    if (snapshot.fd < 0)
    {
        std::array<char, 96> path{};
        const std::string_view name = sysfsDumpNames[static_cast<size_t>(dump)];
        std::snprintf(path.data(), path.size(),
                      "/sys/bus/i2c/devices/%llu-0067/lpu850/%.*s",
                      static_cast<unsigned long long>(bus),
                      static_cast<int>(name.size()), name.data());
        snapshot.fd = ::open(path.data(), O_RDONLY | O_CLOEXEC);
        if (snapshot.fd < 0)
        {
            return nullptr;
        }
    }

    const ssize_t length =
        ::pread(snapshot.fd, buffer.data(), buffer.size(), 0);
    if (length < 0)
    {
        // ENODEV after the device was unbound; reopen on the next cycle.
        snapshot.closeDescriptor();
        return nullptr;
    }

    snapshot.readRealtimeNs = realtimeNowNs();
    snapshot.readSteady = std::chrono::steady_clock::now();
    std::string_view contents(buffer.data(), static_cast<size_t>(length));
    while (!contents.empty())
    {
        const size_t newline = contents.find('\n');
        if (newline == std::string_view::npos)
        {
            // A dump never exceeds one page, so an unterminated tail means a
            // partial line that must not be decoded.
            break;
        }
        parseSysfsDumpLine(snapshot, contents.substr(0, newline));
        contents.remove_prefix(newline + 1);
    }
    snapshot.valid = true;
    return &snapshot;
}

int readFromLpu850SysfsCache(uint64_t bus, uint16_t offset, uint8_t* out,
                             uint8_t len,
                             std::chrono::nanoseconds maxSampleAge,
                             uint64_t* lastPolledNs)
{
    if (lastPolledNs != nullptr)
    {
        *lastPolledNs = 0;
    }
    if (out == nullptr || len == 0 || len > kMaxLpu850ReadLength)
    {
        return -1;
    }

    std::array<SysfsDump, 2> dumps{};
    size_t dumpCount = 0;
    const std::optional<SysfsDump> pmbusDump = pmbusDumpForOffset(offset);
    if (pmbusDump)
    {
        dumps[dumpCount++] = *pmbusDump;
    }
    else if (isFastRegister(offset))
    {
        dumps[dumpCount++] = SysfsDump::fastRegs;
        dumps[dumpCount++] = SysfsDump::slowRegs;
    }
    else
    {
        dumps[dumpCount++] = SysfsDump::slowRegs;
        dumps[dumpCount++] = SysfsDump::fastRegs;
    }

    for (size_t dumpIndex = 0; dumpIndex < dumpCount; ++dumpIndex)
    {
        const SysfsSnapshot* snapshot = sysfsSnapshot(bus, dumps[dumpIndex]);
        if (snapshot == nullptr)
        {
            continue;
        }
        const SysfsRegister* reg = snapshot->find(offset);
        if (reg == nullptr)
        {
            continue;
        }

        const unsigned width = reg->width;
        const unsigned long long value = reg->value;
        if (reg->lastPolled == 0)
        {
            continue;
        }
        if (maxSampleAge > std::chrono::nanoseconds::zero())
        {
            // Age when the dump was read, plus the time the snapshot has been
            // reused since that read, so the limit holds for the whole cache
            // lifetime without reading the wall clock again for every sensor.
            const uint64_t readNs = snapshot->readRealtimeNs;
            const auto reused = cycleSteadyNow() - snapshot->readSteady;
            const uint64_t reusedNs =
                reused > std::chrono::steady_clock::duration::zero()
                    ? static_cast<uint64_t>(
                          std::chrono::duration_cast<std::chrono::nanoseconds>(
                              reused)
                              .count())
                    : 0U;
            if (readNs == 0 || reg->lastPolled > readNs ||
                (readNs - reg->lastPolled) + reusedNs >
                    static_cast<uint64_t>(maxSampleAge.count()))
            {
                continue;
            }
        }
        if (pmbusDump && width != static_cast<unsigned>(len))
        {
            continue;
        }
        const uint8_t* wireBytes = snapshot->bytes.data() + reg->bytesBegin;

        /* Entity-manager ReadWidthBytes includes a leading status byte for
         * most rows. Accept the current full status+payload sysfs record or a
         * legacy payload-only record, synthesizing status 0 for the latter.
         *
         * Full-width sysfs values: wire-order hex (status in byte 0) for
         * widths 3/5/9. */
        const unsigned lenU = static_cast<unsigned>(len);
        if (width == lenU)
        {
            if (pmbusDump || sysfsRegisterUsesWireHex(width))
            {
                std::memcpy(out, wireBytes, width);
            }
            else
            {
                const uint8_t lowByte = static_cast<uint8_t>(value & 0xffU);
                const uint8_t highByte =
                    static_cast<uint8_t>((value >> ((len - 1U) * 8U)) & 0xffU);

                if (lowByte == 0U)
                {
                    for (unsigned i = 0; i < len; ++i)
                    {
                        out[i] =
                            static_cast<uint8_t>((value >> (i * 8U)) & 0xffU);
                    }
                }
                else if (highByte == 0U)
                {
                    out[0] = highByte;
                    const unsigned payloadBytes = len - 1U;
                    unsigned long long payloadMask = 0;
                    for (unsigned i = 0; i < payloadBytes; ++i)
                    {
                        payloadMask = (payloadMask << 8U) | 0xffULL;
                    }
                    const unsigned long long payload = value & payloadMask;
                    for (unsigned i = 0; i < payloadBytes; ++i)
                    {
                        out[1U + i] = static_cast<uint8_t>(
                            (payload >> ((payloadBytes - 1U - i) * 8U)) &
                            0xffU);
                    }
                }
                else
                {
                    for (unsigned i = 0; i < len; ++i)
                    {
                        out[i] =
                            static_cast<uint8_t>((value >> (i * 8U)) & 0xffU);
                    }
                }
            }
        }
        else if (lenU > 1U && width == lenU - 1U)
        {
            out[0] = 0;
            if (sysfsRegisterUsesWireHex(lenU))
            {
                std::memcpy(out + 1U, wireBytes, width);
            }
            else
            {
                for (unsigned i = 0; i < width; ++i)
                {
                    out[1U + i] =
                        static_cast<uint8_t>((value >> (i * 8U)) & 0xffU);
                }
            }
        }
        else
        {
            continue;
        }
        if (lastPolledNs != nullptr)
        {
            *lastPolledNs = static_cast<uint64_t>(reg->lastPolled);
        }
        return 0;
    }

    return -1;
}
} // namespace

SampleCycle::SampleCycle() :
    steadyNow(std::chrono::steady_clock::now()),
    owner(activeSampleCycle == nullptr)
{
    if (owner)
    {
        activeSampleCycle = this;
    }
}

SampleCycle::~SampleCycle()
{
    if (owner)
    {
        activeSampleCycle = nullptr;
    }
}

bool isI2cEndpointBlocked(uint8_t bus, uint8_t addr)
{
    (void)bus;
    (void)addr;
    return false;
}

int i2cReadBytes(uint64_t bus, uint8_t addr, uint16_t offset, uint8_t* out,
                 uint8_t len, bool rejectIfResponseAllOnes,
                 std::chrono::nanoseconds maxSampleAge,
                 uint64_t* lastPolledNs)
{
    if (lastPolledNs != nullptr)
    {
        *lastPolledNs = 0;
    }
    if (len == 0 || len > kMaxLpu850ReadLength)
    {
        lg2::error("I2C read rejected: invalid len {L} (use 1-97)", "L", len);
        return -1;
    }
    if (addr != 0x67)
    {
        lg2::error(
            "I2C read rejected: unsupported address 0x{ADDR} for lpu850 sysfs cache",
            "ADDR", lg2::hex, static_cast<unsigned>(addr));
        return -1;
    }

    if (readFromLpu850SysfsCache(bus, offset, out, len, maxSampleAge,
                                 lastPolledNs) != 0)
    {
        return -1;
    }

    if (rejectIfResponseAllOnes)
    {
        bool allOnes = true;
        for (uint8_t i = 0; i < len; ++i)
        {
            if (out[i] != 0xFF)
            {
                allOnes = false;
                break;
            }
        }
        if (allOnes)
        {
            lg2::error(
                "I2C read rejected: all 0xFF data from lpu850 sysfs cache");
            return -1;
        }
    }

    return 0;
}

std::string formatRegisterVersionString(const uint8_t* buf, uint8_t len)
{
    if (buf == nullptr || (len != 4U && len != 5U) || buf[0] != 0U)
    {
        return "Unknown";
    }

    return std::to_string(buf[3]) + "." + std::to_string(buf[2]) + "." +
           std::to_string(buf[1]);
}

std::string formatLpuAssetSerialString(const uint8_t* buf, uint8_t len)
{
    if (len == 0 || buf == nullptr)
    {
        return "NA";
    }
    const uint8_t* payload = buf;
    uint8_t payloadLen = len;
    if (len == 9U)
    {
        if (buf[0] != 0)
        {
            return "NA";
        }
        payload = buf + 1;
        payloadLen = 8;
    }
    if (payloadLen == 8U)
    {
        return formatLpuEcidSerialStringLe(payload);
    }
    size_t n = 0;
    while (n < static_cast<size_t>(payloadLen) && payload[n] != 0)
    {
        ++n;
    }
    std::string s;
    s.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        s.push_back(static_cast<char>(payload[i]));
    }
    while (!s.empty() &&
           std::isspace(static_cast<unsigned char>(s.back())) != 0)
    {
        s.pop_back();
    }
    size_t lead = 0;
    while (lead < s.size() &&
           std::isspace(static_cast<unsigned char>(s[lead])) != 0)
    {
        ++lead;
    }
    if (lead > 0)
    {
        s.erase(0, lead);
    }
    return s.empty() ? "NA" : s;
}

void createInventoryAssoc(
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::shared_ptr<sdbusplus::asio::dbus_interface>& association,
    const std::string& path, AssociationList&& additionalAssociations)
{
    if (!association)
    {
        return;
    }

    constexpr auto allInterfaces = std::to_array({
        "xyz.openbmc_project.Inventory.Item.Board",
        "xyz.openbmc_project.Inventory.Item.Chassis",
    });
    std::weak_ptr<sdbusplus::asio::dbus_interface> weakRef = association;
    conn->async_method_call(
        [weakRef, path, additional = std::move(additionalAssociations)](
            const boost::system::error_code ec,
            const GetSubTreeType& subtree) mutable {
            auto association = weakRef.lock();
            if (!association)
            {
                return;
            }
            const std::string parent =
                std::filesystem::path(path).parent_path().string();
            const std::string chassis =
                ec ? parent
                   : findContainingChassisForLpu(parent, subtree)
                         .value_or(parent);
            AssociationList associations{
                {"inventory", "sensors", parent},
                {"chassis", "all_sensors", chassis},
            };
            for (Association& entry : additional)
            {
                if (std::find(associations.begin(), associations.end(), entry) ==
                    associations.end())
                {
                    associations.emplace_back(std::move(entry));
                }
            }
            association->register_property("Associations", associations);
            association->initialize();
        },
        mapper::busName, mapper::path, mapper::interface, "GetSubTree",
        "/xyz/openbmc_project/inventory/system", 2, allInterfaces);
}

} // namespace lpu_common
