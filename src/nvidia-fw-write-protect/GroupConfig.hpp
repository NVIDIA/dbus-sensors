#pragma once

#include "ReactiveGraph.hpp"

#include <sdbusplus/async.hpp>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace nvidia::write_protect
{

/**
 * @brief Parsed representation of a write-protect Entity Manager config.
 *
 * Common to all three config types: WriteProtectGroup,
 * HardwareWriteProtectedControl, and WriteProtectDomain.
 */
struct GroupConfig
{
    using PropertiesVariant =
        std::variant<std::string, std::vector<std::string>, bool>;

    std::string name;
    std::vector<std::string> sources;
    std::vector<std::string> flashProtectedComponents;
    BoolOp sourceMode = BoolOp::Or;

    /**
     * @brief Read and parse a write-protect config from Entity Manager.
     * @param ctx       Async context.
     * @param service   Entity Manager D-Bus service name.
     * @param path      D-Bus object path of the config.
     * @param interface D-Bus interface to read properties from.
     * @return Parsed config, or @c std::nullopt on failure.
     */
    static auto tryFrom(sdbusplus::async::context& ctx,
                        std::string_view service, std::string_view path,
                        std::string_view interface)
        -> sdbusplus::async::task<std::optional<GroupConfig>>;
};

} // namespace nvidia::write_protect
