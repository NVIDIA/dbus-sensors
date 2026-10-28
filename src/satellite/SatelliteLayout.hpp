#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <istream>
#include <map>
#include <string>

// The HMC temperature in the telemetry EEPROM, 24.8 fixed point, fraction
// byte first: virtual-i2c-target writes it, satellitesensor reads it. The
// integer is 24-bit two's complement, the fraction in 1/256 of the absolute
// value. satellitesensor takes the offset from entity-manager;
// virtual-i2c-target places it by the layout version it writes at byte 24.
namespace satellite_layout
{

inline std::array<uint8_t, 4> encodeTemperature(long millidegrees)
{
    const unsigned long value =
        millidegrees < 0 ? -static_cast<unsigned long>(millidegrees)
                         : millidegrees;
    auto integer = static_cast<uint32_t>(value / 1000);
    integer = millidegrees < 0 ? (0x1000000 - integer) & 0xffffff : integer;
    return {static_cast<uint8_t>((value % 1000) * 256 / 1000),
            static_cast<uint8_t>(integer), static_cast<uint8_t>(integer >> 8),
            static_cast<uint8_t>(integer >> 16)};
}

// Layout version 4 (HGX) puts the block at 0xEF, one byte before version 5.
inline std::size_t temperatureOffset(int layoutVersion)
{
    return layoutVersion == 4 ? 0xEF : 0xF0;
}

// virtual-i2c-target.conf: key=value lines, numbers as in C; '#' starts a
// comment. Values that are not numbers are skipped.
inline std::map<std::string, unsigned> readConfig(std::istream& in)
{
    std::map<std::string, unsigned> config;
    for (std::string line; std::getline(in, line);)
    {
        const auto eq = line.find('=');
        if (line.starts_with('#') || eq == std::string::npos)
        {
            continue;
        }
        const std::string value = line.substr(eq + 1);
        char* end = nullptr;
        const unsigned long number = std::strtoul(value.c_str(), &end, 0);
        if (end != value.c_str() && *end == '\0')
        {
            config[line.substr(0, eq)] = static_cast<unsigned>(number);
        }
    }
    return config;
}

inline double decodeTemperature(const uint8_t* rawData)
{
    // this automatic convert to int (two's complement integer)
    int32_t intg =
        (rawData[3] << 24 | rawData[2] << 16 | rawData[1] << 8 | rawData[0]);
    uint8_t frac = uint8_t(intg & 0xFF);
    intg >>= 8; // shift operation on a int keep the signal in two's complement

    double temp = 0;
    if (intg > 0)
    {
        temp = double(intg) + double(frac / 256.0);
    }
    else
    {
        temp = double(intg) - double(frac / 256.0);
    }

    return temp;
}

} // namespace satellite_layout
