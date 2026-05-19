/* sd_bus_wrappers.cpp
 *
 * ld --wrap implementations that intercept sd-bus calls so tests can
 * create fake sdbusplus::asio::connection objects and exercise code paths
 * that normally require a real D-Bus connection.
 *
 * All wrappers are designed to be "safe no-ops":
 *  - sd_bus_get_fd         → return a pre-set pipe fd (allows connection ctor)
 *  - sd_bus_get_events     → return 0 (no I/O waiting)
 *  - sd_bus_get_timeout    → UINT64_MAX (no timer)
 *  - sd_bus_add_match      → success, null slot (match_t ctor won't throw)
 *  - sd_bus_call           → -ENOTSUP (throws SdBusError → caught by callers)
 *  - sd_bus_process        → 0 (no message processed)
 *  - sd_bus_add_object_manager → success, null slot
 *  - sd_bus_default        → controlled by gMockSdBusDefault; when true, sets
 *                            *bus = nullptr and returns 0 (fake connection)
 *  - sd_bus_request_name   → controlled by gMockSdBusRequestName; when true,
 *                            returns -ENOTSUP (simulates name-claim failure)
 *
 * Usage in tests:
 *   1. Create a pipe: pipe(fds);
 *   2. gFakeSdBusFd = fds[0];
 *   3. auto conn = std::make_shared<sdbusplus::asio::connection>(io, nullptr);
 *   4. Use conn with the code under test.
 *   5. After test, close fds[0] and fds[1] manually (connection::~connection
 *      calls socket.release() which does NOT close the fd).
 */

#include "async_test_helpers.hpp"

#include <sys/socket.h>
#include <systemd/sd-bus-protocol.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Set this to a valid file descriptor before constructing a fake connection.
// The connection constructor calls sd_bus_get_fd; our wrap returns this value.
int gFakeSdBusFd =
    -1; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// When gMockSdBusDefault=true, __wrap_sd_bus_default sets *bus = nullptr and
// returns 0 instead of opening a real D-Bus connection.  Pair with a valid
// gFakeSdBusFd so that the subsequent sdbusplus::asio::connection ctor can
// call sd_bus_get_fd without crashing.
bool gMockSdBusDefault =
    false; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// When gMockSdBusCallSuccess=true, __wrap_sd_bus_message_new_method_call
// creates a real sd_bus_message using a fresh unconnected bus, and
// __wrap_sd_bus_call returns 0 (success) with a null reply.  Together these
// let setRoleEndpoint() reach its "return true" line without a real daemon.
bool gMockSdBusCallSuccess =
    false; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

int gSdBusCallCount =
    0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// When gMockSdBusRequestName=true, __wrap_sd_bus_request_name returns
// -ENOTSUP to simulate a name-claim failure (causes request_name() to throw).
bool gMockSdBusRequestName =
    false; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
TestSdBusInterface gTestSdBusInterface;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

