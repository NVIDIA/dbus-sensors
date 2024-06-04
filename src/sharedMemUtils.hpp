#pragma once

#include <sdbusplus/asio/object_server.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

using DbusVariantType = std::variant<
    std::vector<std::tuple<std::string, std::string, std::string>>,
    std::vector<std::string>, std::vector<double>, std::string, int64_t,
    uint64_t, double, int32_t, uint32_t, int16_t, uint16_t, uint8_t, bool,
    sdbusplus::message::unix_fd, std::vector<uint32_t>, std::vector<uint16_t>,
    sdbusplus::message::object_path,
    std::tuple<uint64_t, std::vector<std::tuple<std::string, std::string,
                                                double, uint64_t>>>,
    std::vector<std::tuple<std::string, std::string>>,
    std::vector<std::tuple<uint32_t, std::vector<uint32_t>>>,
    std::vector<std::tuple<uint32_t, size_t>>,
    std::vector<std::tuple<sdbusplus::message::object_path, std::string,
                           std::string, std::string>>,
    std::vector<sdbusplus::message::object_path>, std::vector<uint8_t>,
    std::vector<std::tuple<uint8_t, std::string>>, std::tuple<size_t, bool>,
    std::tuple<bool, uint32_t>, std::map<std::string, uint64_t>,
    std::tuple<std::string, std::string, std::string, uint64_t>>;
