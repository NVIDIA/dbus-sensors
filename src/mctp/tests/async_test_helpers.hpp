#pragma once
// async_test_helpers.hpp
// Declares globals and helpers for intercepting sd_bus_call_async and
// exercising async callback paths in MCTP tests.
//
// All globals are DEFINED in sd_bus_wrappers.cpp.
// All drive helpers are DEFINED in sd_bus_wrappers.cpp.
// Test files include this header to use them.

#include <systemd/sd-bus.h>

#include <sdbusplus/sdbus.hpp>

#include <cerrno>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

// ── PendingAsync ─────────────────────────────────────────────────────────────
// Stores a pending sd_bus_call_async callback for manual delivery in tests.
struct PendingAsync
{
    sd_bus_message_handler_t callback;
    void* userdata;
    sd_bus_message* request; // ref-counted copy of the original request
};

// Forward-declare globals needed by TestSdBusInterface (below).
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
extern int gFakeSdBusFd;
extern bool gMockSdBusDefault;
extern bool gMockSdBusCallSuccess;
extern int gSdBusCallCount;
// Synchronous connection->call() reply crafting (see below). Declared here so
// TestSdBusInterface::sd_bus_call can delegate to it.
extern std::function<int(sd_bus_message*, sd_bus_message**)> gSyncCallHandler;
extern bool gMockSdBusCallAsync;
extern std::vector<PendingAsync> gPendingAsyncCalls;
extern std::function<void()> gSdBusCallAsyncHook;
extern bool gMockSdBusRequestName;
extern int gMockSdBusRequestNameResult;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// TestSdBusInterface: overrides sd_bus methods used by
// sdbusplus::asio::connection via virtual dispatch. Since sdbusplus is a shared
// library, virtual calls from SdBusImpl bypass --wrap intercepts. This class
// provides test-friendly overrides that live in the test binary itself.
class TestSdBusInterface : public sdbusplus::SdBusImpl
{
  public:
    int sd_bus_get_fd(sd_bus* /*bus*/) override
    {
        return gFakeSdBusFd;
    }

    int sd_bus_get_events(sd_bus* /*bus*/) override
    {
        return 0;
    }

    int sd_bus_get_timeout(sd_bus* /*bus*/, uint64_t* t) override
    {
        if (t != nullptr)
        {
            *t = UINT64_MAX;
        }
        return 0;
    }

    int sd_bus_process(sd_bus* /*bus*/, sd_bus_message** /*ret*/) override
    {
        return 0;
    }

    int sd_bus_add_match(sd_bus* /*bus*/, sd_bus_slot** slot,
                         const char* /*path*/, sd_bus_message_handler_t /*cb*/,
                         void* /*ud*/) override
    {
        if (slot != nullptr)
        {
            *slot = nullptr;
        }
        return 0;
    }

    int sd_bus_add_object_manager(sd_bus* /*bus*/, sd_bus_slot** slot,
                                  const char* /*path*/) override
    {
        if (slot != nullptr)
        {
            *slot = nullptr;
        }
        return 0;
    }

    int sd_bus_add_object_vtable(
        sd_bus* /*bus*/, sd_bus_slot** slot, const char* /*path*/,
        const char* /*interface*/, const sd_bus_vtable* /*vtable*/,
        void* /*userdata*/) override
    {
        if (slot != nullptr)
        {
            *slot = nullptr;
        }
        return 0;
    }

    int sd_bus_emit_interfaces_added_strv(sd_bus* /*bus*/, const char* /*path*/,
                                          char** /*interfaces*/) override
    {
        return 0;
    }

    int sd_bus_emit_interfaces_removed_strv(
        sd_bus* /*bus*/, const char* /*path*/, char** /*interfaces*/) override
    {
        return 0;
    }

    int sd_bus_emit_object_added(sd_bus* /*bus*/, const char* /*path*/) override
    {
        return 0;
    }

    int sd_bus_emit_object_removed(sd_bus* /*bus*/,
                                   const char* /*path*/) override
    {
        return 0;
    }

    int sd_bus_emit_properties_changed_strv(
        sd_bus* /*bus*/, const char* /*path*/, const char* /*interface*/,
        const char** /*names*/) override
    {
        return 0;
    }

    int sd_bus_message_new_method_call(
        sd_bus* bus, sd_bus_message** m, const char* dest, const char* path,
        const char* iface, const char* member) override
    {
        // When bus is nullptr (test mode), create a semi-initialized fake bus
        // to satisfy sd_bus_message_new_method_call's bus-state requirement.
        // We call SdBusImpl's implementation which is in libsdbusplus.so and
        // calls the real sd_bus_message_new_method_call bypassing --wrap.
        sd_bus* effectiveBus = bus;
        sd_bus* fakeBus = nullptr;
        if (bus == nullptr)
        {
            (void)sd_bus_new(&fakeBus);
            (void)sd_bus_set_address(fakeBus,
                                     "unix:abstract=dbus-sensors-test-fake");
            (void)sd_bus_start(fakeBus); // fails ECONNREFUSED, advances state
            effectiveBus = fakeBus;
        }
        int r = SdBusImpl::sd_bus_message_new_method_call(effectiveBus, m, dest,
                                                          path, iface, member);
        if (fakeBus != nullptr)
        {
            sd_bus_unref(fakeBus);
        }
        return r;
    }

