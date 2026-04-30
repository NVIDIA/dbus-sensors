#include "OpenOCDPortForwardPorts.hpp"

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace openocd_port_forward
{

std::string trim(std::string value)
{
    auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(),
                value.end());
    return value;
}

std::vector<uint16_t> parsePorts(std::string_view portList)
{
    std::vector<uint16_t> ports;
    std::string normalized(portList);
    std::replace(normalized.begin(), normalized.end(), ',', ':');
    std::replace(normalized.begin(), normalized.end(), ';', ':');

    std::stringstream stream(normalized);
    std::string token;
    while (std::getline(stream, token, ':'))
    {
        token = trim(token);
        if (token.empty())
        {
            continue;
        }

        unsigned int parsed = 0;
        const char* begin = token.data();
        const char* end = begin + token.size();
        auto [ptr, ec] = std::from_chars(begin, end, parsed);
        if (ec != std::errc{} || ptr != end)
        {
            lg2::error("Invalid OpenOCD port token: {TOKEN}", "TOKEN", token);
            return {};
        }

        if (parsed == 0 || parsed > 65535)
        {
            lg2::error("OpenOCD port out of range: {PORT}", "PORT", token);
            return {};
        }
        ports.push_back(static_cast<uint16_t>(parsed));
    }

    if (ports.empty())
    {
        lg2::error("No OpenOCD port forward ports configured");
        return {};
    }

    std::sort(ports.begin(), ports.end());
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());

    return ports;
}

} // namespace openocd_port_forward
