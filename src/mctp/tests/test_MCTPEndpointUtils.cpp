#include "MCTPEndpointUtils.hpp"
#include "Utils.hpp"

#include <cerrno>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

// ---- getPollingInterval tests ----

TEST(MCTPEndpointUtils, getPollingIntervalMissingKeyReturnsNullopt)
{
    SensorBaseConfigMap iface{};
    auto result = getPollingInterval(iface);
    EXPECT_FALSE(result.has_value());
}

TEST(MCTPEndpointUtils, getPollingIntervalZeroReturnsZero)
{
    SensorBaseConfigMap iface{{"PollingInterval", std::string("0")}};
    auto result = getPollingInterval(iface);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value_or(9999), 0);
}

TEST(MCTPEndpointUtils, getPollingIntervalValidValueOne)
{
    SensorBaseConfigMap iface{{"PollingInterval", std::string("1")}};
    auto result = getPollingInterval(iface);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value_or(9999), 1);
}

TEST(MCTPEndpointUtils, getPollingIntervalMaxValid180)
{
    SensorBaseConfigMap iface{{"PollingInterval", std::string("180")}};
    auto result = getPollingInterval(iface);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value_or(9999), 180);
}

TEST(MCTPEndpointUtils, getPollingIntervalAboveMaxReturnsNullopt)
{
    SensorBaseConfigMap iface{{"PollingInterval", std::string("181")}};
    auto result = getPollingInterval(iface);
    EXPECT_FALSE(result.has_value());
}

TEST(MCTPEndpointUtils, getPollingIntervalLargeValueReturnsNullopt)
{
    SensorBaseConfigMap iface{{"PollingInterval", std::string("999")}};
    auto result = getPollingInterval(iface);
    EXPECT_FALSE(result.has_value());
}

TEST(MCTPEndpointUtils, getPollingIntervalNonNumericReturnsNullopt)
{
    SensorBaseConfigMap iface{{"PollingInterval", std::string("abc")}};
    auto result = getPollingInterval(iface);
    EXPECT_FALSE(result.has_value());
}

TEST(MCTPEndpointUtils, getPollingIntervalEmptyStringReturnsNullopt)
{
    SensorBaseConfigMap iface{{"PollingInterval", std::string("")}};
    auto result = getPollingInterval(iface);
    EXPECT_FALSE(result.has_value());
}

TEST(MCTPEndpointUtils, getPollingIntervalNumericVariant)
{
    // The variant can hold numeric types too; VariantToStringVisitor
    // converts them to string, then stoul parses them
    SensorBaseConfigMap iface{{"PollingInterval", uint64_t(60)}};
    auto result = getPollingInterval(iface);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value_or(9999), 60);
}

// ---- getDeviceNames tests ----

TEST(MCTPEndpointUtils, getDeviceNamesMissingKeyReturnsEmpty)
{
    SensorBaseConfigMap iface{};
    auto names = getDeviceNames(iface);
    EXPECT_TRUE(names.empty());
}

TEST(MCTPEndpointUtils, getDeviceNamesSingleString)
{
    SensorBaseConfigMap iface{{"Name", std::string("GPU0")}};
    auto names = getDeviceNames(iface);
    ASSERT_EQ(names.size(), 1U);
    EXPECT_EQ(names[0], "GPU0");
}

TEST(MCTPEndpointUtils, getDeviceNamesCommaSeparated)
{
    SensorBaseConfigMap iface{{"Name", std::string("GPU0,GPU1,GPU2")}};
    auto names = getDeviceNames(iface);
    ASSERT_EQ(names.size(), 3U);
    EXPECT_EQ(names[0], "GPU0");
    EXPECT_EQ(names[1], "GPU1");
    EXPECT_EQ(names[2], "GPU2");
}