    // Override sd_bus_call so synchronous D-Bus calls (e.g. setRoleEndpoint)
    // succeed without a real bus when gMockSdBusCallSuccess=true.
    // This intercepts the virtual-dispatch path from SdBusImpl
    // (libsdbusplus.so) which the linker --wrap cannot reach.
    int sd_bus_call(sd_bus* /*bus*/, sd_bus_message* m, uint64_t /*usec*/,
                    sd_bus_error* /*ret_error*/,
                    sd_bus_message** reply) override
    {
        ++gSdBusCallCount;
        if (gSyncCallHandler)
        {
            return gSyncCallHandler(m, reply);
        }
        if (gMockSdBusCallSuccess)
        {
            if (reply != nullptr)
            {
                *reply = nullptr;
            }
            return 0;
        }
        return -ENOTSUP;
    }

    int sd_bus_call_async(sd_bus* bus, sd_bus_slot** slot,
                          sd_bus_message* message,
                          sd_bus_message_handler_t callback, void* userdata,
                          uint64_t usec) override
    {
        if (!gMockSdBusCallAsync)
        {
            return SdBusImpl::sd_bus_call_async(bus, slot, message, callback,
                                                userdata, usec);
        }
        if (slot != nullptr)
        {
            *slot = nullptr;
        }
        if (gSdBusCallAsyncHook)
        {
            auto hook = std::move(gSdBusCallAsyncHook);
            gSdBusCallAsyncHook = {};
            hook();
        }
        static uint64_t callSerial = 5000;
        (void)sd_bus_message_seal(message, callSerial++, 0);
        sd_bus_message_ref(message);
        gPendingAsyncCalls.push_back({callback, userdata, message});
        return 0;
    }

    int sd_bus_request_name(sd_bus* bus, const char* name,
                            uint64_t flags) override
    {
        if (gMockSdBusRequestName)
        {
            return gMockSdBusRequestNameResult;
        }
        return SdBusImpl::sd_bus_request_name(bus, name, flags);
    }
};

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)

// ── sd_bus_call_async intercept globals ──────────────────────────────────────
// Set gMockSdBusCallAsync=true AND gMockSdBusCallSuccess=true before calling
// code that uses connection->async_method_call(...). Callbacks are then stored
// in gPendingAsyncCalls instead of being registered with the real bus. The
// controlling globals are declared above for use by TestSdBusInterface.

// ── std::filesystem intercept globals ────────────────────────────────────────
extern bool gMockCreateDirectories;
extern bool gCreateDirectoriesRetval;
extern int gCreateDirectoriesFailOnCall;
extern int gCreateDirectoriesCallCount;

extern bool gMockFilesystemExists;
extern bool gFilesystemExistsRetval;

// ── writeSysfsFile intercept globals ─────────────────────────────────────────
extern bool gMockWriteSysfsFile;
extern bool gWriteSysfsFileRetval;
extern int gWriteSysfsFileFailOnCall;
extern int gWriteSysfsFileCallCount;

// TestSdBusInterface instance (defined in sd_bus_wrappers.cpp).
// Inject into sdbusplus::asio::connection to route virtual sd_bus dispatch
// through test-local overrides instead of the shared-library SdBusImpl.
// NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
extern TestSdBusInterface gTestSdBusInterface;

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// ── Async drive helpers
// ───────────────────────────────────────────────────────
// driveAsyncCallSuccess: fire the oldest pending async call with an empty
// method-return reply (no body). Use for void-return callbacks that only
// inspect the error_code (e.g., EndpointPing, Recover).
void driveAsyncCallSuccess();

// driveAsyncCallAssignEndpoint: fire with an
// AssignEndpoint/AssignEndpointStatic return signature "yisb" (eid, network,
// objpath, allocated).
void driveAsyncCallAssignEndpoint(uint8_t eid, int32_t network,
                                  const char* objpath, bool allocated);

// driveAsyncCallError: fire with a D-Bus method error reply (sets ec != 0).
void driveAsyncCallError();

// Complete and release every currently pending async call with an error.
// Intended for fixture teardown after an assertion aborts a test early.
void drainPendingAsyncCalls();

// driveAsyncCallUnknownInterface: fire with an UnknownInterface D-Bus method
// error reply. Use when a Properties.GetAll probe should model an explicitly
// absent interface.
void driveAsyncCallUnknownInterface();

// driveAsyncCallErrorTimedOut: fire the oldest pending async call with an
// ETIMEDOUT error reply. Exercises the timed_out branch in performHealthCheck
// (MCTPEndpoint.cpp ~line 462 and ~line 552).
void driveAsyncCallErrorTimedOut();

// driveAsyncCallStringVariant: fire with a Properties.Get success reply whose
// body is "v"->"s" (variant containing the given string).  Use to exercise
// async callbacks that expect std::variant<std::string>, e.g. subscribe().
void driveAsyncCallStringVariant(const char* value);

// driveAsyncCallManagedObjectsEmpty: fire with a successful GetManagedObjects
// reply containing an empty a{oa{sa{sv}}} array (ec==0, empty objects map).
// Exercises the "networkIt == end" branch in subscribe() callback.
void driveAsyncCallManagedObjectsEmpty();

// driveAsyncCallSubTreeEmpty: fire with a successful ObjectMapper GetSubTree
// reply containing an empty a{sa{sas}} array.
void driveAsyncCallSubTreeEmpty();

// driveAsyncCallManagedObjectsWithLocalEIDs: fire with a GetManagedObjects
// reply containing the MCTP network1 path, its network interface, and a
// "LocalEIDs" property set to the given byte vector.  Exercises the full
// inner-path branches in subscribe() (MCTPCustomDevices.cpp lines ~430-451).
void driveAsyncCallManagedObjectsWithLocalEIDs(
    const std::vector<uint8_t>& eids);
