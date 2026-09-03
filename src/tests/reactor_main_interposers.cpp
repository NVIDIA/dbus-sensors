// Test-only interposition for MCTPReactorMain's default D-Bus connection.
//
// The production main constructs sdbusplus::asio::connection with the default
// SdBusImpl.  Calls made by that implementation originate in libsdbusplus.so,
// so the executable's usual ld --wrap stubs do not see them.  Strong symbols
// in the test executable do interpose those calls.  When the main harness is
// disabled, each function forwards to the next shared-library definition.

#include "async_test_helpers.hpp"

#include <dlfcn.h>
#include <sys/utsname.h>
#include <systemd/sd-bus-protocol.h>
#include <systemd/sd-bus-vtable.h>
#include <systemd/sd-bus.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <utility>

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
bool gMockReactorMainUname = false;
int gReactorMainUnameResult = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

namespace
{
template <typename Function>
Function nextSymbol(const char* name)
{
    static_assert(std::is_pointer_v<Function>);
    void* symbol = dlsym(RTLD_NEXT, name);
    static_assert(sizeof(Function) == sizeof(symbol));
    return std::bit_cast<Function>(symbol);
}

template <typename Function, typename... Args>
auto callNext(const char* name, Args&&... args)
    -> std::invoke_result_t<Function, Args...>
{
    Function function = nextSymbol<Function>(name);
    if (function == nullptr)
    {
        if constexpr (std::is_pointer_v<
                          std::invoke_result_t<Function, Args...>>)
        {
            return nullptr;
        }
        else
        {
            return -ENOSYS;
        }
    }
    return function(std::forward<Args>(args)...);
}

void clearSlot(sd_bus_slot** slot)
{
    if (slot != nullptr)
    {
        *slot = nullptr;
    }
}
} // namespace

