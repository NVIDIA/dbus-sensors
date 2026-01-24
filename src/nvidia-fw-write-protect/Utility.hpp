#pragma once
#include <sdbusplus/async.hpp>

namespace detail
{

template <typename>
struct ComposedTypes;

template <typename Instance, template <typename, typename> typename... Types>
struct ComposedTypes<sdbusplus::async::server::server<Instance, Types...>>
{
    static void emitAdded(auto& server)
    {
        (static_cast<Types<
             Instance, sdbusplus::async::server::server<Instance, Types...>>&>(
             server)
             .emit_added(),
         ...);
    }
};
} // namespace detail

/**
 * @brief Emit InterfacesAdded signals for all composed D-Bus interfaces on a
 *        server object.
 * @tparam T A sdbusplus composed server type.
 * @param server The server instance to emit signals for.
 */
template <typename T>
void emitAdded(T& server)
{
    detail::ComposedTypes<typename T::Self>::emitAdded(server);
}

/**
 * @brief Build a D-Bus software inventory object path from a name.
 * @param name Software inventory identifier.
 * @return The object path "/xyz/openbmc_project/software/<name>".
 */
inline sdbusplus::message::object_path softwarePath(std::string_view name)
{
    return sdbusplus::message::object_path("/xyz/openbmc_project/software") /
           name;
}