extern "C"
{
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
// Declare the "real" functions that the linker generates when --wrap is used.
extern sd_bus_slot* __real_sd_bus_slot_unref(sd_bus_slot* slot);

int __wrap_sd_bus_get_fd(sd_bus* /*bus*/)
{
    return gFakeSdBusFd;
}

int __wrap_sd_bus_get_events(sd_bus* /*bus*/)
{
    return 0;
}

int __wrap_sd_bus_get_timeout(sd_bus* /*bus*/, uint64_t* timeoutUsec)
{
    if (timeoutUsec != nullptr)
    {
        *timeoutUsec = UINT64_MAX;
    }
    return 0;
}

int __wrap_sd_bus_add_match(
    sd_bus* /*bus*/, sd_bus_slot** slot, const char* /*match*/,
    sd_bus_message_handler_t /*callback*/, void* /*userdata*/)
{
    if (slot != nullptr)
    {
        *slot = nullptr;
    }
    return 0;
}

int __wrap_sd_bus_call(sd_bus* /*bus*/, sd_bus_message* /*message*/,
                       uint64_t /*usec*/, sd_bus_error* /*error*/,
                       sd_bus_message** reply)
{
    ++gSdBusCallCount;
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

extern int __real_sd_bus_message_new_method_call(
    sd_bus* bus, sd_bus_message** m, const char* destination, const char* path,
    const char* interface, const char* member);

// When gMockSdBusCallSuccess=true, create the message object using a fresh
// unconnected sd_bus instead of the (null) connection bus so that
// sdbusplus can build the method-call message without a real daemon.
int __wrap_sd_bus_message_new_method_call(
    sd_bus* bus, sd_bus_message** m, const char* destination, const char* path,
    const char* interface, const char* member)
{
    if (!gMockSdBusCallSuccess)
    {
        return __real_sd_bus_message_new_method_call(bus, m, destination, path,
                                                     interface, member);
    }
    // Create the message using a semi-initialized bus.  sd_bus_new() creates a
    // bus in BUS_UNSET state; in systemd >= 255, sd_bus_message_new_method_call
    // requires the bus to be past BUS_UNSET.  Calling sd_bus_set_address +
    // sd_bus_start advances the state (start fails with -ECONNREFUSED but the
    // bus is now in BUS_CONNECTING/BUS_AUTHENTICATING which is sufficient for
    // message allocation).
    sd_bus* fakeBus = nullptr;
    (void)sd_bus_new(&fakeBus);
    (void)sd_bus_set_address(fakeBus, "unix:abstract=dbus-sensors-test-fake");
    (void)sd_bus_start(fakeBus); // fails ECONNREFUSED, but advances bus state
    int r = __real_sd_bus_message_new_method_call(fakeBus, m, destination, path,
                                                  interface, member);
    sd_bus_unref(fakeBus);
    return r;
}

int __wrap_sd_bus_process(sd_bus* /*bus*/, sd_bus_message** /*ret*/)
{
    return 0;
}

int __wrap_sd_bus_add_object_manager(sd_bus* /*bus*/, sd_bus_slot** slot,
                                     const char* /*path*/)
{
    if (slot != nullptr)
    {
        *slot = nullptr;
    }
    return 0;
}

// sd_bus_slot_unref(nullptr) is safe in libsystemd, but wrap it explicitly
// to avoid any edge cases when fake match_t objects with null slots are freed.
sd_bus_slot* __wrap_sd_bus_slot_unref(sd_bus_slot* slot)
{
    if (slot == nullptr)
    {
        return nullptr;
    }
    return __real_sd_bus_slot_unref(slot);
}

// When gMockSdBusDefault is true, succeed with a null bus pointer so that
// sdbusplus::asio::connection(io) can be constructed without a real D-Bus
// daemon.  The sd_bus_get_fd wrapper returns gFakeSdBusFd, so the connection's
// internal posix::stream_descriptor gets a real file descriptor.
extern int __real_sd_bus_default(sd_bus** bus);
int __wrap_sd_bus_default(sd_bus** bus)
{
    if (!gMockSdBusDefault)
    {
        return __real_sd_bus_default(bus);
    }
    if (bus != nullptr)
    {
        *bus = nullptr;
    }
    return 0;
}

// When gMockSdBusRequestName is true, return -ENOTSUP so that
// sdbusplus::bus::request_name() throws SdBusError.  This allows tests to
// call disabled_main_reactor() and verify that the function body executes up
// to (but not including) io.run(), without blocking indefinitely.
extern int __real_sd_bus_request_name(sd_bus* bus, const char* name,
                                      uint64_t flags);
int __wrap_sd_bus_request_name(sd_bus* bus, const char* name, uint64_t flags)
{
    if (!gMockSdBusRequestName)
    {
        return __real_sd_bus_request_name(bus, name, flags);
    }
    (void)bus;
    (void)name;
    (void)flags;
    return -ENOTSUP;
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
} // extern "C"

// ── Socket wrapping infrastructure ──────────────────────────────────────────
// Set gMockMctpSocket = true in a test fixture to make socket(AF_MCTP,...)
// return gMockMctpSocketFd instead of calling the real socket().

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
bool gMockMctpSocket = false;
int gMockMctpSocketFd = -1;

bool gMockSetsockopt = false;
bool gMockBind = false;

int gSystemRetval = 0;
int gSystemCallCount = 0;
bool gMockSystem = false;
// gSystemFailOnCall: if >= 0, fail (return 1) only on that call index and
// succeed (return gSystemRetval) on all others.  If -1, return gSystemRetval
// for every call.  Reset to -1 after use.
int gSystemFailOnCall = -1;
// gSystemFailErrno: when >= 0, __wrap_system sets errno to this value on the
// failing call (gSystemFailOnCall).  0 means "do not set errno".  Lets tests
// exercise branches that inspect errno after a system() failure (e.g., the
// EEXIST check in MCTPCustomDevices.cpp::setup()).
int gSystemFailErrno = 0;

bool gMockSymlink = false;
int gSymlinkRetval = 0;

bool gMockIfNametoindex = false;
unsigned gIfNametoindexRetval = 1;

bool gMockSendto = false;
ssize_t gSendtoRetval = 0;
// When gSendtoExact=true, __wrap_sendto returns gSendtoRetval directly
// (instead of the normal "return len on success" behaviour).  Set this to
// exercise the "sendto returned fewer bytes than requested" error branch.
bool gSendtoExact = false;

// gSendtoCallCount: incremented on every intercepted sendto() call.
// gSendtoFailOnCall: if >= 0, fail (errno=EINVAL, return -1) only on that
// call index and succeed (return len) on all others.  If -1, use the
// normal gSendtoRetval / gSendtoExact logic.
int gSendtoCallCount = 0;
int gSendtoFailOnCall = -1;

// When gBindFail=true, __wrap_bind returns -1 (errno=EACCES) for mocked fds.
bool gBindFail = false;

// When gSetsockoptFail=true, __wrap_setsockopt returns -1 for mocked fds.
// gSetsockoptFailOnCall: if >= 0, fail only on that call index; if -1, always
// fail.  gSetsockoptCallCount is incremented on every intercepted call.
bool gSetsockoptFail = false;
int gSetsockoptFailOnCall = -1;
int gSetsockoptCallCount = 0;

// When gMockRecvfromSmctpType=true, __wrap_recvfrom overwrites the smctp_type
// field (offset 9 in struct sockaddr_mctp) with gMockRecvfromSmctpTypeVal after
// the real recvfrom call.  This lets tests simulate a response whose MCTP type
// matches the request type without needing a real MCTP kernel stack.
bool gMockRecvfromSmctpType = false;
uint8_t gMockRecvfromSmctpTypeVal = 0;

// gRecvfromCallCount: incremented on every intercepted recvfrom() call.
// gRecvfromFailOnCall: if >= 0, fail (errno=EIO, return -1) only on that call
// index.  If -1 (default), normal passthrough behaviour applies.
int gRecvfromCallCount = 0;
int gRecvfromFailOnCall = -1;
// gRecvfromShortOnCall: if >= 0, the recvfrom call at that index returns
// gRecvfromShortRetval instead of the actual byte count.  This simulates a
// kernel-level short read so that the size-mismatch branch in readMessage()
// can be exercised.  Both are -1 / 0 by default (disabled).
int gRecvfromShortOnCall = -1;
ssize_t gRecvfromShortRetval = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// ── sd_bus_call_async intercept globals ──────────────────────────────────────
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
bool gMockSdBusCallAsync = false;
static int gAsyncReplySerial =
    1000; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::vector<PendingAsync> gPendingAsyncCalls;
std::function<void()> gSdBusCallAsyncHook;

// ── std::filesystem intercept globals ────────────────────────────────────────
bool gMockCreateDirectories = false;
bool gCreateDirectoriesRetval = true;
int gCreateDirectoriesFailOnCall = -1;
int gCreateDirectoriesCallCount = 0;

bool gMockFilesystemExists = false;
bool gFilesystemExistsRetval = false;

// ── writeSysfsFile intercept globals ─────────────────────────────────────────
bool gMockWriteSysfsFile = false;
bool gWriteSysfsFileRetval = true;
int gWriteSysfsFileFailOnCall = -1;
int gWriteSysfsFileCallCount = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

extern "C"
{
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
extern int __real_socket(int, int, int);
int __wrap_socket(int domain, int type, int protocol)
{
    // Use AF_MCTP from <sys/socket.h>; the old hardcoded value of 46 was wrong
    // on systems where AF_MCTP=45 (glibc >= 2.34 / Linux headers >= 5.15).
    if (domain == AF_MCTP && gMockMctpSocket)
    {
        return gMockMctpSocketFd;
    }
    return __real_socket(domain, type, protocol);
}

extern int __real_setsockopt(int, int, int, const void*, socklen_t);
int __wrap_setsockopt(int fd, int level, int optname, const void* optval,
                      socklen_t optlen)
{
    if ((gMockMctpSocket && fd == gMockMctpSocketFd) || gMockSetsockopt)
    {
        int callIdx = gSetsockoptCallCount++;
        if (gSetsockoptFail)
        {
            if (gSetsockoptFailOnCall < 0 || callIdx == gSetsockoptFailOnCall)
            {
                errno = EINVAL;
                return -1;
            }
        }
        return 0;
    }
    return __real_setsockopt(fd, level, optname, optval, optlen);
}

extern int __real_bind(int, const struct sockaddr*, socklen_t);
int __wrap_bind(int fd, const struct sockaddr* addr, socklen_t len)
{
    if ((gMockMctpSocket && fd == gMockMctpSocketFd) || gMockBind)
    {
        if (gBindFail)
        {
            errno = EACCES;
            return -1;
        }
        return 0;
    }
    return __real_bind(fd, addr, len);
}

extern int __real_system(const char*);
int __wrap_system(const char* cmd)
{
    if (!gMockSystem)
    {
        return __real_system(cmd);
    }
    int idx = gSystemCallCount++;
    (void)cmd;
    if (gSystemFailOnCall >= 0 && idx == gSystemFailOnCall)
    {
        if (gSystemFailErrno != 0)
        {
            errno = gSystemFailErrno;
        }
        return 1;
    }
    return gSystemRetval;
}

extern int __real_symlink(const char*, const char*);
int __wrap_symlink(const char* target, const char* linkpath)
{
    if (!gMockSymlink)
    {
        return __real_symlink(target, linkpath);
    }
    (void)target;
    (void)linkpath;
    return gSymlinkRetval;
}

extern unsigned __real_if_nametoindex(const char*);
unsigned __wrap_if_nametoindex(const char* ifname)
{
    if (!gMockIfNametoindex)
    {
        return __real_if_nametoindex(ifname);
    }
    (void)ifname;
    return gIfNametoindexRetval;
}

extern ssize_t __real_sendto(int, const void*, size_t, int,
                             const struct sockaddr*, socklen_t);
ssize_t __wrap_sendto(int fd, const void* buf, size_t len, int flags,
                      const struct sockaddr* dest, socklen_t alen)
{
    if (!gMockSendto)
    {
        return __real_sendto(fd, buf, len, flags, dest, alen);
    }
    (void)fd;
    (void)buf;
    (void)flags;
    (void)dest;
    (void)alen;
    int callIdx = gSendtoCallCount++;
    if (gSendtoFailOnCall >= 0)
    {
        if (callIdx == gSendtoFailOnCall)
        {
            errno = EINVAL;
            return -1;
        }
        return static_cast<ssize_t>(len);
    }
    if (gSendtoExact)
    {
        return gSendtoRetval;
    }
    return (gSendtoRetval >= 0) ? static_cast<ssize_t>(len) : gSendtoRetval;
}

// When gMockRecvfromSmctpType is true, overwrite the smctp_type field at byte
// offset 9 in the address buffer after the real recvfrom call.  This allows
// tests to exercise the type-match branch in mctpQueryVdmCommand without a real
// MCTP kernel stack.  struct sockaddr_mctp layout: sa_family(2) + pad(2) +
// network(4) + addr(1) + type(1) → smctp_type is at byte offset 9.
//
// When gRecvfromFailOnCall >= 0, the call at that index (0-based,
// gRecvfromCallCount tracks the index) returns -1/EIO regardless of the real
// socket state.  This allows tests to inject a failure on the second recvfrom
// call (the actual read after the MSG_PEEK peek) inside readMessage().
extern ssize_t __real_recvfrom(int, void*, size_t, int, struct sockaddr*,
                               socklen_t*);
ssize_t __wrap_recvfrom(int fd, void* buf, size_t len, int flags,
                        struct sockaddr* src_addr, socklen_t* addrlen)
{
    int callIdx = gRecvfromCallCount++;
    if (gRecvfromFailOnCall >= 0 && callIdx == gRecvfromFailOnCall)
    {
        errno = EIO;
        return -1;
    }
    ssize_t rc = __real_recvfrom(fd, buf, len, flags, src_addr, addrlen);
    // If this call index should return a short count, consume the datagram
    // from the socket (the real recvfrom above already did that) but return
    // the injected short length so the caller sees a size mismatch.
    if (gRecvfromShortOnCall >= 0 && callIdx == gRecvfromShortOnCall)
    {
        return gRecvfromShortRetval;
    }
    constexpr size_t kSmctpTypeOffset = 9;
    if (gMockRecvfromSmctpType && src_addr != nullptr)
    {
        // Safe: callers always pass a buffer of at least sizeof(sockaddr_mctp)
        // = 12 bytes; kSmctpTypeOffset = 9 < 12.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        reinterpret_cast<uint8_t*>(src_addr)[kSmctpTypeOffset] =
            gMockRecvfromSmctpTypeVal;
    }
    return rc;
}
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)

// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
extern int __real_sd_bus_call_async(
    sd_bus* bus, sd_bus_slot** slot, sd_bus_message* m,
    sd_bus_message_handler_t callback, void* userdata, uint64_t usec);
int __wrap_sd_bus_call_async(sd_bus* bus, sd_bus_slot** slot, sd_bus_message* m,
                             sd_bus_message_handler_t callback, void* userdata,
                             uint64_t usec)
{
    if (!gMockSdBusCallAsync)
    {
        return __real_sd_bus_call_async(bus, slot, m, callback, userdata, usec);
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
    // Seal the message before storing: sd_bus_message_new_method_return and
    // sd_bus_message_new_method_errorf both require a sealed call message
    // (they return -EPERM otherwise, leaving the reply pointer null).
    // The real sd_bus_call_async seals the message internally before sending;
    // since we intercept before that, we seal it here instead.
    static uint64_t sCallSerial = 100; // NOLINT
    (void)sd_bus_message_seal(m, sCallSerial++, 0);
    sd_bus_message_ref(m);
    gPendingAsyncCalls.push_back({callback, userdata, m});
    return 0;
}
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
} // extern "C"

// ── writeSysfsFile linker-wrap
// ──────────────────────────────────────────────── Uses GCC asm labels to
// assign the __wrap_ and __real_ linker names required by
// `--wrap,_Z14writeSysfsFileRKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES6_`.
// The mangled name was verified from the compiled test binary with nm.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
extern bool writeSysfsFileReal( // NOLINT
    const std::string& path,
    const std::string&
        value) __asm__("__real__Z14writeSysfsFileRKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES6_")
    __attribute__((weak));

bool writeSysfsFileWrap( // NOLINT
    const std::string& path,
    const std::string&
        value) __asm__("__wrap__Z14writeSysfsFileRKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES6_");

bool writeSysfsFileWrap( // NOLINT
    const std::string& path, const std::string& value)
{
    if (!gMockWriteSysfsFile)
    {
        if (writeSysfsFileReal != nullptr)
        {
            return writeSysfsFileReal(path, value);
        }
        return false;
    }
    int idx = gWriteSysfsFileCallCount++;
    if (gWriteSysfsFileFailOnCall >= 0 && idx == gWriteSysfsFileFailOnCall)
    {
        return false;
    }
    return gWriteSysfsFileRetval;
}
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)

// ── Async drive helpers
// ───────────────────────────────────────────────────────

// driveAsyncCallSuccess: fire the oldest pending async call with a plain
// method-return reply (no payload body). Covers void-return async callbacks.
void driveAsyncCallSuccess()
{
    assert(!gPendingAsyncCalls.empty());
    PendingAsync p = gPendingAsyncCalls.front();
    gPendingAsyncCalls.erase(gPendingAsyncCalls.begin());
    sd_bus_message* reply = nullptr;
    (void)sd_bus_message_new_method_return(p.request, &reply);
    (void)sd_bus_message_seal(reply, static_cast<uint64_t>(++gAsyncReplySerial),
                              0);
    (void)p.callback(reply, p.userdata, nullptr);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(p.request);
}

// driveAsyncCallAssignEndpoint: fire with AssignEndpoint/AssignEndpointStatic
// return payload "yisb" (eid, network, objpath, allocated).
void driveAsyncCallAssignEndpoint(uint8_t eid, int32_t network,
                                  const char* objpath, bool allocated)
{
    assert(!gPendingAsyncCalls.empty());
    PendingAsync p = gPendingAsyncCalls.front();
    gPendingAsyncCalls.erase(gPendingAsyncCalls.begin());
    sd_bus_message* reply = nullptr;
    (void)sd_bus_message_new_method_return(p.request, &reply);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    (void)sd_bus_message_append(reply, "yisb", eid, network, objpath,
                                static_cast<int>(allocated));
    (void)sd_bus_message_seal(reply, static_cast<uint64_t>(++gAsyncReplySerial),
                              0);
    (void)p.callback(reply, p.userdata, nullptr);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(p.request);
}

// driveAsyncCallError: fire with a D-Bus method error (sets ec != 0 in
// the async_method_call callback).
void driveAsyncCallError()
{
    assert(!gPendingAsyncCalls.empty());
    PendingAsync p = gPendingAsyncCalls.front();
    gPendingAsyncCalls.erase(gPendingAsyncCalls.begin());
    sd_bus_message* reply = nullptr;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    (void)sd_bus_message_new_method_errorf(
        p.request, &reply, "org.freedesktop.DBus.Error.Failed", "Mock failure");
    (void)sd_bus_message_seal(reply, static_cast<uint64_t>(++gAsyncReplySerial),
                              0);
    (void)p.callback(reply, p.userdata, nullptr);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(p.request);
}

void driveAsyncCallUnknownInterface()
{
    assert(!gPendingAsyncCalls.empty());
    PendingAsync p = gPendingAsyncCalls.front();
    gPendingAsyncCalls.erase(gPendingAsyncCalls.begin());
    sd_bus_message* reply = nullptr;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    (void)sd_bus_message_new_method_errorf(
        p.request, &reply, SD_BUS_ERROR_UNKNOWN_INTERFACE,
        "Mock missing interface");
    (void)sd_bus_message_seal(reply, static_cast<uint64_t>(++gAsyncReplySerial),
                              0);
    (void)p.callback(reply, p.userdata, nullptr);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(p.request);
}

// driveAsyncCallErrorTimedOut: fire the oldest pending async call with an
// ETIMEDOUT error reply. Exercises the `ec == boost::system::errc::timed_out`
// branch in performHealthCheck (MCTPEndpoint.cpp ~line 462 and ~line 552).
void driveAsyncCallErrorTimedOut()
{
    assert(!gPendingAsyncCalls.empty());
    PendingAsync p = gPendingAsyncCalls.front();
    gPendingAsyncCalls.erase(gPendingAsyncCalls.begin());
    sd_bus_message* reply = nullptr;
    // sd_bus_message_new_method_errno takes (sd_bus_message* call, ...) —
    // pass the original request as the call context.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    (void)sd_bus_message_new_method_errno(p.request, &reply, ETIMEDOUT,
                                          nullptr);
    (void)sd_bus_message_seal(reply, static_cast<uint64_t>(++gAsyncReplySerial),
                              0);
    // Swallow exceptions from the callback: in the test environment,
    // logMCTPError() → CommitDeviceError tries to create a thread, which
    // throws because BOOST_ASIO is compiled with DISABLE_THREADS.  The
    // timed_out branch in performHealthCheck IS exercised (and counted by
    // gcovr) before the throw, so coverage is not lost.
    try
    {
        (void)p.callback(reply, p.userdata, nullptr);
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {}
    sd_bus_message_unref(reply);
    sd_bus_message_unref(p.request);
}

// driveAsyncCallStringVariant: fire the oldest pending async call with a
// successful Properties.Get reply whose body is a variant containing a string:
// D-Bus type "v" → inner type "s" → the given value.  Use this to exercise
// async callbacks that expect `std::variant<std::string>` (e.g. the
// MCTPDEndpoint::subscribe() async_method_call at MCTPEndpoint.cpp ~line 821).
void driveAsyncCallStringVariant(const char* value)
{
    assert(!gPendingAsyncCalls.empty());
    PendingAsync p = gPendingAsyncCalls.front();
    gPendingAsyncCalls.erase(gPendingAsyncCalls.begin());
    sd_bus_message* reply = nullptr;
    (void)sd_bus_message_new_method_return(p.request, &reply);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    (void)sd_bus_message_append(reply, "v", "s", value);
    (void)sd_bus_message_seal(reply, static_cast<uint64_t>(++gAsyncReplySerial),
                              0);
    (void)p.callback(reply, p.userdata, nullptr);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(p.request);
}

// driveAsyncCallManagedObjectsEmpty: fire with a successful GetManagedObjects
// reply containing an empty a{oa{sa{sv}}} array. The callback receives
// ec==0 and an empty ManagedObjectType → exercises the "networkIt == end"
// branch in subscribe() (MCTPCustomDevices.cpp ~line 430).
void driveAsyncCallManagedObjectsEmpty()
{
    assert(!gPendingAsyncCalls.empty());
    PendingAsync p = gPendingAsyncCalls.front();
    gPendingAsyncCalls.erase(gPendingAsyncCalls.begin());
    sd_bus_message* reply = nullptr;
    (void)sd_bus_message_new_method_return(p.request, &reply);
    (void)sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY,
                                        "{oa{sa{sv}}}");
    (void)sd_bus_message_close_container(reply);
    (void)sd_bus_message_seal(reply, static_cast<uint64_t>(++gAsyncReplySerial),
                              0);
    (void)p.callback(reply, p.userdata, nullptr);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(p.request);
}