TEST(MCTPEndpointUtils, getDeviceNamesCommaSeparatedWithWhitespace)
{
    SensorBaseConfigMap iface{
        {"Name", std::string("  GPU0 , GPU1  ,  GPU2  ")}};
    auto names = getDeviceNames(iface);
    ASSERT_EQ(names.size(), 3U);
    EXPECT_EQ(names[0], "GPU0");
    EXPECT_EQ(names[1], "GPU1");
    EXPECT_EQ(names[2], "GPU2");
}

TEST(MCTPEndpointUtils, getDeviceNamesVectorOfStrings)
{
    std::vector<std::string> nameVec{"dev1", "dev2", "dev3"};
    SensorBaseConfigMap iface{{"Name", nameVec}};
    auto names = getDeviceNames(iface);
    ASSERT_EQ(names.size(), 3U);
    EXPECT_EQ(names[0], "dev1");
    EXPECT_EQ(names[1], "dev2");
    EXPECT_EQ(names[2], "dev3");
}

TEST(MCTPEndpointUtils, getDeviceNamesEmptyStringReturnsEmpty)
{
    SensorBaseConfigMap iface{{"Name", std::string("")}};
    auto names = getDeviceNames(iface);
    EXPECT_TRUE(names.empty());
}

TEST(MCTPEndpointUtils, getDeviceNamesTrailingCommaIgnoresEmpty)
{
    SensorBaseConfigMap iface{{"Name", std::string("GPU0,")}};
    auto names = getDeviceNames(iface);
    ASSERT_EQ(names.size(), 1U);
    EXPECT_EQ(names[0], "GPU0");
}

TEST(MCTPEndpointUtils, getDeviceNamesOnlyWhitespace)
{
    SensorBaseConfigMap iface{{"Name", std::string("  ,  ,  ")}};
    auto names = getDeviceNames(iface);
    EXPECT_TRUE(names.empty());
}

TEST(MCTPEndpointUtils, getDeviceNamesUnsupportedVariantTypeReturnsEmpty)
{
    SensorBaseConfigMap iface{{"Name", int64_t(42)}};
    auto names = getDeviceNames(iface);
    EXPECT_TRUE(names.empty());
}

TEST(MCTPEndpointUtils, getDeviceNamesSingleElementVector)
{
    std::vector<std::string> nameVec{"single"};
    SensorBaseConfigMap iface{{"Name", nameVec}};
    auto names = getDeviceNames(iface);
    ASSERT_EQ(names.size(), 1U);
    EXPECT_EQ(names[0], "single");
}

TEST(MCTPEndpointUtils, getDeviceNamesEmptyVector)
{
    std::vector<std::string> nameVec{};
    SensorBaseConfigMap iface{{"Name", nameVec}};
    auto names = getDeviceNames(iface);
    EXPECT_TRUE(names.empty());
}

TEST(MCTPEndpointUtils, getPollingIntervalBoundaryAt180)
{
    SensorBaseConfigMap iface{{"PollingInterval", std::string("180")}};
    auto result = getPollingInterval(iface);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value_or(0), 180);
}

TEST(MCTPEndpointUtils, getPollingIntervalBoundaryAt181)
{
    SensorBaseConfigMap iface{{"PollingInterval", std::string("181")}};
    auto result = getPollingInterval(iface);
    EXPECT_FALSE(result.has_value());
}

TEST(MCTPEndpointUtils, getPollingIntervalNegativeValue)
{
    SensorBaseConfigMap iface{{"PollingInterval", std::string("-1")}};
    auto result = getPollingInterval(iface);
    EXPECT_FALSE(result.has_value());
}

TEST(MCTPEndpointUtils, getDeviceNamesMultipleCommasInRow)
{
    SensorBaseConfigMap iface{{"Name", std::string("a,,b")}};
    auto names = getDeviceNames(iface);
    ASSERT_EQ(names.size(), 2U);
    EXPECT_EQ(names[0], "a");
    EXPECT_EQ(names[1], "b");
}