extern "C"
{
int sd_bus_default(sd_bus** ret)
{
    if (!gMockSdBusDefault)
    {
        using Function = int (*)(sd_bus**);
        return callNext<Function>("sd_bus_default", ret);
    }
    if (ret != nullptr)
    {
        *ret = nullptr;
    }
    return 0;
}

int sd_bus_get_fd(sd_bus* bus)
{
    if (!gMockSdBusDefault)
    {
        using Function = int (*)(sd_bus*);
        return callNext<Function>("sd_bus_get_fd", bus);
    }
    return gFakeSdBusFd;
}

int sd_bus_get_events(sd_bus* bus)
{
    if (!gMockSdBusDefault)
    {
        using Function = int (*)(sd_bus*);
        return callNext<Function>("sd_bus_get_events", bus);
    }
    return 0;
}

// Match libsystemd's public parameter name exactly.
// NOLINTNEXTLINE(readability-identifier-naming)
int sd_bus_get_timeout(sd_bus* bus, uint64_t* ret)
{
    if (!gMockSdBusDefault)
    {
        using Function = int (*)(sd_bus*, uint64_t*);
        return callNext<Function>("sd_bus_get_timeout", bus, ret);
    }
    if (ret != nullptr)
    {
        *ret = UINT64_MAX;
    }
    return 0;
}

int sd_bus_process(sd_bus* bus, sd_bus_message** r)
{
    if (!gMockSdBusDefault)
    {
        using Function = int (*)(sd_bus*, sd_bus_message**);
        return callNext<Function>("sd_bus_process", bus, r);
    }
    if (r != nullptr)
    {
        *r = nullptr;
    }
    return 0;
}

int sd_bus_add_object_manager(sd_bus* bus, sd_bus_slot** slot, const char* path)
{
    if (!gMockSdBusDefault)
    {
        using Function = int (*)(sd_bus*, sd_bus_slot**, const char*);
        return callNext<Function>("sd_bus_add_object_manager", bus, slot, path);
    }
    clearSlot(slot);
    return 0;
}

int sd_bus_add_object_vtable(sd_bus* bus, sd_bus_slot** slot, const char* path,
                             const char* interface, const sd_bus_vtable* vtable,
                             void* userdata)
{
    if (!gMockSdBusDefault)
    {
        using Function = int (*)(sd_bus*, sd_bus_slot**, const char*,
                                 const char*, const sd_bus_vtable*, void*);
        return callNext<Function>("sd_bus_add_object_vtable", bus, slot, path,
                                  interface, vtable, userdata);
    }
    clearSlot(slot);
    return 0;
}

int sd_bus_add_match(sd_bus* bus, sd_bus_slot** slot, const char* match,
                     sd_bus_message_handler_t callback, void* userdata)
{
    if (!gMockSdBusDefault)
    {
        using Function = int (*)(sd_bus*, sd_bus_slot**, const char*,
                                 sd_bus_message_handler_t, void*);
        return callNext<Function>("sd_bus_add_match", bus, slot, match,
                                  callback, userdata);
    }
    clearSlot(slot);
    return 0;
}

int sd_bus_request_name(sd_bus* bus, const char* name, uint64_t flags)
{
    if (!gMockSdBusDefault)
    {
        using Function = int (*)(sd_bus*, const char*, uint64_t);
        return callNext<Function>("sd_bus_request_name", bus, name, flags);
    }
    if (gMockSdBusRequestName)
    {
        return gMockSdBusRequestNameResult;
    }
    return 0;
}

int sd_bus_message_new_method_call(sd_bus* bus, sd_bus_message** ret,
                                   const char* destination, const char* path,
                                   const char* interface, const char* member)
{
    if (!gMockSdBusDefault)
    {
        using Function = int (*)(sd_bus*, sd_bus_message**, const char*,
                                 const char*, const char*, const char*);
        return callNext<Function>("sd_bus_message_new_method_call", bus, ret,
                                  destination, path, interface, member);
    }
    if (ret != nullptr)
    {
        *ret = nullptr;
    }
    return -ENOTSUP;
}

int sd_bus_emit_object_added(sd_bus* bus, const char* path)
{
    if (!gMockSdBusDefault)
    {
        using Function = int (*)(sd_bus*, const char*);
        return callNext<Function>("sd_bus_emit_object_added", bus, path);
    }
    return 0;
}

int sd_bus_emit_object_removed(sd_bus* bus, const char* path)
{
    if (!gMockSdBusDefault)
    {
        using Function = int (*)(sd_bus*, const char*);
        return callNext<Function>("sd_bus_emit_object_removed", bus, path);
    }
    return 0;
}

int sd_bus_emit_properties_changed_strv(sd_bus* bus, const char* path,
                                        const char* interface, char** names)
{
    if (!gMockSdBusDefault)
    {
        using Function = int (*)(sd_bus*, const char*, const char*, char**);
        return callNext<Function>("sd_bus_emit_properties_changed_strv", bus,
                                  path, interface, names);
    }
    return 0;
}

int realUname(struct utsname* buffer) asm("__real_uname");
int wrapUname(struct utsname* buffer) asm("__wrap_uname");

int wrapUname(struct utsname* buffer)
{
    if (!gMockReactorMainUname)
    {
        return realUname(buffer);
    }
    if (buffer != nullptr)
    {
        std::memset(buffer, 0, sizeof(*buffer));
        constexpr std::string_view release = "reactor-main-test";
        std::ranges::copy(release, std::begin(buffer->release));
    }
    return gReactorMainUnameResult;
}

} // extern "C"

std::filesystem::file_status
    realFilesystemStatus(const std::filesystem::path& path) asm(
        "__real__ZNSt10filesystem6statusERKNS_7__cxx114pathE");

std::filesystem::file_status
    wrapFilesystemStatus(const std::filesystem::path& path) asm(
        "__wrap__ZNSt10filesystem6statusERKNS_7__cxx114pathE");

std::filesystem::file_status wrapFilesystemStatus(
    const std::filesystem::path& path)
{
    if (!gMockFilesystemExists)
    {
        return realFilesystemStatus(path);
    }
    return std::filesystem::file_status(
        gFilesystemExistsRetval ? std::filesystem::file_type::regular
                                : std::filesystem::file_type::not_found);
}
