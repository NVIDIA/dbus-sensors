// filesystem_wrappers.cpp
//
// Overrides std::filesystem::create_directories for test builds.
// On Linux ELF, a strong (T) symbol definition in an executable object
// overrides a weak (W) symbol pulled in from libstdc++, allowing tests to
// mock filesystem operations without modifying production code.
//
// Note: std::filesystem::exists(const path&) is an inline function in
// libstdc++ (GCC 11+) and cannot be overridden via this technique.  The
// gMockFilesystemExists globals are declared for future use when a
// non-inline override mechanism is available.
//
// The globals controlling behaviour are DEFINED in sd_bus_wrappers.cpp and
// declared in async_test_helpers.hpp.
//
// Usage:
//   gMockCreateDirectories = true;  // enable mock
//   gCreateDirectoriesRetval = true; // all calls succeed
//   gCreateDirectoriesFailOnCall = 2; // only call #2 (0-based) fails
//   // ... call production code ...
//   gMockCreateDirectories = false; // restore default

#include <filesystem>
#include <system_error>

// Globals defined in sd_bus_wrappers.cpp
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
extern bool gMockCreateDirectories;
extern bool gCreateDirectoriesRetval;
extern int gCreateDirectoriesFailOnCall;
extern int gCreateDirectoriesCallCount;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

namespace std::filesystem // NOLINT(cert-dcl58-cpp)
{

// Override create_directories(const path&, error_code&)
// Mangled:
// _ZNSt10filesystem18create_directoriesERKNS_7__cxx114pathERSt10error_code
bool create_directories(const path& /*p*/, error_code& ec)
{
    if (!gMockCreateDirectories)
    {
        // Default fallback when mock is not enabled: fail with ENOENT.
        // In the Docker/test environment, /sys/kernel/config is not mounted,
        // so the real call would also fail.  Returning false here prevents
        // tests from accidentally succeeding on paths that don't exist.
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return false;
    }
    int idx = gCreateDirectoriesCallCount++;
    if (gCreateDirectoriesFailOnCall >= 0 &&
        idx == gCreateDirectoriesFailOnCall)
    {
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    }
    ec.clear();
    return gCreateDirectoriesRetval;
}

} // namespace std::filesystem