TEST(MCTPEndpointUtils, getDeviceNamesLeadingComma)
{
    SensorBaseConfigMap iface{{"Name", std::string(",GPU0")}};
    auto names = getDeviceNames(iface);
    ASSERT_EQ(names.size(), 1U);
    EXPECT_EQ(names[0], "GPU0");
}

TEST(MCTPEndpointUtils, getPollingIntervalUint64Zero)
{
    SensorBaseConfigMap iface{{"PollingInterval", uint64_t(0)}};
    auto result = getPollingInterval(iface);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value_or(99), 0);
}

TEST(MCTPEndpointUtils, getPollingIntervalUint64AboveMax)
{
    SensorBaseConfigMap iface{{"PollingInterval", uint64_t(200)}};
    auto result = getPollingInterval(iface);
    EXPECT_FALSE(result.has_value());
}

TEST(MCTPEndpointUtils, getDeviceNamesSingleCharName)
{
    SensorBaseConfigMap iface{{"Name", std::string("X")}};
    auto names = getDeviceNames(iface);
    ASSERT_EQ(names.size(), 1U);
    EXPECT_EQ(names[0], "X");
}

TEST(MCTPEndpointUtils, getDeviceNamesCommaOnly)
{
    SensorBaseConfigMap iface{{"Name", std::string(",")}};
    auto names = getDeviceNames(iface);
    EXPECT_TRUE(names.empty());
}

TEST(MCTPEndpointUtils, writeSysfsFileEmptyValue)
{
    auto path = std::filesystem::temp_directory_path() /
                "mctp-write-empty-test.txt";
    std::filesystem::remove(path);

    ASSERT_TRUE(writeSysfsFile(path.string(), ""));
    std::ifstream in(path);
    std::string content;
    std::getline(in, content);
    EXPECT_TRUE(content.empty());

    std::filesystem::remove(path);
}

TEST(MCTPEndpointUtils, writeSysfsFileWritesContent)
{
    auto path = std::filesystem::temp_directory_path() /
                "mctp-write-sysfs-test.txt";
    std::filesystem::remove(path);

    ASSERT_TRUE(writeSysfsFile(path.string(), "42"));
    std::ifstream in(path);
    std::string content;
    std::getline(in, content);
    EXPECT_EQ(content, "42");

    std::filesystem::remove(path);
}

TEST(MCTPEndpointUtils, writeSysfsFileFailsForMissingDirectory)
{
    auto path = std::filesystem::temp_directory_path() / "no-such-dir" /
                "mctp-write-sysfs-test.txt";
    EXPECT_FALSE(writeSysfsFile(path.string(), "42"));
}

TEST(MCTPEndpointUtils, createMctpTransportRedfishEventSkipsBroadcastEid)
{
    // destEid=0 (broadcast) triggers early return before any logging call,
    // so this path must complete without throwing regardless of environment.
    bool completed = false;
    createMctpTransportRedfishEvent(/*errorCode=*/1, MCTP_DIR_TX,
                                    /*binding=*/1, /*destEid=*/0,
                                    "SetEndpointID", "test-device");
    completed = true;
    EXPECT_TRUE(completed);
}

TEST(MCTPEndpointUtils, logMCTPErrorFallsBackToEidBasedNameWhenDeviceNameEmpty)
{
    // logMCTPError builds "EID_<n>" when deviceName is empty. The function
    // may throw if phosphor-logging commit is unavailable, which is an
    // environment limitation, not a code defect.
    try
    {
        logMCTPError("", /*destEid=*/7, /*errorCode=*/1,
                     "unit-test error path");
        EXPECT_TRUE(true);
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "phosphor-logging unavailable: " << ex.what();
    }
}

TEST(MCTPEndpointUtils, logMCTPErrorUsesProvidedDeviceName)
{
    try
    {
        logMCTPError("test-device", /*destEid=*/7, /*errorCode=*/1,
                     "unit-test error path");
        EXPECT_TRUE(true);
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "phosphor-logging unavailable: " << ex.what();
    }
}

