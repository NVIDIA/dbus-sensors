#include "LpuCommon.hpp"

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cerrno>
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
#include <utility>

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

/** Sysfs value=0xHH.. is wire-order (MSB first); widths > 8 do not fit uint64. */
bool parseSysfsValueHexWireBytes(const char* line, unsigned width,
                                 uint8_t* out)
{
    if (line == nullptr || out == nullptr || width == 0 ||
        width > kMaxLpu850ReadLength)
    {
        return false;
    }
    const char* v = std::strstr(line, "value=0x");
    if (v == nullptr)
    {
        return false;
    }
    v += 8;

    size_t digitCount = 0;
    while (std::isxdigit(static_cast<unsigned char>(v[digitCount])) != 0)
    {
        ++digitCount;
    }
    if (digitCount == 0)
    {
        return false;
    }

    const size_t needDigits = static_cast<size_t>(width) * 2U;
    if (width > sizeof(uint64_t) && digitCount != needDigits)
    {
        return false;
    }
    std::array<char, kMaxLpu850ReadLength * 2U + 1U> hexBuf{};
    size_t pad = 0;
    if (digitCount < needDigits)
    {
        pad = needDigits - digitCount;
        std::memset(hexBuf.data(), '0', pad);
    }
    else if (digitCount > needDigits)
    {
        v += digitCount - needDigits;
        digitCount = needDigits;
        pad = 0;
    }
    std::memcpy(hexBuf.data() + pad, v, digitCount);

    for (unsigned i = 0; i < width; ++i)
    {
        unsigned byte = 0;
        const char* begin =
            hexBuf.data() + static_cast<size_t>(i) * 2U;
        const auto result = std::from_chars(begin, begin + 2, byte, 16);
        if (result.ec != std::errc{} || result.ptr != begin + 2)
        {
            return false;
        }
        out[i] = static_cast<uint8_t>(byte);
    }
    return true;
}

/** SMBus telemetry with leading status byte; sysfs value=0x is wire-order hex.
 */
