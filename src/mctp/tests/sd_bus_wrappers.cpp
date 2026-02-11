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
 *
 * Usage in tests:
 *   1. Create a pipe: pipe(fds);
 *   2. gFakeSdBusFd = fds[0];
 *   3. auto conn = std::make_shared<sdbusplus::asio::connection>(io, nullptr);
 *   4. Use conn with the code under test.
 *   5. After test, close fds[0] and fds[1] manually (connection::~connection
 *      calls socket.release() which does NOT close the fd).
 */

#include <sys/socket.h>
#include <sys/types.h>
#include <systemd/sd-bus.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Set this to a valid file descriptor before constructing a fake connection.
// The connection constructor calls sd_bus_get_fd; our wrap returns this value.
int gFakeSdBusFd =
    -1; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

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
                       sd_bus_message** /*reply*/)
{
    return -ENOTSUP;
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

bool gMockSymlink = false;
int gSymlinkRetval = 0;

bool gMockIfNametoindex = false;
unsigned gIfNametoindexRetval = 1;

bool gMockSendto = false;
ssize_t gSendtoRetval = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

extern "C"
{
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
extern int __real_socket(int, int, int);
int __wrap_socket(int domain, int type, int protocol)
{
    constexpr int afMctpVal = 46;
    if (domain == afMctpVal && gMockMctpSocket)
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
        return 0;
    }
    return __real_setsockopt(fd, level, optname, optval, optlen);
}

extern int __real_bind(int, const struct sockaddr*, socklen_t);
int __wrap_bind(int fd, const struct sockaddr* addr, socklen_t len)
{
    if ((gMockMctpSocket && fd == gMockMctpSocketFd) || gMockBind)
    {
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
    ++gSystemCallCount;
    (void)cmd;
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
    return (gSendtoRetval >= 0) ? static_cast<ssize_t>(len) : gSendtoRetval;
}
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
} // extern "C"