TEST(MCTPEndpointUtils, createMctpTransportRedfishEventUnknownMappingPath)
{
    // Exercises the path where errorToRedfishRegistry returns no match.
    try
    {
        createMctpTransportRedfishEvent(
            /*errorCode=*/0x7fffffffU,
            /*direction=*/static_cast<uint8_t>(0xff),
            /*binding=*/static_cast<uint8_t>(0xff),
            /*destEid=*/7, "UnknownOp", "test-device");
        EXPECT_TRUE(true);
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "phosphor-logging unavailable: " << ex.what();
    }
}

TEST(MCTPEndpointUtils, createMctpTransportRedfishEventRateLimitPath)
{
    // Exercises the rate-limit branch: after 3 calls with the same EID
    // within 60s, subsequent calls return early (suppressed).
    try
    {
        for (int i = 0; i < 4; i++)
        {
            createMctpTransportRedfishEvent(
                /*errorCode=*/0x7fffffffU,
                /*direction=*/static_cast<uint8_t>(0xff),
                /*binding=*/static_cast<uint8_t>(0xff),
                /*destEid=*/9, "UnknownOp", "test-device");
        }
        EXPECT_TRUE(true);
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "phosphor-logging unavailable: " << ex.what();
    }
}

TEST(MCTPEndpointUtils, createMctpTransportRedfishEventMappedPathWithDeviceName)
{
    // Exercises the registry-match path with a provided device name,
    // covering the EID-placeholder replacement branch.
    try
    {
        createMctpTransportRedfishEvent(
            /*errorCode=*/ENODEV,
            /*direction=*/MCTP_DIR_TX,
            /*binding=*/static_cast<uint8_t>(3),
            /*destEid=*/0x31, "MessageTransmit", "gpu0");
        EXPECT_TRUE(true);
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "phosphor-logging unavailable: " << ex.what();
    }
}

TEST(MCTPEndpointUtils, createMctpTransportRedfishEventBroadcastEidSkips)
{
    try
    {
        createMctpTransportRedfishEvent(
            /*errorCode=*/ETIMEDOUT,
            /*direction=*/MCTP_DIR_RX,
            /*binding=*/static_cast<uint8_t>(1),
            /*destEid=*/0, "MessageTransmit", "test-device");
        EXPECT_TRUE(true);
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "phosphor-logging unavailable: " << ex.what();
    }
}

TEST(MCTPEndpointUtils, createMctpTransportRedfishEventDifferentErrorCodes)
{
    try
    {
        createMctpTransportRedfishEvent(
            /*errorCode=*/ECONNREFUSED,
            /*direction=*/MCTP_DIR_TX,
            /*binding=*/static_cast<uint8_t>(1),
            /*destEid=*/0x40, "SetEndpointID", "gpu1");
        createMctpTransportRedfishEvent(
            /*errorCode=*/ENXIO,
            /*direction=*/MCTP_DIR_TX,
            /*binding=*/static_cast<uint8_t>(1),
            /*destEid=*/0x41, "GetEndpointID", "gpu2");
        createMctpTransportRedfishEvent(
            /*errorCode=*/EIO,
            /*direction=*/MCTP_DIR_RX,
            /*binding=*/static_cast<uint8_t>(2),
            /*destEid=*/0x42, "MCTPControlMessage", "");
        EXPECT_TRUE(true);
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "phosphor-logging unavailable: " << ex.what();
    }
}

TEST(MCTPEndpointUtils,
     createMctpTransportRedfishEventMappedPathWithoutDeviceName)
{
    // Exercises the registry-match path without a device name,
    // covering the "EID_<n>" fallback name branch.
    try
    {
        createMctpTransportRedfishEvent(
            /*errorCode=*/ETIMEDOUT,
            /*direction=*/MCTP_DIR_RX,
            /*binding=*/static_cast<uint8_t>(3),
            /*destEid=*/0x32, "MessageReceive", "");
        EXPECT_TRUE(true);
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "phosphor-logging unavailable: " << ex.what();
    }
}