// driveAsyncCallManagedObjectsWithLocalEIDs: fire with a successful
// GetManagedObjects reply containing:
//   {"/au/com/codeconstruct/mctp1/networks/1":
//     {"au.com.codeconstruct.MCTP.Network1":
//       {"LocalEIDs": variant("ay", eids)}}}
// Exercises the full inner-path branches in subscribe() callback
// (MCTPCustomDevices.cpp lines ~430-451): networkIt != end, ifaceIt != end,
// propIt != end, eids != null, and the EID loop body.
void driveAsyncCallManagedObjectsWithLocalEIDs(const std::vector<uint8_t>& eids)
{
    static constexpr const char* kNetworkPath =
        "/au/com/codeconstruct/mctp1/networks/1";
    static constexpr const char* kNetworkIface =
        "au.com.codeconstruct.MCTP.Network1";

    assert(!gPendingAsyncCalls.empty());
    PendingAsync p = gPendingAsyncCalls.front();
    gPendingAsyncCalls.erase(gPendingAsyncCalls.begin());
    sd_bus_message* reply = nullptr;
    (void)sd_bus_message_new_method_return(p.request, &reply);

    // a{oa{sa{sv}}} — outer objects array
    (void)sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY,
                                        "{oa{sa{sv}}}");
    // {o a{sa{sv}}} — one object entry
    (void)sd_bus_message_open_container(reply, SD_BUS_TYPE_DICT_ENTRY,
                                        "oa{sa{sv}}");
    (void)sd_bus_message_append_basic(reply, SD_BUS_TYPE_OBJECT_PATH,
                                      kNetworkPath);
    // a{sa{sv}} — interface map
    (void)sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "{sa{sv}}");
    // {s a{sv}} — one interface entry
    (void)sd_bus_message_open_container(reply, SD_BUS_TYPE_DICT_ENTRY,
                                        "sa{sv}");
    (void)sd_bus_message_append_basic(reply, SD_BUS_TYPE_STRING, kNetworkIface);
    // a{sv} — properties map
    (void)sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "{sv}");
    // {sv} — one property: "LocalEIDs" = variant("ay", eids)
    (void)sd_bus_message_open_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sv");
    (void)sd_bus_message_append_basic(reply, SD_BUS_TYPE_STRING, "LocalEIDs");
    // v "ay" — variant containing array of bytes
    (void)sd_bus_message_open_container(reply, SD_BUS_TYPE_VARIANT, "ay");
    (void)sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "y");
    for (uint8_t eid : eids)
    {
        (void)sd_bus_message_append_basic(reply, SD_BUS_TYPE_BYTE, &eid);
    }
    (void)sd_bus_message_close_container(reply); // "ay" array
    (void)sd_bus_message_close_container(reply); // variant "ay"
    (void)sd_bus_message_close_container(reply); // dict "{sv}" LocalEIDs
    (void)sd_bus_message_close_container(reply); // array "{sv}" properties
    (void)sd_bus_message_close_container(reply); // dict "{sa{sv}}" interface
    (void)sd_bus_message_close_container(reply); // array "{sa{sv}}" interfaces
    (void)sd_bus_message_close_container(reply); // dict "{oa{sa{sv}}}" object
    (void)sd_bus_message_close_container(reply); // array "{oa{sa{sv}}}" objects

    (void)sd_bus_message_seal(reply, static_cast<uint64_t>(++gAsyncReplySerial),
                              0);
    (void)p.callback(reply, p.userdata, nullptr);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(p.request);
}