bool sysfsRegisterUsesWireHex(unsigned off, unsigned width)
{
    (void)off;
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

std::optional<std::string_view> pmbusCacheFileForOffset(uint16_t offset)
{
    struct Range
    {
        uint16_t base;
        std::string_view file;
    };
    static constexpr std::array<Range, 5> ranges = {{
        {lpu_pmbus::vddCoreBaseOffset, "pmbus_vdd_core_regs"},
        {lpu_pmbus::vddC2cBaseOffset, "pmbus_vdd_c2c_regs"},
        {lpu_pmbus::vddmBaseOffset, "pmbus_vddm_regs"},
        {lpu_pmbus::vddhC2cBaseOffset, "pmbus_vddh_c2c_regs"},
        {lpu_pmbus::vddlC2cBaseOffset, "pmbus_vddl_c2c_regs"},
    }};

    for (const Range& range : ranges)
    {
        if (offset >= range.base &&
            offset <= range.base + lpu_pmbus::lastRegisterSubOffset)
        {
            return range.file;
        }
    }
    return std::nullopt;
}

struct CachedSysfsRegister
{
    unsigned width{};
    unsigned long long value{};
    unsigned long long lastPolled{};
    std::string line;
};

struct CachedSysfsDump
{
    std::chrono::steady_clock::time_point updatedAt{};
    std::map<unsigned, CachedSysfsRegister> registers;
};

/** Read and parse each generated sysfs dump once per metric sampling group. */
const CachedSysfsRegister* cachedSysfsRegister(const char* path,
                                               uint16_t offset)
{
    static std::map<std::string, CachedSysfsDump> cache;
    constexpr auto cacheLifetime = std::chrono::milliseconds(500);

    const auto now = std::chrono::steady_clock::now();
    auto& entry = cache[path];
    if (now - entry.updatedAt >= cacheLifetime)
    {
        entry.updatedAt = now;
        entry.registers.clear();

        if (FILE* f = std::fopen(path, "r"))
        {
            std::array<char, 512> line{};
            while (std::fgets(line.data(), line.size(), f) != nullptr)
            {
                const char* p = std::strstr(line.data(), "off=0x");
                if (p == nullptr)
                {
                    continue;
                }

                unsigned parsedOffset = 0;
                CachedSysfsRegister reg;
                if (!parseUnsignedField(p, "off=0x", 16, parsedOffset) ||
                    !parseUnsignedField(p, "width=", 10, reg.width) ||
                    !parseUnsignedField(p, "last_polled_ns=", 10,
                                        reg.lastPolled))
                {
                    continue;
                }

                const char* value = std::strstr(p, "value=0x");
                if (value == nullptr ||
                    (reg.width <= sizeof(reg.value) &&
                     !parseUnsignedField(p, "value=0x", 16, reg.value)))
                {
                    continue;
                }
                reg.line = line.data();
                entry.registers.insert_or_assign(parsedOffset, std::move(reg));
            }
            std::fclose(f);
        }
    }

    auto reg = entry.registers.find(static_cast<unsigned>(offset));
    return reg == entry.registers.end() ? nullptr : &reg->second;
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

    std::array<std::string_view, 2> files{};
    size_t fileCount = 0;
    const std::optional<std::string_view> pmbusFile =
        pmbusCacheFileForOffset(offset);
    if (pmbusFile)
    {
        files[fileCount++] = *pmbusFile;
    }
    else if (isFastRegister(offset))
    {
        files[fileCount++] = "fast_regs";
        files[fileCount++] = "slow_regs";
    }
    else
    {
        files[fileCount++] = "slow_regs";
        files[fileCount++] = "fast_regs";
    }
    std::array<char, 96> path{};
    for (size_t fileIndex = 0; fileIndex < fileCount; ++fileIndex)
    {
        const std::string_view file = files[fileIndex];
        std::snprintf(
            path.data(), path.size(),
            "/sys/bus/i2c/devices/%llu-0067/lpu850/%.*s",
            static_cast<unsigned long long>(bus),
            static_cast<int>(file.size()), file.data());
        const CachedSysfsRegister* reg =
            cachedSysfsRegister(path.data(), offset);
        if (reg == nullptr)
        {
            continue;
        }

        const unsigned off = static_cast<unsigned>(offset);
        const unsigned width = reg->width;
        const unsigned long long value = reg->value;
        if (reg->lastPolled == 0)
        {
            continue;
        }
        if (maxSampleAge > std::chrono::nanoseconds::zero())
        {
            const auto now =
                std::chrono::system_clock::now().time_since_epoch();
            const auto nowNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(now)
                    .count();
            if (nowNs <= 0 ||
                reg->lastPolled > static_cast<unsigned long long>(nowNs) ||
                static_cast<unsigned long long>(nowNs) - reg->lastPolled >
                    static_cast<unsigned long long>(maxSampleAge.count()))
            {
                continue;
            }
        }
        if (pmbusFile && width != static_cast<unsigned>(len))
        {
            continue;
        }

        /* Entity-manager ReadWidthBytes includes a leading status byte for
         * most rows. Accept the current full status+payload sysfs record or a
         * legacy payload-only record, synthesizing status 0 for the latter.
         *
         * Full-width sysfs values: wire-order hex (status in byte 0) for
         * widths 3/5/9. */
        const unsigned lenU = static_cast<unsigned>(len);
        if (width == lenU)
        {
            if (pmbusFile || sysfsRegisterUsesWireHex(off, width))
            {
                if (!parseSysfsValueHexWireBytes(reg->line.c_str(), width, out))
                {
                    continue;
                }
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
            if (sysfsRegisterUsesWireHex(off, lenU))
            {
                out[0] = 0;
                if (!parseSysfsValueHexWireBytes(reg->line.c_str(), width,
                                                 out + 1U))
                {
                    continue;
                }
            }
            else
            {
                out[0] = 0;
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
