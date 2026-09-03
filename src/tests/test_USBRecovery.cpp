// White-box coverage for USBRecovery: the helpers live in an anonymous
// namespace, so the implementation is included directly (same pattern as
// test_MCTPHeartBeatApp.cpp) to exercise them and the LibusbUSBRecovery
// early-return paths without requiring a real USB device.
#include "../mctp/LibusbSystem.hpp"
#include "USBRecovery.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdarg>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../mctp/USBRecovery.cpp" // NOLINT(bugprone-suspicious-include)

#include <gtest/gtest.h>

// libusb types/macros and LibusbUSBRecovery are provided by the white-box
// USBRecovery.cpp include above, so include-cleaner cannot see a direct
// providing header for them.
// NOLINTBEGIN(misc-include-cleaner)
namespace
{

TEST(USBRecoveryParseUint8, AcceptsInRangeValues)
{
    uint8_t out = 0xFF;
    EXPECT_TRUE(parseUint8("0", out));
    EXPECT_EQ(out, 0);
    EXPECT_TRUE(parseUint8("255", out));
    EXPECT_EQ(out, 255);
    EXPECT_TRUE(parseUint8("42", out));
    EXPECT_EQ(out, 42);
}

TEST(USBRecoveryParseUint8, RejectsOutOfRangeAndMalformed)
{
    uint8_t out = 7;
    EXPECT_FALSE(parseUint8("256", out));
    EXPECT_FALSE(parseUint8("99999", out));
    EXPECT_FALSE(parseUint8("abc", out));
    EXPECT_FALSE(parseUint8("12x", out));
    EXPECT_FALSE(parseUint8("", out));
    EXPECT_FALSE(parseUint8("-1", out));
}

TEST(USBRecoveryParsePortPath, ParsesSingleAndMultiplePorts)
{
    auto single = parsePortPath("3");
    ASSERT_TRUE(single.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE.
    EXPECT_EQ(single.value().size(), 1U);
    EXPECT_EQ(single.value()[0], 3);
    // NOLINTEND(bugprone-unchecked-optional-access)

    auto multi = parsePortPath("1.2.3");
    ASSERT_TRUE(multi.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE.
    ASSERT_EQ(multi.value().size(), 3U);
    EXPECT_EQ(multi.value()[0], 1);
    EXPECT_EQ(multi.value()[1], 2);
    EXPECT_EQ(multi.value()[2], 3);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST(USBRecoveryParsePortPath, RejectsEmptyAndInvalid)
{
    EXPECT_FALSE(parsePortPath("").has_value());
    EXPECT_FALSE(parsePortPath("1..2").has_value());
    EXPECT_FALSE(parsePortPath("1.300").has_value());
    EXPECT_FALSE(parsePortPath("1.bad").has_value());
}

TEST(USBRecoveryReadTrimmedLine, MissingFileReturnsNullopt)
{
    auto missing = readTrimmedLine(
        std::filesystem::path("/nonexistent/usb-recovery-test/does-not-exist"));
    EXPECT_FALSE(missing.has_value());
}

TEST(USBRecoveryReadTrimmedLine, TrimsWhitespaceAndHandlesEmpty)
{
    auto dir = std::filesystem::temp_directory_path();
    auto valuePath = dir / "usb_recovery_value.txt";
    auto emptyPath = dir / "usb_recovery_empty.txt";

    {
        std::ofstream valueStream(valuePath);
        valueStream << "  12 \t\r\n";
    }
    {
        std::ofstream emptyStream(emptyPath);
        emptyStream << "\n";
    }

    auto value = readTrimmedLine(valuePath);
    ASSERT_TRUE(value.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT.
    EXPECT_EQ(value.value(), "12");

    auto empty = readTrimmedLine(emptyPath);
    EXPECT_FALSE(empty.has_value());

    std::filesystem::remove(valuePath);
    std::filesystem::remove(emptyPath);
}

TEST(USBRecoveryBuildLocationString, IncludesDevnumWhenPresent)
{
    USBDeviceLocation location{1, std::vector<uint8_t>{1, 2, 3}, uint8_t{13}};
    auto text = buildLocationString(location);
    EXPECT_NE(text.find("bus 1"), std::string::npos);
    EXPECT_NE(text.find("1.2.3"), std::string::npos);
    EXPECT_NE(text.find("devnum 13"), std::string::npos);
}

TEST(USBRecoveryBuildLocationString, OmitsDevnumWhenAbsent)
{
    USBDeviceLocation location{2, std::vector<uint8_t>{4}, std::nullopt};
    auto text = buildLocationString(location);
    EXPECT_NE(text.find("bus 2"), std::string::npos);
    EXPECT_EQ(text.find("devnum"), std::string::npos);
}

TEST(USBRecoveryBuildLocationString, HandlesEmptyPortPath)
{
    USBDeviceLocation location{3, {}, std::nullopt};
    EXPECT_EQ(buildLocationString(location), "bus 3, ports ");
}

TEST(USBRecoveryBuildUsbfsNodePath, FormatsZeroPaddedPath)
{
    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    auto node = buildUsbfsNodePath(location);
    ASSERT_TRUE(node.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT.
    EXPECT_EQ(node.value(), "/dev/bus/usb/001/013");
}

TEST(USBRecoveryBuildUsbfsNodePath, NulloptWithoutDevnum)
{
    USBDeviceLocation location{1, std::vector<uint8_t>{1}, std::nullopt};
    EXPECT_FALSE(buildUsbfsNodePath(location).has_value());
}

TEST(USBRecoveryFormatLibusbError, ContainsPrefixAndCode)
{
    auto text = formatLibusbError("op failed", LIBUSB_ERROR_OTHER);
    EXPECT_NE(text.find("op failed"), std::string::npos);
    EXPECT_NE(text.find(std::to_string(LIBUSB_ERROR_OTHER)), std::string::npos);
}

TEST(USBRecoveryScopedFd, DefaultIsInvalid)
{
    ScopedFd fd;
    EXPECT_EQ(fd.get(), -1);
}

TEST(USBRecoveryScopedFd, ClosesOwnedDescriptorOnDestruction)
{
    std::array<int, 2> pipeFds{-1, -1};
    ASSERT_EQ(pipe(pipeFds.data()), 0);
    const int writeEnd = pipeFds[1];
    {
        ScopedFd fd(writeEnd);
        EXPECT_EQ(fd.get(), writeEnd);
    }
    // Destructor should have closed writeEnd.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    EXPECT_EQ(fcntl(writeEnd, F_GETFD), -1);
    close(pipeFds[0]);
}

TEST(USBRecoveryScopedFd, MoveTransfersOwnership)
{
    std::array<int, 2> pipeFds{-1, -1};
    ASSERT_EQ(pipe(pipeFds.data()), 0);

    ScopedFd source(pipeFds[1]);
    ScopedFd moved(std::move(source));
    // Intentionally inspect the moved-from object: ScopedFd deterministically
    // resets the source descriptor to -1 on move.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_EQ(source.get(), -1);
    EXPECT_EQ(moved.get(), pipeFds[1]);

    ScopedFd assigned;
    assigned = std::move(moved);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_EQ(moved.get(), -1);
    EXPECT_EQ(assigned.get(), pipeFds[1]);

    close(pipeFds[0]);
}

TEST(USBRecoveryScopedFd, SelfMoveAssignmentPreservesOwnership)
{
    std::array<int, 2> pipeFds{-1, -1};
    ASSERT_EQ(pipe(pipeFds.data()), 0);

    ScopedFd fd(pipeFds[1]);
    ScopedFd* sameObject = &fd;
    fd = std::move(*sameObject);

    EXPECT_EQ(fd.get(), pipeFds[1]);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    EXPECT_NE(fcntl(pipeFds[1], F_GETFD), -1);
    close(pipeFds[0]);
}

TEST(USBRecoveryScopedFd, ResetClosesAndReplaces)
{
    std::array<int, 2> pipeFds{-1, -1};
    ASSERT_EQ(pipe(pipeFds.data()), 0);
    ScopedFd fd(pipeFds[1]);
    fd.reset();
    EXPECT_EQ(fd.get(), -1);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    EXPECT_EQ(fcntl(pipeFds[1], F_GETFD), -1);
    close(pipeFds[0]);
}

TEST(USBRecoveryResolveLocation, UnknownInterfaceReturnsNullopt)
{
    std::string status{};
    auto location = resolveUSBDeviceLocationForInterface(
        "usb-recovery-bogus-interface", status);
    EXPECT_FALSE(location.has_value());
    EXPECT_FALSE(status.empty());
}

// Builds <tmp>/usbrec_<tag>/<iface>/device and returns the net-class root to
// pass as the sysfs override. Caller removes the tree.
std::filesystem::path makeNetDeviceTree(const std::string& tag,
                                        const std::string& iface)
{
    auto root = std::filesystem::temp_directory_path() / ("usbrec_" + tag);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / iface / "device");
    return root;
}

void writeSysfsValue(const std::filesystem::path& path,
                     const std::string& content)
{
    std::ofstream stream(path);
    stream << content;
}

TEST(USBRecoveryResolveLocation, ResolvesBusPortsAndDevnum)
{
    auto root = makeNetDeviceTree("resolve_ok", "usbx");
    auto deviceDir = root / "usbx" / "device";
    writeSysfsValue(deviceDir / "busnum", "1\n");
    writeSysfsValue(deviceDir / "devpath", "1.2.3\n");
    writeSysfsValue(deviceDir / "devnum", "13\n");

    std::string status{};
    auto location = resolveUSBDeviceLocationForInterface("usbx", status, root);
    ASSERT_TRUE(location.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE.
    EXPECT_EQ(location.value().bus, 1);
    ASSERT_EQ(location.value().ports.size(), 3U);
    EXPECT_EQ(location.value().ports[0], 1);
    EXPECT_EQ(location.value().ports[2], 3);
    ASSERT_TRUE(location.value().deviceAddress.has_value());
    EXPECT_EQ(location.value().deviceAddress.value(), 13);
    // NOLINTEND(bugprone-unchecked-optional-access)

    std::filesystem::remove_all(root);
}

TEST(USBRecoveryResolveLocation, ResolvesWithoutDevnum)
{
    auto root = makeNetDeviceTree("resolve_no_devnum", "usbx");
    auto deviceDir = root / "usbx" / "device";
    writeSysfsValue(deviceDir / "busnum", "2\n");
    writeSysfsValue(deviceDir / "devpath", "4\n");

    std::string status{};
    auto location = resolveUSBDeviceLocationForInterface("usbx", status, root);
    ASSERT_TRUE(location.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT.
    EXPECT_FALSE(location.value().deviceAddress.has_value());

    std::filesystem::remove_all(root);
}

TEST(USBRecoveryResolveLocation, InvalidDevnumIsIgnored)
{
    auto root = makeNetDeviceTree("resolve_bad_devnum", "usbx");
    auto deviceDir = root / "usbx" / "device";
    writeSysfsValue(deviceDir / "busnum", "1\n");
    writeSysfsValue(deviceDir / "devpath", "1.2\n");
    writeSysfsValue(deviceDir / "devnum", "not-a-number\n");

    std::string status{};
    auto location = resolveUSBDeviceLocationForInterface("usbx", status, root);
    ASSERT_TRUE(location.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT.
    EXPECT_FALSE(location.value().deviceAddress.has_value());

    std::filesystem::remove_all(root);
}

TEST(USBRecoveryResolveLocation, EmptyBusnumReportsMissing)
{
    auto root = makeNetDeviceTree("resolve_empty_busnum", "usbx");
    auto deviceDir = root / "usbx" / "device";
    writeSysfsValue(deviceDir / "busnum", "\n"); // present but empty
    writeSysfsValue(deviceDir / "devpath", "1.2\n");

    std::string status{};
    auto location = resolveUSBDeviceLocationForInterface("usbx", status, root);
    EXPECT_FALSE(location.has_value());
    EXPECT_NE(status.find("busnum/devpath missing or unreadable"),
              std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(USBRecoveryResolveLocation, InvalidBusnumReportsError)
{
    auto root = makeNetDeviceTree("resolve_bad_busnum", "usbx");
    auto deviceDir = root / "usbx" / "device";
    writeSysfsValue(deviceDir / "busnum", "999\n"); // > 255
    writeSysfsValue(deviceDir / "devpath", "1.2\n");

    std::string status{};
    auto location = resolveUSBDeviceLocationForInterface("usbx", status, root);
    EXPECT_FALSE(location.has_value());
    EXPECT_NE(status.find("invalid busnum content"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(USBRecoveryResolveLocation, InvalidDevpathReportsError)
{
    auto root = makeNetDeviceTree("resolve_bad_devpath", "usbx");
    auto deviceDir = root / "usbx" / "device";
    writeSysfsValue(deviceDir / "busnum", "1\n");
    writeSysfsValue(deviceDir / "devpath", "1.999\n"); // 999 > 255

    std::string status{};
    auto location = resolveUSBDeviceLocationForInterface("usbx", status, root);
    EXPECT_FALSE(location.has_value());
    EXPECT_NE(status.find("invalid devpath content"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(USBRecoveryResolveLocation, MissingDevpathContinuesAncestrySearch)
{
    auto root = makeNetDeviceTree("missing_devpath", "usbx");
    auto deviceDir = root / "usbx" / "device";
    writeSysfsValue(deviceDir / "busnum", "1\n");

    std::string status{};
    auto location = resolveUSBDeviceLocationForInterface("usbx", status, root);
    EXPECT_FALSE(location.has_value());
    EXPECT_NE(status.find("no USB busnum/devpath"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(USBRecoveryResolveLocation, EmptyDevpathReportsMissing)
{
    auto root = makeNetDeviceTree("empty_devpath", "usbx");
    auto deviceDir = root / "usbx" / "device";
    writeSysfsValue(deviceDir / "busnum", "1\n");
    writeSysfsValue(deviceDir / "devpath", "\n");

    std::string status{};
    auto location = resolveUSBDeviceLocationForInterface("usbx", status, root);
    EXPECT_FALSE(location.has_value());
    EXPECT_NE(status.find("busnum/devpath missing or unreadable"),
              std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(USBRecoveryResolveLocation, EmptyDevnumIsIgnored)
{
    auto root = makeNetDeviceTree("empty_devnum", "usbx");
    auto deviceDir = root / "usbx" / "device";
    writeSysfsValue(deviceDir / "busnum", "1\n");
    writeSysfsValue(deviceDir / "devpath", "2.3\n");
    writeSysfsValue(deviceDir / "devnum", "\n");

    std::string status{};
    auto location = resolveUSBDeviceLocationForInterface("usbx", status, root);
    ASSERT_TRUE(location.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT.
    EXPECT_FALSE(location->deviceAddress.has_value());

    std::filesystem::remove_all(root);
}

TEST(USBRecoveryResolveLocation, FindsMetadataOnParent)
{
    auto root = makeNetDeviceTree("parent_metadata", "usbx");
    auto interfaceDir = root / "usbx";
    writeSysfsValue(interfaceDir / "busnum", "2\n");
    writeSysfsValue(interfaceDir / "devpath", "4.5\n");
    writeSysfsValue(interfaceDir / "devnum", "6\n");

    std::string status{};
    auto location = resolveUSBDeviceLocationForInterface("usbx", status, root);
    ASSERT_TRUE(location.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access): guarded by ASSERT.
    EXPECT_EQ(location->bus, 2);
    EXPECT_EQ(location->ports, (std::vector<uint8_t>{4, 5}));
    EXPECT_EQ(location->deviceAddress, uint8_t{6});
    // NOLINTEND(bugprone-unchecked-optional-access)

    std::filesystem::remove_all(root);
}

TEST(USBRecoveryResolveLocation, SymlinkLoopReportsCanonicalizationFailure)
{
    auto root = std::filesystem::temp_directory_path() /
                ("usbrec_symlink_loop_" + std::to_string(getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::filesystem::create_symlink(root / "loop", root / "loop");

    std::string status{};
    auto location = resolveUSBDeviceLocationForInterface("loop", status, root);
    EXPECT_FALSE(location.has_value());
    EXPECT_EQ(status, "failed to resolve sysfs path");

    std::filesystem::remove_all(root);
}

TEST(USBRecoveryClearBulkOutHalt, UnknownInterfaceFailsBeforeLibusb)
{
    LibusbUSBRecovery recovery;
    std::string status{};
    EXPECT_FALSE(
        recovery.clearBulkOutHalt("usb-recovery-bogus-interface", status));
    EXPECT_FALSE(status.empty());
}

TEST(USBRecoveryOpenHandle, NoDevnumFails)
{
    USBDeviceLocation location{1, std::vector<uint8_t>{1}, std::nullopt};
    libusb_device_handle* handle = nullptr;
    ScopedFd wrappedFd;
    std::string status{};
    EXPECT_FALSE(openHandleDirectFromUsbfs(nullptr, location, &handle,
                                           wrappedFd, status));
    EXPECT_NE(status.find("devnum unavailable"), std::string::npos);
}

TEST(USBRecoveryOpenHandle, OpenFailsForMissingNode)
{
    // bus/devnum that will not exist as a usbfs node on the test host.
    USBDeviceLocation location{200, std::vector<uint8_t>{1}, uint8_t{201}};
    libusb_device_handle* handle = nullptr;
    ScopedFd wrappedFd;
    std::string status{};
    EXPECT_FALSE(openHandleDirectFromUsbfs(nullptr, location, &handle,
                                           wrappedFd, status));
    EXPECT_NE(status.find("open("), std::string::npos);
}

// ---------------------------------------------------------------------------
// findBulkOutEndpoint branch coverage. The libusb descriptor fetches are
// intercepted via ld --wrap so crafted descriptors can drive every branch.
// ---------------------------------------------------------------------------

// State shared with the __wrap_libusb_* functions defined below.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
libusb_config_descriptor* gFakeConfig = nullptr;
int gActiveConfigRc = LIBUSB_SUCCESS;
int gFallbackConfigRc = LIBUSB_SUCCESS;
int gFreeConfigCount = 0;

// Device-descriptor mock state for the RCM recovery-mode check. Defaults to a
// non-recovery VID:PID so existing clearHaltOnHandle tests are unaffected.
int gGetDeviceDescriptorRc = LIBUSB_SUCCESS;
uint16_t gDeviceVendorId = 0x0000;
uint16_t gDeviceProductId = 0x0000;

// Handle-operation mock state for clearHaltOnHandle.
int gWrapSysDeviceRc = LIBUSB_SUCCESS;
bool gGetDeviceReturnsNull = false;
int gKernelDriverActiveRc = 0; // 0 == driver not active
int gDetachRc = LIBUSB_SUCCESS;
int gClaimRc = LIBUSB_SUCCESS;
int gAttachRc = LIBUSB_SUCCESS;
int gClearHaltRc = LIBUSB_SUCCESS;
int gDetachCount = 0;
int gAttachCount = 0;
int gClaimCount = 0;
int gClearHaltCount = 0;
int gReleaseCount = 0;
int gLibusbInitRc = LIBUSB_SUCCESS;
int gLibusbInitCount = 0;
int gLibusbCloseCount = 0;
int gLibusbExitCount = 0;
bool gRedirectUSBRecoveryPaths = false;
std::filesystem::path gUSBRecoveryNetRoot;
std::filesystem::path gUSBRecoveryUsbfsRoot;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void resetFakeConfigState()
{
    gFakeConfig = nullptr;
    gActiveConfigRc = LIBUSB_SUCCESS;
    gFallbackConfigRc = LIBUSB_SUCCESS;
    gFreeConfigCount = 0;
}

void resetFakeHandleState()
{
    gGetDeviceDescriptorRc = LIBUSB_SUCCESS;
    gDeviceVendorId = 0x0000;
    gDeviceProductId = 0x0000;
    gWrapSysDeviceRc = LIBUSB_SUCCESS;
    gGetDeviceReturnsNull = false;
    gKernelDriverActiveRc = 0;
    gDetachRc = LIBUSB_SUCCESS;
    gClaimRc = LIBUSB_SUCCESS;
    gAttachRc = LIBUSB_SUCCESS;
    gClearHaltRc = LIBUSB_SUCCESS;
    gDetachCount = 0;
    gAttachCount = 0;
    gClaimCount = 0;
    gClearHaltCount = 0;
    gReleaseCount = 0;
    gLibusbInitRc = LIBUSB_SUCCESS;
    gLibusbInitCount = 0;
    gLibusbCloseCount = 0;
    gLibusbExitCount = 0;
}

TEST(USBRecoveryApxDevice, descriptorReadFailureReturnsFalse)
{
    resetFakeHandleState();
    gGetDeviceDescriptorRc = LIBUSB_ERROR_IO;
    // Opaque sentinel; the wrapper does not dereference the device.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    auto* device = reinterpret_cast<libusb_device*>(0x1);

    EXPECT_FALSE(isNvidiaApxDevice(device));
}

TEST(USBRecoveryFindBulkOutEndpoint, BothDescriptorFetchesFailReturnsNullopt)
{
    resetFakeConfigState();
    gActiveConfigRc = LIBUSB_ERROR_NOT_FOUND;
    gFallbackConfigRc = LIBUSB_ERROR_NOT_FOUND;

    EXPECT_FALSE(findBulkOutEndpoint(nullptr).has_value());
    // No descriptor was obtained, so nothing should have been freed.
    EXPECT_EQ(gFreeConfigCount, 0);
}

TEST(USBRecoveryFindBulkOutEndpoint, FallbackDescriptorSucceedsFindsBulkOut)
{
    resetFakeConfigState();
    gActiveConfigRc = LIBUSB_ERROR_NOT_FOUND; // active fails, fallback used
    gFallbackConfigRc = LIBUSB_SUCCESS;

    libusb_endpoint_descriptor bulkOut{};
    bulkOut.bEndpointAddress = 0x04; // OUT (0x80 bit clear)
    bulkOut.bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;

    libusb_interface_descriptor altsetting{};
    altsetting.bInterfaceNumber = 2;
    altsetting.bInterfaceClass = mctpInterfaceClass;
    altsetting.bNumEndpoints = 1;
    altsetting.endpoint = &bulkOut;

    libusb_interface iface{};
    iface.altsetting = &altsetting;
    iface.num_altsetting = 1;

    libusb_config_descriptor config{};
    config.bNumInterfaces = 1;
    config.interface = &iface;
    gFakeConfig = &config;

    auto endpoint = findBulkOutEndpoint(nullptr);
    ASSERT_TRUE(endpoint.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access): guarded by ASSERT_TRUE.
    EXPECT_EQ(endpoint.value().endpointAddress, 0x04);
    EXPECT_EQ(endpoint.value().interfaceNumber, 2);
    // NOLINTEND(bugprone-unchecked-optional-access)
    EXPECT_EQ(gFreeConfigCount, 1);
}

TEST(USBRecoveryFindBulkOutEndpoint, SkipsNonMctpInterfaceThenMatches)
{
    resetFakeConfigState();
    gActiveConfigRc = LIBUSB_SUCCESS;

    libusb_endpoint_descriptor bulkOut{};
    bulkOut.bEndpointAddress = 0x05;
    bulkOut.bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;

    libusb_interface_descriptor nonMctpAlt{};
    nonMctpAlt.bInterfaceNumber = 0;
    nonMctpAlt.bInterfaceClass = 0xFF; // not MCTP -> skipped
    nonMctpAlt.bNumEndpoints = 1;
    nonMctpAlt.endpoint = &bulkOut;

    libusb_interface_descriptor mctpAlt{};
    mctpAlt.bInterfaceNumber = 3;
    mctpAlt.bInterfaceClass = mctpInterfaceClass;
    mctpAlt.bNumEndpoints = 1;
    mctpAlt.endpoint = &bulkOut;

    std::array<libusb_interface, 2> ifaces{};
    ifaces[0].altsetting = &nonMctpAlt;
    ifaces[0].num_altsetting = 1;
    ifaces[1].altsetting = &mctpAlt;
    ifaces[1].num_altsetting = 1;

    libusb_config_descriptor config{};
    config.bNumInterfaces = 2;
    config.interface = ifaces.data();
    gFakeConfig = &config;

    auto endpoint = findBulkOutEndpoint(nullptr);
    ASSERT_TRUE(endpoint.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT.
    EXPECT_EQ(endpoint.value().interfaceNumber, 3);
}

TEST(USBRecoveryFindBulkOutEndpoint, MctpInterfaceWithoutBulkOutReturnsNullopt)
{
    resetFakeConfigState();
    gActiveConfigRc = LIBUSB_SUCCESS;

    // An IN bulk endpoint and an interrupt OUT endpoint: neither is bulk OUT.
    std::array<libusb_endpoint_descriptor, 2> endpoints{};
    endpoints[0].bEndpointAddress = 0x84; // IN
    endpoints[0].bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;
    endpoints[1].bEndpointAddress = 0x06; // OUT but interrupt
    endpoints[1].bmAttributes = LIBUSB_TRANSFER_TYPE_INTERRUPT;

    libusb_interface_descriptor altsetting{};
    altsetting.bInterfaceNumber = 1;
    altsetting.bInterfaceClass = mctpInterfaceClass;
    altsetting.bNumEndpoints = 2;
    altsetting.endpoint = endpoints.data();

    libusb_interface iface{};
    iface.altsetting = &altsetting;
    iface.num_altsetting = 1;

    libusb_config_descriptor config{};
    config.bNumInterfaces = 1;
    config.interface = &iface;
    gFakeConfig = &config;

    EXPECT_FALSE(findBulkOutEndpoint(nullptr).has_value());
    EXPECT_EQ(gFreeConfigCount, 1);
    gFakeConfig = nullptr;
}

// ---------------------------------------------------------------------------
// clearHaltOnHandle branch coverage. All libusb handle operations are
// intercepted via ld --wrap, so the driver-detach / claim / clear-halt /
// re-attach branches can be exercised without a real USB device. The handle
// pointer is an opaque non-null sentinel that the wrappers ignore.
// ---------------------------------------------------------------------------
class ClearHaltOnHandleTest : public ::testing::Test
{
  protected:
    libusb_endpoint_descriptor bulkOut{};
    libusb_interface_descriptor altsetting{};
    libusb_interface iface{};
    libusb_config_descriptor config{};
    libusb_device_handle* handle = nullptr;

    void SetUp() override
    {
        resetFakeConfigState();
        resetFakeHandleState();

        // Opaque non-null handle sentinel; wrappers never dereference it.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
        handle = reinterpret_cast<libusb_device_handle*>(0x2);

        bulkOut = {};
        bulkOut.bEndpointAddress = 0x04; // OUT
        bulkOut.bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;

        altsetting = {};
        altsetting.bInterfaceNumber = 2;
        altsetting.bInterfaceClass = mctpInterfaceClass;
        altsetting.bNumEndpoints = 1;
        altsetting.endpoint = &bulkOut;

        iface = {};
        iface.altsetting = &altsetting;
        iface.num_altsetting = 1;

        config = {};
        config.bNumInterfaces = 1;
        config.interface = &iface;

        gFakeConfig = &config;
        gActiveConfigRc = LIBUSB_SUCCESS;
    }
};

TEST_F(ClearHaltOnHandleTest, NullDeviceFails)
{
    gGetDeviceReturnsNull = true;
    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    std::string status{};
    EXPECT_FALSE(clearHaltOnHandle(handle, location, status));
    EXPECT_NE(status.find("libusb_get_device returned null"),
              std::string::npos);
}

TEST_F(ClearHaltOnHandleTest, NoBulkOutEndpointFails)
{
    altsetting.bInterfaceClass = 0xFF; // not MCTP -> no endpoint found
    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    std::string status{};
    EXPECT_FALSE(clearHaltOnHandle(handle, location, status));
    EXPECT_NE(status.find("No MCTP class bulk OUT endpoint"),
              std::string::npos);
}

TEST_F(ClearHaltOnHandleTest, RecoveryModeDeviceSkipsClearHalt)
{
    // NVIDIA RCM recovery signature: recovery VID:PID plus interface 3 with the
    // recovery bulk endpoint 0x08.
    gDeviceVendorId = 0x0955;
    gDeviceProductId = 0x7410;
    bulkOut.bEndpointAddress = 0x08;
    altsetting.bInterfaceNumber = 3;

    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    std::string status;
    EXPECT_TRUE(clearHaltOnHandle(handle, location, status));
    EXPECT_NE(status.find("RCM recovery mode"), std::string::npos);
    // The clear-halt and the claim/detach that precede it must be skipped.
    EXPECT_EQ(gClearHaltCount, 0);
    EXPECT_EQ(gClaimCount, 0);
    EXPECT_EQ(gDetachCount, 0);
}

TEST_F(ClearHaltOnHandleTest, RecoveryVidPidWithoutRecoveryEndpointStillClears)
{
    // Recovery VID:PID but the MCTP interface/endpoint layout (not the recovery
    // interface 3 / endpoint 0x08): this is not recovery mode, so clear-halt
    // proceeds normally.
    gDeviceVendorId = 0x0955;
    gDeviceProductId = 0x7410;

    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    std::string status;
    EXPECT_TRUE(clearHaltOnHandle(handle, location, status));
    EXPECT_NE(status.find("Cleared halt on endpoint"), std::string::npos);
    EXPECT_EQ(gClearHaltCount, 1);
}

TEST_F(ClearHaltOnHandleTest, RecoveryVidPidInterface3InEndpoint8StillClears)
{
    // APX VID:PID with recovery interface 3, but the endpoint numbered 8 is IN
    // (0x88), not the bulk OUT recovery endpoint. A mask-only match would treat
    // 0x88 as the recovery endpoint (0x88 & 0x7F == 0x08); ensure we no longer
    // over-match on direction and the normal clear-halt still runs against the
    // real bulk OUT endpoint.
    gDeviceVendorId = 0x0955;
    gDeviceProductId = 0x7410;

    std::array<libusb_endpoint_descriptor, 2> endpoints{};
    endpoints[0].bEndpointAddress = 0x88; // IN endpoint 8 (would false-match)
    endpoints[0].bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;
    endpoints[1].bEndpointAddress = 0x04; // real bulk OUT
    endpoints[1].bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;

    altsetting.bInterfaceNumber = 3;
    altsetting.bNumEndpoints = 2;
    altsetting.endpoint = endpoints.data();

    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    std::string status;
    EXPECT_TRUE(clearHaltOnHandle(handle, location, status));
    EXPECT_NE(status.find("Cleared halt on endpoint"), std::string::npos);
    EXPECT_EQ(gClearHaltCount, 1);
}

TEST_F(ClearHaltOnHandleTest, SuccessWithKernelDriverDetachAndReattach)
{
    gKernelDriverActiveRc = 1; // active -> detach then re-attach
    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    std::string status{};
    EXPECT_TRUE(clearHaltOnHandle(handle, location, status));
    EXPECT_NE(status.find("Cleared halt on endpoint"), std::string::npos);
    EXPECT_EQ(gDetachCount, 1);
    EXPECT_EQ(gClaimCount, 1);
    EXPECT_EQ(gClearHaltCount, 1);
    EXPECT_EQ(gReleaseCount, 1);
    EXPECT_EQ(gAttachCount, 1);
}

TEST_F(ClearHaltOnHandleTest, SuccessWithoutKernelDriver)
{
    gKernelDriverActiveRc = 0; // not active -> no detach/reattach
    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    std::string status{};
    EXPECT_TRUE(clearHaltOnHandle(handle, location, status));
    EXPECT_EQ(gDetachCount, 0);
    EXPECT_EQ(gAttachCount, 0);
    EXPECT_EQ(gClearHaltCount, 1);
    EXPECT_EQ(gReleaseCount, 1);
}

TEST_F(ClearHaltOnHandleTest, KernelDriverQueryErrorIsTolerated)
{
    // Negative, non-NOT_SUPPORTED result -> warning branch, then proceeds.
    gKernelDriverActiveRc = LIBUSB_ERROR_OTHER;
    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    std::string status{};
    EXPECT_TRUE(clearHaltOnHandle(handle, location, status));
    EXPECT_EQ(gDetachCount, 0);
}

TEST_F(ClearHaltOnHandleTest, KernelDriverNotSupportedIsTolerated)
{
    gKernelDriverActiveRc = LIBUSB_ERROR_NOT_SUPPORTED;
    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    std::string status{};
    EXPECT_TRUE(clearHaltOnHandle(handle, location, status));
    EXPECT_EQ(gDetachCount, 0);
    EXPECT_EQ(gAttachCount, 0);
}

TEST_F(ClearHaltOnHandleTest, DetachFailureAborts)
{
    gKernelDriverActiveRc = 1;
    gDetachRc = LIBUSB_ERROR_OTHER;
    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    std::string status{};
    EXPECT_FALSE(clearHaltOnHandle(handle, location, status));
    EXPECT_NE(status.find("libusb_detach_kernel_driver failed"),
              std::string::npos);
    EXPECT_EQ(gClaimCount, 0);
}

TEST_F(ClearHaltOnHandleTest, ClaimFailureReattachesAfterDetach)
{
    gKernelDriverActiveRc = 1;
    gClaimRc = LIBUSB_ERROR_BUSY;
    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    std::string status{};
    EXPECT_FALSE(clearHaltOnHandle(handle, location, status));
    EXPECT_NE(status.find("libusb_claim_interface failed"), std::string::npos);
    // The kernel driver detached for the attempt must be re-attached.
    EXPECT_EQ(gAttachCount, 1);
    EXPECT_EQ(gClearHaltCount, 0);
}

TEST_F(ClearHaltOnHandleTest, ClaimFailureWithoutDriverDoesNotAttach)
{
    gKernelDriverActiveRc = 0;
    gClaimRc = LIBUSB_ERROR_BUSY;
    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    std::string status{};
    EXPECT_FALSE(clearHaltOnHandle(handle, location, status));
    EXPECT_NE(status.find("libusb_claim_interface failed"), std::string::npos);
    EXPECT_EQ(gDetachCount, 0);
    EXPECT_EQ(gAttachCount, 0);
    EXPECT_EQ(gClearHaltCount, 0);
}

TEST_F(ClearHaltOnHandleTest, ClaimFailureReattachAlsoFails)
{
    gKernelDriverActiveRc = 1;
    gClaimRc = LIBUSB_ERROR_BUSY;
    gAttachRc = LIBUSB_ERROR_OTHER; // re-attach also fails -> warning branch
    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    std::string status{};
    EXPECT_FALSE(clearHaltOnHandle(handle, location, status));
    EXPECT_EQ(gAttachCount, 1);
}

TEST_F(ClearHaltOnHandleTest, ClearHaltFailureReportsError)
{
    gClearHaltRc = LIBUSB_ERROR_PIPE;
    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    std::string status{};
    EXPECT_FALSE(clearHaltOnHandle(handle, location, status));
    EXPECT_NE(status.find("libusb_clear_halt failed"), std::string::npos);
    // Interface is still released even when clear-halt fails.
    EXPECT_EQ(gReleaseCount, 1);
}

TEST_F(ClearHaltOnHandleTest, ReattachFailureAfterClearHaltStillSucceeds)
{
    gKernelDriverActiveRc = 1;
    gAttachRc = LIBUSB_ERROR_OTHER; // re-attach after success warns, not fatal
    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    std::string status{};
    EXPECT_TRUE(clearHaltOnHandle(handle, location, status));
    EXPECT_EQ(gClearHaltCount, 1);
    EXPECT_EQ(gAttachCount, 1);
}

// ---------------------------------------------------------------------------
// openHandleDirectFromUsbfs open-success path. A real file under a temp usbfs
// root makes open() succeed; libusb_wrap_sys_device is intercepted so both the
// wrap-success and wrap-failure branches are exercised.
// ---------------------------------------------------------------------------
TEST(USBRecoveryOpenHandle, OpenSuccessWrapSuccess)
{
    resetFakeHandleState();
    auto root = std::filesystem::temp_directory_path() / "usbrec_usbfs_ok";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "001");
    writeSysfsValue(root / "001" / "013", "node");

    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    libusb_device_handle* handle = nullptr;
    ScopedFd wrappedFd;
    std::string status{};
    gWrapSysDeviceRc = LIBUSB_SUCCESS;

    EXPECT_TRUE(openHandleDirectFromUsbfs(nullptr, location, &handle, wrappedFd,
                                          status, root));
    EXPECT_NE(handle, nullptr);
    EXPECT_GE(wrappedFd.get(), 0);
    EXPECT_TRUE(status.empty());

    std::filesystem::remove_all(root);
}

TEST(USBRecoveryOpenHandle, OpenSuccessWrapFails)
{
    resetFakeHandleState();
    auto root = std::filesystem::temp_directory_path() /
                "usbrec_usbfs_wrapfail";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "001");
    writeSysfsValue(root / "001" / "013", "node");

    USBDeviceLocation location{1, std::vector<uint8_t>{1}, uint8_t{13}};
    libusb_device_handle* handle = nullptr;
    ScopedFd wrappedFd;
    std::string status{};
    gWrapSysDeviceRc = LIBUSB_ERROR_ACCESS;

    EXPECT_FALSE(openHandleDirectFromUsbfs(nullptr, location, &handle,
                                           wrappedFd, status, root));
    EXPECT_NE(status.find("libusb_wrap_sys_device failed"), std::string::npos);

    std::filesystem::remove_all(root);
}

class USBRecoveryOrchestrationTest : public ::testing::Test
{
  protected:
    std::filesystem::path netRoot =
        std::filesystem::temp_directory_path() /
        ("usbrec_orchestration_net_" + std::to_string(getpid()));
    std::filesystem::path usbfsRoot =
        std::filesystem::temp_directory_path() /
        ("usbrec_orchestration_usbfs_" + std::to_string(getpid()));
    libusb_endpoint_descriptor bulkOut{};
    libusb_interface_descriptor altsetting{};
    libusb_interface iface{};
    libusb_config_descriptor config{};

    void SetUp() override
    {
        resetFakeConfigState();
        resetFakeHandleState();

        std::filesystem::remove_all(netRoot);
        std::filesystem::remove_all(usbfsRoot);
        std::filesystem::create_directories(netRoot / "usbx" / "device");
        std::filesystem::create_directories(usbfsRoot / "001");
        writeSysfsValue(netRoot / "usbx" / "device" / "busnum", "1\n");
        writeSysfsValue(netRoot / "usbx" / "device" / "devpath", "2.3\n");
        writeSysfsValue(netRoot / "usbx" / "device" / "devnum", "13\n");
        writeSysfsValue(usbfsRoot / "001" / "013", "node");

        bulkOut.bEndpointAddress = 0x04;
        bulkOut.bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;
        altsetting.bInterfaceNumber = 2;
        altsetting.bInterfaceClass = mctpInterfaceClass;
        altsetting.bNumEndpoints = 1;
        altsetting.endpoint = &bulkOut;
        iface.altsetting = &altsetting;
        iface.num_altsetting = 1;
        config.bNumInterfaces = 1;
        config.interface = &iface;
        gFakeConfig = &config;

        gUSBRecoveryNetRoot = netRoot;
        gUSBRecoveryUsbfsRoot = usbfsRoot;
        gRedirectUSBRecoveryPaths = true;
    }

    void TearDown() override
    {
        gRedirectUSBRecoveryPaths = false;
        gUSBRecoveryNetRoot.clear();
        gUSBRecoveryUsbfsRoot.clear();
        std::filesystem::remove_all(netRoot);
        std::filesystem::remove_all(usbfsRoot);
        resetFakeConfigState();
        resetFakeHandleState();
    }
};

TEST_F(USBRecoveryOrchestrationTest, LibusbInitFailureReturnsError)
{
    gLibusbInitRc = LIBUSB_ERROR_OTHER;
    LibusbUSBRecovery recovery;
    std::string status{};

    EXPECT_FALSE(recovery.clearBulkOutHalt("usbx", status));
    EXPECT_NE(status.find("libusb_init failed"), std::string::npos);
    EXPECT_EQ(gLibusbInitCount, 1);
    EXPECT_EQ(gLibusbCloseCount, 0);
    EXPECT_EQ(gLibusbExitCount, 0);
}

TEST_F(USBRecoveryOrchestrationTest, DirectOpenFailureExitsContext)
{
    std::filesystem::remove(usbfsRoot / "001" / "013");
    LibusbUSBRecovery recovery;
    std::string status{};

    EXPECT_FALSE(recovery.clearBulkOutHalt("usbx", status));
    EXPECT_NE(status.find("Direct USB open failed"), std::string::npos);
    EXPECT_EQ(gLibusbInitCount, 1);
    EXPECT_EQ(gLibusbCloseCount, 0);
    EXPECT_EQ(gLibusbExitCount, 1);
}

TEST_F(USBRecoveryOrchestrationTest, ClearHaltSuccessClosesResources)
{
    LibusbUSBRecovery recovery;
    std::string status{};

    EXPECT_TRUE(recovery.clearBulkOutHalt("usbx", status));
    EXPECT_NE(status.find("Cleared halt on endpoint"), std::string::npos);
    EXPECT_EQ(gLibusbInitCount, 1);
    EXPECT_EQ(gLibusbCloseCount, 1);
    EXPECT_EQ(gLibusbExitCount, 1);
}

TEST_F(USBRecoveryOrchestrationTest, ClearHaltFailureClosesResources)
{
    gClearHaltRc = LIBUSB_ERROR_PIPE;
    LibusbUSBRecovery recovery;
    std::string status{};

    EXPECT_FALSE(recovery.clearBulkOutHalt("usbx", status));
    EXPECT_NE(status.find("libusb_clear_halt failed"), std::string::npos);
    EXPECT_EQ(gLibusbInitCount, 1);
    EXPECT_EQ(gLibusbCloseCount, 1);
    EXPECT_EQ(gLibusbExitCount, 1);
}

} // namespace

std::filesystem::path
    realWeaklyCanonical(const std::filesystem::path& path, std::error_code& ec) asm(
        "__real__ZNSt10filesystem16weakly_canonicalERKNS_7__cxx114pathERSt10error_code");
std::filesystem::path
    wrapWeaklyCanonical(const std::filesystem::path& path, std::error_code& ec) asm(
        "__wrap__ZNSt10filesystem16weakly_canonicalERKNS_7__cxx114pathERSt10error_code");

std::filesystem::path wrapWeaklyCanonical(const std::filesystem::path& path,
                                          std::error_code& ec)
{
    static const std::filesystem::path productionPath =
        "/sys/class/net/usbx/device";
    if (gRedirectUSBRecoveryPaths && path == productionPath)
    {
        return realWeaklyCanonical(gUSBRecoveryNetRoot / "usbx" / "device", ec);
    }
    return realWeaklyCanonical(path, ec);
}

// open(2) is variadic when O_CREAT/O_TMPFILE supplies a mode.  These
// declarations retain that ABI for ld --wrap while exposing non-reserved C++
// identifiers to static analysis.
// NOLINTBEGIN(cert-dcl50-cpp,cppcoreguidelines-pro-bounds-array-to-pointer-decay,cppcoreguidelines-pro-type-vararg)
extern "C" int realOpen(const char* path, int flags, ...) asm("__real_open");
extern "C" int realOpen64(const char* path, int flags,
                          ...) asm("__real_open64");

namespace
{
std::string redirectUSBRecoveryOpenPath(const char* path)
{
    std::filesystem::path original(path);
    static const std::filesystem::path productionRoot = "/dev/bus/usb";
    if (!gRedirectUSBRecoveryPaths)
    {
        return original.string();
    }

    std::filesystem::path relative =
        original.lexically_relative(productionRoot);
    if (relative.empty() || relative == "." || *relative.begin() == "..")
    {
        return original.string();
    }
    return (gUSBRecoveryUsbfsRoot / relative).string();
}

bool openNeedsMode(int flags)
{
    bool needsMode = (flags & O_CREAT) != 0;
#ifdef O_TMPFILE
    needsMode = needsMode || ((flags & O_TMPFILE) == O_TMPFILE);
#endif
    return needsMode;
}
} // namespace

extern "C" int wrapOpen(const char* path, int flags, ...) asm("__wrap_open");
extern "C" int wrapOpen(const char* path, int flags, ...)
{
    const std::string redirected = redirectUSBRecoveryOpenPath(path);
    if (!openNeedsMode(flags))
    {
        return realOpen(redirected.c_str(), flags);
    }

    va_list args;
    va_start(args, flags);
    mode_t mode = va_arg(args, mode_t);
    va_end(args);
    return realOpen(redirected.c_str(), flags, mode);
}

extern "C" int wrapOpen64(const char* path, int flags,
                          ...) asm("__wrap_open64");
extern "C" int wrapOpen64(const char* path, int flags, ...)
{
    const std::string redirected = redirectUSBRecoveryOpenPath(path);
    if (!openNeedsMode(flags))
    {
        return realOpen64(redirected.c_str(), flags);
    }

    va_list args;
    va_start(args, flags);
    mode_t mode = va_arg(args, mode_t);
    va_end(args);
    return realOpen64(redirected.c_str(), flags, mode);
}
// NOLINTEND(cert-dcl50-cpp,cppcoreguidelines-pro-bounds-array-to-pointer-decay,cppcoreguidelines-pro-type-vararg)

// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming,cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
extern "C" int __wrap_libusb_get_active_config_descriptor(
    libusb_device* /*dev*/, libusb_config_descriptor** config)
{
    if (gActiveConfigRc == LIBUSB_SUCCESS)
    {
        *config = gFakeConfig;
    }
    return gActiveConfigRc;
}

extern "C" int __wrap_libusb_get_device_descriptor(
    libusb_device* /*dev*/, libusb_device_descriptor* desc)
{
    if (gGetDeviceDescriptorRc == LIBUSB_SUCCESS && desc != nullptr)
    {
        *desc = {};
        desc->idVendor = gDeviceVendorId;
        desc->idProduct = gDeviceProductId;
    }
    return gGetDeviceDescriptorRc;
}

extern "C" libusb_device* __wrap_libusb_get_device(
    libusb_device_handle* /*dev_handle*/)
{
    if (gGetDeviceReturnsNull)
    {
        return nullptr;
    }
    // Opaque non-null sentinel; the descriptor wrappers ignore the device.
    return reinterpret_cast<libusb_device*>(0x1);
}

extern "C" int __wrap_libusb_wrap_sys_device(libusb_context* /*ctx*/,
                                             intptr_t /*sys_dev*/,
                                             libusb_device_handle** dev_handle)
{
    if (gWrapSysDeviceRc == LIBUSB_SUCCESS)
    {
        *dev_handle = reinterpret_cast<libusb_device_handle*>(0x3);
    }
    return gWrapSysDeviceRc;
}

extern "C" int __wrap_libusb_kernel_driver_active(
    libusb_device_handle* /*dev_handle*/, int /*interface_number*/)
{
    return gKernelDriverActiveRc;
}

extern "C" int __wrap_libusb_detach_kernel_driver(
    libusb_device_handle* /*dev_handle*/, int /*interface_number*/)
{
    ++gDetachCount;
    return gDetachRc;
}

extern "C" int __wrap_libusb_attach_kernel_driver(
    libusb_device_handle* /*dev_handle*/, int /*interface_number*/)
{
    ++gAttachCount;
    return gAttachRc;
}

extern "C" int __wrap_libusb_claim_interface(
    libusb_device_handle* /*dev_handle*/, int /*interface_number*/)
{
    ++gClaimCount;
    return gClaimRc;
}

extern "C" int __wrap_libusb_release_interface(
    libusb_device_handle* /*dev_handle*/, int /*interface_number*/)
{
    ++gReleaseCount;
    return LIBUSB_SUCCESS;
}

extern "C" int __wrap_libusb_clear_halt(libusb_device_handle* /*dev_handle*/,
                                        unsigned char /*endpoint*/)
{
    ++gClearHaltCount;
    return gClearHaltRc;
}

extern "C" int __wrap_libusb_get_config_descriptor(
    libusb_device* /*dev*/, uint8_t /*config_index*/,
    libusb_config_descriptor** config)
{
    if (gFallbackConfigRc == LIBUSB_SUCCESS)
    {
        *config = gFakeConfig;
    }
    return gFallbackConfigRc;
}

extern "C" void __wrap_libusb_free_config_descriptor(
    libusb_config_descriptor* /*config*/)
{
    ++gFreeConfigCount;
}

extern "C" int __wrap_libusb_init(libusb_context** context)
{
    ++gLibusbInitCount;
    if (gLibusbInitRc == LIBUSB_SUCCESS)
    {
        // Opaque non-null sentinel; every downstream libusb call is wrapped.
        *context = reinterpret_cast<libusb_context*>(0x4);
    }
    return gLibusbInitRc;
}

extern "C" void __wrap_libusb_close(libusb_device_handle* /*handle*/)
{
    ++gLibusbCloseCount;
}

extern "C" void __wrap_libusb_exit(libusb_context* /*context*/)
{
    ++gLibusbExitCount;
}
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming,cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)

// NOLINTEND(misc-include-cleaner)

// ---------------------------------------------------------------------------
// hasRecoveryInterface() branch coverage. The function scans the active (or,
// on failure, index-0) config descriptor for the RCM recovery interface
// (bInterfaceNumber == 3) carrying the bulk recovery endpoint
// (bEndpointAddress == 0x08). The libusb descriptor fetches are intercepted so
// crafted descriptors drive each branch without a real device.
// ---------------------------------------------------------------------------
TEST(USBRecoveryHasRecoveryInterface, RecoveryInterfaceWithBulkEndpointMatches)
{
    resetFakeConfigState();
    gActiveConfigRc = LIBUSB_SUCCESS;

    libusb_endpoint_descriptor recoveryEp{};
    recoveryEp.bEndpointAddress = 0x08; // recoveryEndpointAddress
    recoveryEp.bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;

    libusb_interface_descriptor altsetting{};
    altsetting.bInterfaceNumber = 3; // recoveryInterfaceNumber
    altsetting.bNumEndpoints = 1;
    altsetting.endpoint = &recoveryEp;

    libusb_interface iface{};
    iface.altsetting = &altsetting;
    iface.num_altsetting = 1;

    libusb_config_descriptor config{};
    config.bNumInterfaces = 1;
    config.interface = &iface;
    gFakeConfig = &config;

    EXPECT_TRUE(hasRecoveryInterface(nullptr));
    EXPECT_EQ(gFreeConfigCount, 1);
}

TEST(USBRecoveryHasRecoveryInterface, NonRecoveryInterfaceNumberIsSkipped)
{
    resetFakeConfigState();
    gActiveConfigRc = LIBUSB_SUCCESS;

    // A bulk endpoint at the recovery address but on the wrong interface number
    // must be skipped by the bInterfaceNumber != 3 guard.
    libusb_endpoint_descriptor recoveryEp{};
    recoveryEp.bEndpointAddress = 0x08;
    recoveryEp.bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;

    libusb_interface_descriptor altsetting{};
    altsetting.bInterfaceNumber = 1; // not the recovery interface
    altsetting.bNumEndpoints = 1;
    altsetting.endpoint = &recoveryEp;

    libusb_interface iface{};
    iface.altsetting = &altsetting;
    iface.num_altsetting = 1;

    libusb_config_descriptor config{};
    config.bNumInterfaces = 1;
    config.interface = &iface;
    gFakeConfig = &config;

    EXPECT_FALSE(hasRecoveryInterface(nullptr));
    EXPECT_EQ(gFreeConfigCount, 1);
}

TEST(USBRecoveryHasRecoveryInterface, ActiveDescriptorFailureUsesIndexZero)
{
    resetFakeConfigState();
    // Active descriptor fetch fails; the index-0 fallback supplies the config.
    gActiveConfigRc = LIBUSB_ERROR_NOT_FOUND;
    gFallbackConfigRc = LIBUSB_SUCCESS;

    libusb_endpoint_descriptor recoveryEp{};
    recoveryEp.bEndpointAddress = 0x08;
    recoveryEp.bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;

    libusb_interface_descriptor altsetting{};
    altsetting.bInterfaceNumber = 3;
    altsetting.bNumEndpoints = 1;
    altsetting.endpoint = &recoveryEp;

    libusb_interface iface{};
    iface.altsetting = &altsetting;
    iface.num_altsetting = 1;

    libusb_config_descriptor config{};
    config.bNumInterfaces = 1;
    config.interface = &iface;
    gFakeConfig = &config;

    EXPECT_TRUE(hasRecoveryInterface(nullptr));
    EXPECT_EQ(gFreeConfigCount, 1);
}

TEST(USBRecoveryHasRecoveryInterface, BothDescriptorFetchesFailReturnsFalse)
{
    resetFakeConfigState();
    gActiveConfigRc = LIBUSB_ERROR_NOT_FOUND;
    gFallbackConfigRc = LIBUSB_ERROR_NOT_FOUND;

    EXPECT_FALSE(hasRecoveryInterface(nullptr));
    // No descriptor obtained → nothing to free.
    EXPECT_EQ(gFreeConfigCount, 0);
}
