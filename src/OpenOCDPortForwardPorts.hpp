#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace openocd_port_forward
{

std::string trim(std::string value);

std::vector<uint16_t> parsePorts(std::string_view portList);

} // namespace openocd_port_forward
