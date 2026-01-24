#pragma once

#include <sdbusplus/async.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace nvidia::write_protect
{

struct GpioInputConfig
{
    using PropertiesVariant =
        std::variant<std::string, std::vector<std::string>, bool, uint64_t>;

    std::string name;
    std::string lineName;
    bool activeLow = false;
    std::vector<std::string> flashProtectedComponents = {};
    std::optional<std::chrono::milliseconds> pollInterval = {};

    static auto tryFrom(sdbusplus::async::context& ctx,
                        std::string_view service, std::string_view path,
                        std::string_view interface)
        -> sdbusplus::async::task<std::optional<GpioInputConfig>>;
};
} // namespace nvidia::write_protect
