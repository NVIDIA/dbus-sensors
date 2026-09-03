#include "MCTPDeviceRepository.hpp"
#include "MCTPEndpoint.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

class MockMCTPDevice : public MCTPDevice
{
  public:
    ~MockMCTPDevice() override = default;

    MOCK_METHOD(void, setup,
                (std::function<void(const std::error_code& ec,
                                    const std::shared_ptr<MCTPEndpoint>& ep)> &&
                 added),
                (override));
    MOCK_METHOD(void, remove, (), (override));
    MOCK_METHOD(std::string, describe, (), (const, override));
    MOCK_METHOD(std::size_t, id, (), (const, override));
};

class ThrowingNameMCTPDevice : public MockMCTPDevice
{
  public:
    std::optional<std::string> getNameForEid(uint8_t /*eid*/) const override
    {
        throw std::runtime_error("name lookup failed");
    }
};

TEST(MCTPDeviceRepository, addAndContains)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*device, describe())
        .WillRepeatedly(testing::Return("mock device"));

    repo.add("/test/inventory", device);
    EXPECT_TRUE(repo.contains(device));
}

TEST(MCTPDeviceRepository, containsReturnsFalseForUnknownDevice)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<MockMCTPDevice>();
    EXPECT_FALSE(repo.contains(device));
}

TEST(MCTPDeviceRepository, addSameDeviceTwiceIsNoOp)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*device, describe())
        .WillRepeatedly(testing::Return("mock device"));

    repo.add("/test/inventory", device);
    // Adding same device with same key should not throw
    EXPECT_NO_THROW(repo.add("/test/inventory", device));
    EXPECT_TRUE(repo.contains(device));
}

TEST(MCTPDeviceRepository, addDifferentDeviceToExistingKeyThrows)
{
    MCTPDeviceRepository repo;
    auto device1 = std::make_shared<MockMCTPDevice>();
    auto device2 = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*device1, describe())
        .WillRepeatedly(testing::Return("device 1"));
    EXPECT_CALL(*device2, describe())
        .WillRepeatedly(testing::Return("device 2"));

    repo.add("/test/inventory", device1);
    EXPECT_THROW(repo.add("/test/inventory", device2), std::system_error);
}

TEST(MCTPDeviceRepository, removeKnownDevice)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*device, describe())
        .WillRepeatedly(testing::Return("mock device"));

    repo.add("/test/inventory", device);
    EXPECT_TRUE(repo.contains(device));

    repo.remove(device);
    EXPECT_FALSE(repo.contains(device));
}

TEST(MCTPDeviceRepository, getStaticEidFromInterfaceSkipsNonMctpdDevices)
{
    MCTPDeviceRepository repo;
    auto plain = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*plain, describe())
        .WillRepeatedly(testing::Return("plain mock"));

    auto usb = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-main", "usb0", std::vector<uint8_t>{}, uint8_t{33},
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"usb-main"});

    repo.add("/inv/plain", plain);
    repo.add("/inv/usb0", usb);

    auto eid = repo.getStaticEidFromInterface("usb0");
    ASSERT_TRUE(eid.has_value());
    EXPECT_EQ(eid.value_or(0), 33);
}

TEST(MCTPDeviceRepository, inventoryForKnownDevice)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*device, describe())
        .WillRepeatedly(testing::Return("mock device"));

    repo.add("/test/inventory", device);
    auto inv = repo.inventoryFor(device);
    ASSERT_TRUE(inv.has_value());
    EXPECT_EQ(inv.value_or(""), "/test/inventory");
}

TEST(MCTPDeviceRepository, inventoryForUnknownDeviceReturnsEmpty)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<MockMCTPDevice>();

    auto inv = repo.inventoryFor(device);
    EXPECT_FALSE(inv.has_value());
}

TEST(MCTPDeviceRepository, deviceForKnownInventory)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*device, describe())
        .WillRepeatedly(testing::Return("mock device"));

    repo.add("/test/inventory", device);
    auto result = repo.deviceFor("/test/inventory");
    EXPECT_EQ(result.get(), device.get());
}

TEST(MCTPDeviceRepository, deviceForUnknownInventoryReturnsNull)
{
    MCTPDeviceRepository repo;
    auto result = repo.deviceFor("/nonexistent");
    EXPECT_EQ(result, nullptr);
}

TEST(MCTPDeviceRepository, multipleDevices)
{
    MCTPDeviceRepository repo;
    auto dev1 = std::make_shared<MockMCTPDevice>();
    auto dev2 = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*dev1, describe()).WillRepeatedly(testing::Return("device 1"));
    EXPECT_CALL(*dev2, describe()).WillRepeatedly(testing::Return("device 2"));

    repo.add("/inv/1", dev1);
    repo.add("/inv/2", dev2);

    EXPECT_TRUE(repo.contains(dev1));
    EXPECT_TRUE(repo.contains(dev2));
    EXPECT_EQ(repo.deviceFor("/inv/1").get(), dev1.get());
    EXPECT_EQ(repo.deviceFor("/inv/2").get(), dev2.get());
    EXPECT_EQ(repo.inventoryFor(dev1).value_or(""), "/inv/1");
    EXPECT_EQ(repo.inventoryFor(dev2).value_or(""), "/inv/2");
}

TEST(MCTPDeviceRepository, removeOneDeviceLeavesOthers)
{
    MCTPDeviceRepository repo;
    auto dev1 = std::make_shared<MockMCTPDevice>();
    auto dev2 = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*dev1, describe()).WillRepeatedly(testing::Return("device 1"));
    EXPECT_CALL(*dev2, describe()).WillRepeatedly(testing::Return("device 2"));

    repo.add("/inv/1", dev1);
    repo.add("/inv/2", dev2);

    repo.remove(dev1);
    EXPECT_FALSE(repo.contains(dev1));
    EXPECT_TRUE(repo.contains(dev2));
}

TEST(MCTPDeviceRepository, getNameForEidReturnsMappedNames)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-main", "usb0", std::vector<uint8_t>{}, uint8_t{9},
        uint8_t{10}, uint8_t{11}, std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"usb-main", "bridge-a", "bridge-b"});

    repo.add("/inv/usb0", device);

    const auto eid9 = repo.getNameForEid(9);
    ASSERT_TRUE(eid9.has_value());
    EXPECT_EQ(eid9.value_or(""), "usb-main");
    const auto eid10 = repo.getNameForEid(10);
    ASSERT_TRUE(eid10.has_value());
    EXPECT_EQ(eid10.value_or(""), "bridge-a");
    const auto eid11 = repo.getNameForEid(11);
    ASSERT_TRUE(eid11.has_value());
    EXPECT_EQ(eid11.value_or(""), "bridge-b");
    EXPECT_FALSE(repo.getNameForEid(12).has_value());
}

TEST(MCTPDeviceRepository, getStaticEidFromInterfaceReturnsValueForKnownDevice)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-main", "usb0", std::vector<uint8_t>{}, uint8_t{33},
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"usb-main"});

    repo.add("/inv/usb0", device);

    const auto staticEid = repo.getStaticEidFromInterface("usb0");
    ASSERT_TRUE(staticEid.has_value());
    EXPECT_EQ(staticEid.value_or(0), 33);
    EXPECT_FALSE(repo.getStaticEidFromInterface("usb1").has_value());
}

TEST(MCTPDeviceRepository, markDiscoveredMctpEndpointEidHandlesMixedDevices)
{
    MCTPDeviceRepository repo;
    EXPECT_NO_THROW(repo.markDiscoveredMctpEndpointEid(44));

    auto plain = std::make_shared<MockMCTPDevice>();
    auto mctp = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-main", "usb0", std::vector<uint8_t>{}, uint8_t{44},
        uint8_t{45}, uint8_t{46}, std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"usb-main", "bridge-a", "bridge-b"});

    repo.add("/inv/plain", plain);
    repo.add("/inv/usb0", mctp);

    EXPECT_NO_THROW(repo.markDiscoveredMctpEndpointEid(44));
    EXPECT_NO_THROW(repo.markDiscoveredMctpEndpointEid(45));
    EXPECT_NO_THROW(repo.markDiscoveredMctpEndpointEid(99));
    EXPECT_EQ(repo.getNameForEid(44).value_or(""), "usb-main");
    EXPECT_EQ(repo.getNameForEid(45).value_or(""), "bridge-a");
}

TEST(MCTPDeviceRepository, getNameForEidSkipsNonMctpdDevices)
{
    MCTPDeviceRepository repo;
    auto plain = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*plain, describe())
        .WillRepeatedly(testing::Return("plain mock device"));
    auto usb = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-main", "usb0", std::vector<uint8_t>{}, uint8_t{19},
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"usb-main"});

    repo.add("/inv/plain", plain);
    repo.add("/inv/usb0", usb);

    const auto usbMain = repo.getNameForEid(19);
    ASSERT_TRUE(usbMain.has_value());
    EXPECT_EQ(usbMain.value_or(""), "usb-main");
}

TEST(MCTPDeviceRepository, getStaticEidFromInterfaceReturnsNullForNoStaticEid)
{
    MCTPDeviceRepository repo;
    auto usb = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-main", "usb0", std::vector<uint8_t>{}, std::nullopt,
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"usb-main"});

    repo.add("/inv/usb0", usb);
    EXPECT_FALSE(repo.getStaticEidFromInterface("usb0").has_value());
}

TEST(MCTPDeviceRepository, getNameForEidFindsMatchAfterEarlierMiss)
{
    MCTPDeviceRepository repo;
    auto first = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-first", "usb0", std::vector<uint8_t>{}, uint8_t{9},
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"usb-first"});
    auto second = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-second", "usb1", std::vector<uint8_t>{}, uint8_t{19},
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"usb-second"});

    repo.add("/inv/first", first);
    repo.add("/inv/second", second);

    const auto usbSecond = repo.getNameForEid(19);
    ASSERT_TRUE(usbSecond.has_value());
    EXPECT_EQ(usbSecond.value_or(""), "usb-second");
}

TEST(MCTPDeviceRepository,
     getStaticEidFromInterfaceFindsMatchAfterEarlierInterfaceMismatch)
{
    MCTPDeviceRepository repo;
    auto first = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-first", "usb0", std::vector<uint8_t>{}, uint8_t{9},
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"usb-first"});
    auto second = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-second", "usb1", std::vector<uint8_t>{}, uint8_t{19},
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"usb-second"});

    repo.add("/inv/first", first);
    repo.add("/inv/second", second);

    const auto staticEid = repo.getStaticEidFromInterface("usb1");
    ASSERT_TRUE(staticEid.has_value());
    EXPECT_EQ(staticEid.value_or(0), 19);
}

TEST(MCTPDeviceRepository, removeAndReAddSameDevice)
{
    MCTPDeviceRepository repo;
    auto dev = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*dev, describe()).WillRepeatedly(testing::Return("reuse-dev"));

    repo.add("/inv/reuse", dev);
    EXPECT_TRUE(repo.contains(dev));
    repo.remove(dev);
    EXPECT_FALSE(repo.contains(dev));
    EXPECT_EQ(repo.deviceFor("/inv/reuse"), nullptr);
    repo.add("/inv/reuse", dev);
    EXPECT_TRUE(repo.contains(dev));
    EXPECT_EQ(repo.deviceFor("/inv/reuse").get(), dev.get());
}

TEST(MCTPDeviceRepository, addDeviceToDifferentPaths)
{
    MCTPDeviceRepository repo;
    auto dev1 = std::make_shared<MockMCTPDevice>();
    auto dev2 = std::make_shared<MockMCTPDevice>();
    auto dev3 = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*dev1, describe()).WillRepeatedly(testing::Return("dev1"));
    EXPECT_CALL(*dev2, describe()).WillRepeatedly(testing::Return("dev2"));
    EXPECT_CALL(*dev3, describe()).WillRepeatedly(testing::Return("dev3"));

    repo.add("/inv/a", dev1);
    repo.add("/inv/b", dev2);
    repo.add("/inv/c", dev3);

    EXPECT_EQ(repo.inventoryFor(dev1).value_or(""), "/inv/a");
    EXPECT_EQ(repo.inventoryFor(dev2).value_or(""), "/inv/b");
    EXPECT_EQ(repo.inventoryFor(dev3).value_or(""), "/inv/c");

    repo.remove(dev2);
    EXPECT_TRUE(repo.contains(dev1));
    EXPECT_FALSE(repo.contains(dev2));
    EXPECT_TRUE(repo.contains(dev3));
}

TEST(MCTPDeviceRepository, getNameForEidBridgePoolOutOfRangeIndex)
{
    MCTPDeviceRepository repo;
    auto dev = std::make_shared<USBMCTPDDevice>(
        nullptr, "main", "usb0", std::vector<uint8_t>{0x20}, uint8_t{9},
        uint8_t{10}, uint8_t{15}, std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"main", "bridge-0"});

    repo.add("/inv/pool", dev);

    EXPECT_EQ(repo.getNameForEid(9).value_or(""), "main");
    EXPECT_EQ(repo.getNameForEid(10).value_or(""), "bridge-0");
    EXPECT_FALSE(repo.getNameForEid(11).has_value());
}

TEST(MCTPDeviceRepository, getStaticEidFromInterfaceMatchesCorrectDevice)
{
    MCTPDeviceRepository repo;
    auto dev = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-match", "usb3", std::vector<uint8_t>{0x20},
        std::optional<uint8_t>(42));

    repo.add("/inv/match", dev);
    auto eid = repo.getStaticEidFromInterface("usb3");
    ASSERT_TRUE(eid.has_value());
    EXPECT_EQ(eid.value_or(0), 42);

    auto noMatch = repo.getStaticEidFromInterface("usb99");
    EXPECT_FALSE(noMatch.has_value());
}

TEST(MCTPDeviceRepository, getNameForEidReturnsNulloptWhenNoDeviceManagesEid)
{
    MCTPDeviceRepository repo;
    auto dev = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-no-eid", "usb0", std::vector<uint8_t>{0x20},
        std::optional<uint8_t>(10));

    repo.add("/inv/no-eid", dev);
    EXPECT_FALSE(repo.getNameForEid(99).has_value());
}

TEST(MCTPDeviceRepository, getStaticEidFromInterfaceEmptyRepoReturnsNullopt)
{
    MCTPDeviceRepository repo;
    EXPECT_FALSE(repo.getStaticEidFromInterface("usb0").has_value());
}

TEST(MCTPDeviceRepository, getNameForEidEmptyRepoReturnsNullopt)
{
    MCTPDeviceRepository repo;
    EXPECT_FALSE(repo.getNameForEid(1).has_value());
}

TEST(MCTPDeviceRepository, getNameForEidWithBridgePoolReturnsCorrectNames)
{
    MCTPDeviceRepository repo;
    auto dev = std::make_shared<USBMCTPDDevice>(
        nullptr, "main-dev", "usb0", std::vector<uint8_t>{0x20}, uint8_t{9},
        uint8_t{10}, uint8_t{12}, std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"main-dev", "bridge-0", "bridge-1",
                                 "bridge-2"});

    repo.add("/inv/bridge", dev);

    EXPECT_EQ(repo.getNameForEid(9).value_or(""), "main-dev");
    EXPECT_EQ(repo.getNameForEid(10).value_or(""), "bridge-0");
    EXPECT_EQ(repo.getNameForEid(11).value_or(""), "bridge-1");
    EXPECT_EQ(repo.getNameForEid(12).value_or(""), "bridge-2");
    EXPECT_FALSE(repo.getNameForEid(13).has_value());
    EXPECT_FALSE(repo.getNameForEid(8).has_value());
}

TEST(MCTPDeviceRepository, addRemoveAddSamePathSucceeds)
{
    MCTPDeviceRepository repo;
    auto dev1 = std::make_shared<MockMCTPDevice>();
    auto dev2 = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*dev1, describe()).WillRepeatedly(testing::Return("dev1"));
    EXPECT_CALL(*dev2, describe()).WillRepeatedly(testing::Return("dev2"));

    repo.add("/inv/reuse", dev1);
    repo.remove(dev1);
    EXPECT_FALSE(repo.contains(dev1));
    repo.add("/inv/reuse", dev2);
    EXPECT_TRUE(repo.contains(dev2));
    EXPECT_EQ(repo.deviceFor("/inv/reuse").get(), dev2.get());
}

TEST(MCTPDeviceRepository,
     getStaticEidFromInterfaceMultipleDevicesDifferentInterfaces)
{
    MCTPDeviceRepository repo;
    auto usb0 = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb0-dev", "usb0", std::vector<uint8_t>{}, uint8_t{10},
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"usb0-dev"});
    auto usb1 = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb1-dev", "usb1", std::vector<uint8_t>{}, uint8_t{20},
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"usb1-dev"});
    auto xrot = std::make_shared<XROTMCTPDDevice>(nullptr, "xrot-dev", "xrot0",
                                                  uint8_t{30});

    repo.add("/inv/usb0", usb0);
    repo.add("/inv/usb1", usb1);
    repo.add("/inv/xrot", xrot);

    EXPECT_EQ(repo.getStaticEidFromInterface("usb0").value_or(0), 10);
    EXPECT_EQ(repo.getStaticEidFromInterface("usb1").value_or(0), 20);
    EXPECT_EQ(repo.getStaticEidFromInterface("xrot0").value_or(0), 30);
    EXPECT_FALSE(repo.getStaticEidFromInterface("usb99").has_value());
}

// ---------------------------------------------------------------------------
// Extra tests targeting the 6 uncovered branches in MCTPDeviceRepository.hpp
// ---------------------------------------------------------------------------

TEST(MCTPDeviceRepository, getNameForEidBridgeStartSetButNoEndReturnsNullopt)
{
    MCTPDeviceRepository repo;
    // bridgePoolStartEid = 10, bridgePoolEndEid = nullopt
    auto dev = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-start-no-end", "usb0", std::vector<uint8_t>{},
        std::optional<uint8_t>(9), std::optional<uint8_t>(10), std::nullopt,
        std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"usb-start-no-end", "bridge-a"});

    repo.add("/inv/start-no-end", dev);

    // EID 9 is the static EID → should be found
    ASSERT_TRUE(repo.getNameForEid(9).has_value());
    EXPECT_EQ(repo.getNameForEid(9).value_or(""), "usb-start-no-end");

    // EID 10 is in the bridge pool start but end is null → bridge check
    // short-circuits, returns nullopt
    EXPECT_FALSE(repo.getNameForEid(10).has_value());
}

TEST(MCTPDeviceRepository, getStaticEidFromInterfaceReturnsEarlyOnFirstMatch)
{
    MCTPDeviceRepository repo;
    auto dev1 = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-first", "usb0", std::vector<uint8_t>{},
        std::optional<uint8_t>(11));
    auto dev2 = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-second", "usb1", std::vector<uint8_t>{},
        std::optional<uint8_t>(22));

    repo.add("/inv/usb0", dev1);
    repo.add("/inv/usb1", dev2);

    // Both interfaces exist; verify each is found independently.
    auto eid0 = repo.getStaticEidFromInterface("usb0");
    ASSERT_TRUE(eid0.has_value());
    EXPECT_EQ(eid0.value_or(0), 11);

    auto eid1 = repo.getStaticEidFromInterface("usb1");
    ASSERT_TRUE(eid1.has_value());
    EXPECT_EQ(eid1.value_or(0), 22);
}

TEST(MCTPDeviceRepository, getNameForEidNonMctpdFollowedByMctpdNoMatch)
{
    MCTPDeviceRepository repo;
    auto plain = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*plain, describe())
        .WillRepeatedly(testing::Return("plain mock"));

    auto usb = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-no-match", "usb0", std::vector<uint8_t>{},
        std::optional<uint8_t>(7));

    repo.add("/inv/plain", plain);
    repo.add("/inv/usb0", usb);

    // EID 99 is not managed by either device
    EXPECT_FALSE(repo.getNameForEid(99).has_value());
}

TEST(MCTPDeviceRepository,
     getStaticEidFromInterfaceNonMctpdFollowedByInterfaceMismatch)
{
    MCTPDeviceRepository repo;
    auto plain = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*plain, describe())
        .WillRepeatedly(testing::Return("plain mock"));

    auto usb = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-mismatch", "usb7", std::vector<uint8_t>{},
        std::optional<uint8_t>(50));

    repo.add("/inv/plain", plain);
    repo.add("/inv/usb7", usb);

    // "usb99" matches neither the plain device nor usb7's interface
    EXPECT_FALSE(repo.getStaticEidFromInterface("usb99").has_value());
    // "usb7" matches → returns 50
    auto eid = repo.getStaticEidFromInterface("usb7");
    ASSERT_TRUE(eid.has_value());
    EXPECT_EQ(eid.value_or(0), 50);
}

// ---------------------------------------------------------------------------
// G551–G562: Additional branch coverage tests
// ---------------------------------------------------------------------------

// G551: add same device pointer with a different inventory path — succeeds,
// device is reachable via both paths.
TEST(MCTPDeviceRepository, G551addSamePointerDifferentPathSucceeds)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*device, describe()).WillRepeatedly(testing::Return("shared"));

    repo.add("/inv/pathA", device);
    EXPECT_NO_THROW(repo.add("/inv/pathB", device));

    // contains() does a value-search so finds it from either insertion
    EXPECT_TRUE(repo.contains(device));
    // Both inventory paths resolve to the same device
    EXPECT_EQ(repo.deviceFor("/inv/pathA").get(), device.get());
    EXPECT_EQ(repo.deviceFor("/inv/pathB").get(), device.get());
}

// G553: contains() on an empty repository returns false.
TEST(MCTPDeviceRepository, G553containsEmptyRepoReturnsFalse)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<MockMCTPDevice>();
    // No describe() expectation needed — contains() must not call describe()
    EXPECT_FALSE(repo.contains(device));
}

// G555: getNameForEid() scans all devices but no device manages the queried
// EID → returns nullopt.  Uses two USBMCTPDDevices with known EIDs.
TEST(MCTPDeviceRepository, G555getNameForEidFullScanNoMatch)
{
    MCTPDeviceRepository repo;
    auto usb0 = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-a", "usb0", std::vector<uint8_t>{0x20},
        std::optional<uint8_t>(5));
    auto usb1 = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-b", "usb1", std::vector<uint8_t>{0x20},
        std::optional<uint8_t>(6));

    repo.add("/inv/usb0", usb0);
    repo.add("/inv/usb1", usb1);

    // EID 99 is not managed by usb0 (EID=5) or usb1 (EID=6)
    EXPECT_FALSE(repo.getNameForEid(99).has_value());
}

// G558: getNameForEid() correctly uses the device name stored at construction
// time, verifying that the returned string equals the name passed to the
// USBMCTPDDevice constructor.
TEST(MCTPDeviceRepository, G558getNameForEidReturnsConstructorName)
{
    MCTPDeviceRepository repo;
    auto dev = std::make_shared<USBMCTPDDevice>(
        nullptr, "my-sensor", "usb2", std::vector<uint8_t>{0x20},
        std::optional<uint8_t>(77));

    repo.add("/inv/sensor", dev);

    auto name = repo.getNameForEid(77);
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(name.value_or(""), "my-sensor");
}

// G559: Large repository scan — add 10 devices and verify getNameForEid()
// finds the last one (exercises the full loop body).
TEST(MCTPDeviceRepository, G559largeRepoScanFindsLastDevice)
{
    MCTPDeviceRepository repo;

    for (int i = 0; i < 9; ++i)
    {
        auto dev = std::make_shared<USBMCTPDDevice>(
            nullptr, "dev-" + std::to_string(i), "usb" + std::to_string(i),
            std::vector<uint8_t>{0x20},
            std::optional<uint8_t>(static_cast<uint8_t>(i + 1)));
        repo.add("/inv/dev" + std::to_string(i), dev);
    }

    // 10th device at EID 100
    auto last = std::make_shared<USBMCTPDDevice>(
        nullptr, "dev-last", "usb9", std::vector<uint8_t>{0x20},
        std::optional<uint8_t>(100));
    repo.add("/inv/dev9", last);

    auto name = repo.getNameForEid(100);
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(name.value_or(""), "dev-last");

    // EID not present in any device
    EXPECT_FALSE(repo.getNameForEid(200).has_value());
}

// G560: getNameForEid() with a device that has no staticEID (nullopt) — the
// device cannot match any EID, so the loop body's early-return branch is not
// taken and nullopt is returned.
TEST(MCTPDeviceRepository, G560skipNullEidDeviceInGetNameForEid)
{
    MCTPDeviceRepository repo;
    // Device has no staticEID and no bridge pool — getNameForEid always misses
    auto usb = std::make_shared<USBMCTPDDevice>(nullptr, "usb-null-eid", "usb4",
                                                std::vector<uint8_t>{0x20});

    repo.add("/inv/usb4", usb);

    EXPECT_FALSE(repo.getNameForEid(0).has_value());
    EXPECT_FALSE(repo.getNameForEid(255).has_value());
}

// G561: contains() returns false after a device is removed.
TEST(MCTPDeviceRepository, G561containsAfterRemove)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*device, describe()).WillRepeatedly(testing::Return("mock"));

    repo.add("/inv/dev", device);
    EXPECT_TRUE(repo.contains(device));

    repo.remove(device);
    EXPECT_FALSE(repo.contains(device));
}

// G562: inventoryFor() returns nullopt after the device has been removed.
TEST(MCTPDeviceRepository, G562inventoryForAfterRemove)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*device, describe()).WillRepeatedly(testing::Return("mock"));

    repo.add("/inv/removable", device);
    ASSERT_TRUE(repo.inventoryFor(device).has_value());

    repo.remove(device);
    EXPECT_FALSE(repo.inventoryFor(device).has_value());
}

// ===========================================================================
// Group G300: remove() unknown-device branch + MCTPDDevice inline functions
// ===========================================================================

// G300 (removed): remove() of an unknown device invokes UB (dereferences
// end() iterator).  EXPECT_DEATH with Google Test's "fast" fork style runs
// the death-test body under the same Valgrind instance, so Valgrind reports
// the expected crash as an error and the test suite exits non-zero.
// The branch is already exercised indirectly; the explicit death test is
// omitted to keep Valgrind clean.

// G301 (removed): remove() of an unknown device invokes UB (dereferences
// end() iterator).  EXPECT_DEATH with Google Test's "fast" fork style runs
// the death-test body under the same Valgrind instance, so Valgrind reports
// the expected crash as an error and the test suite exits non-zero.
// The branch is already exercised indirectly; the explicit death test is
// omitted to keep Valgrind clean.

// G302: getNameForEid() with a bridge pool where the queried EID is strictly
// below the pool start — exercises the `eid >= *bridgePoolStartEid` false
// branch inside MCTPDDevice::getNameForEid().
TEST(MCTPDeviceRepository, G302getNameForEidBelowBridgePoolStartReturnsNullopt)
{
    MCTPDeviceRepository repo;
    // main EID=20, bridge pool [21,23], deviceNames has entries for pool.
    auto dev = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-pool", "usb0", std::vector<uint8_t>{},
        std::optional<uint8_t>(20), std::optional<uint8_t>(21),
        std::optional<uint8_t>(23), std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"usb-pool", "bridge-1", "bridge-2",
                                 "bridge-3"});

    repo.add("/inv/pool", dev);

    // EID 19 is below bridge pool start (21) and is not the main EID (20)
    // → getNameForEid should return nullopt for EID 19
    EXPECT_FALSE(repo.getNameForEid(19).has_value());

    // EID 20 is the main device EID → should be found
    ASSERT_TRUE(repo.getNameForEid(20).has_value());
    EXPECT_EQ(repo.getNameForEid(20).value_or(""), "usb-pool");

    // EID 21 is pool start → should be found as bridge-1
    ASSERT_TRUE(repo.getNameForEid(21).has_value());
    EXPECT_EQ(repo.getNameForEid(21).value_or(""), "bridge-1");
}

// G303: getNameForEid() with a bridge pool where the queried EID is strictly
// above the pool end — exercises the `eid <= *bridgePoolEndEid` false branch.
TEST(MCTPDeviceRepository, G303getNameForEidAboveBridgePoolEndReturnsNullopt)
{
    MCTPDeviceRepository repo;
    // main EID=30, bridge pool [31,32]
    auto dev = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-pool2", "usb1", std::vector<uint8_t>{},
        std::optional<uint8_t>(30), std::optional<uint8_t>(31),
        std::optional<uint8_t>(32), std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"usb-pool2", "bridge-a", "bridge-b"});

    repo.add("/inv/pool2", dev);

    // EID 33 is above bridge pool end (32) → nullopt
    EXPECT_FALSE(repo.getNameForEid(33).has_value());

    // EID 32 is pool end → should be found
    ASSERT_TRUE(repo.getNameForEid(32).has_value());
    EXPECT_EQ(repo.getNameForEid(32).value_or(""), "bridge-b");
}

// G304: getStaticEidFromInterface() when the device has no staticEID and the
// interface does match — `mctpDevice->getEid()` returns nullopt (no endpoint,
// no staticEID), so the function returns nullopt.
// This exercises the inner `if (mctpDevice && mctpDevice->getInterface() ==
// interface)` true-branch combined with getEid() returning nullopt.
TEST(MCTPDeviceRepository, G304getStaticEidFromInterfaceNoEidDeviceReturnsNull)
{
    MCTPDeviceRepository repo;
    // No staticEID passed — getEid() returns nullopt
    auto dev = std::make_shared<USBMCTPDDevice>(
        nullptr, "usb-noeid", "usb5", std::vector<uint8_t>{}, std::nullopt,
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"usb-noeid"});

    repo.add("/inv/noeid", dev);

    // Interface matches but EID is nullopt → no value returned
    EXPECT_FALSE(repo.getStaticEidFromInterface("usb5").has_value());
}

// G305: getNameForEid() with two MCTPDDevices where the first has a bridge pool
// that does NOT contain the target EID (both main and pool miss), and the
// second device does contain it — exercises the "continue to next device"
// path in getNameForEid().
TEST(MCTPDeviceRepository, G305getNameForEidContinuesAfterBridgePoolMiss)
{
    MCTPDeviceRepository repo;
    // First device: main EID=40, bridge pool [41,42]
    auto first = std::make_shared<USBMCTPDDevice>(
        nullptr, "first", "usb0", std::vector<uint8_t>{},
        std::optional<uint8_t>(40), std::optional<uint8_t>(41),
        std::optional<uint8_t>(42), std::nullopt, std::nullopt, std::nullopt,
        std::vector<std::string>{"first", "f-bridge-1", "f-bridge-2"});
    // Second device: main EID=50 (no bridge pool)
    auto second = std::make_shared<USBMCTPDDevice>(
        nullptr, "second", "usb1", std::vector<uint8_t>{},
        std::optional<uint8_t>(50));

    repo.add("/inv/first", first);
    repo.add("/inv/second", second);

    // EID 50 is not in first's main or bridge range → scan continues to second
    auto name = repo.getNameForEid(50);
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(name.value_or(""), "second");

    // EID 43 is outside both devices' ranges → nullopt
    EXPECT_FALSE(repo.getNameForEid(43).has_value());
}

// Registering the same device under two inventory paths sets its reverse-lookup
// reference count to 2. Removing it once drives removeReverseLookup() down the
// count>1 branch, which calls the private refreshReverseLookup() to re-point
// the device at its still-registered inventory path. This exercises
// refreshReverseLookup(), which is otherwise unreached.
TEST(MCTPDeviceRepository, removeMultiInventoryDeviceRefreshesReverseLookup)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*device, describe())
        .WillRepeatedly(testing::Return("mock device"));

    repo.add("/inv/alpha", device);
    // Same device, different inventory path → reverse-lookup count becomes 2.
    repo.add("/inv/beta", device);
    EXPECT_TRUE(repo.contains(device));

    // First removal takes the count>1 branch → refreshReverseLookup() runs and
    // the device remains, now pointing at the surviving inventory path.
    EXPECT_NO_THROW(repo.remove(device));
    EXPECT_TRUE(repo.contains(device));
    auto inventory = repo.inventoryFor(device);
    ASSERT_TRUE(inventory.has_value());
    EXPECT_EQ(inventory.value_or(""), "/inv/beta");

    // Second removal now takes the count<=1 branch and fully removes it.
    EXPECT_NO_THROW(repo.remove(device));
    EXPECT_FALSE(repo.contains(device));
}

TEST(MCTPDeviceRepository, iteratorsExposeRegisteredInventory)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<MockMCTPDevice>();
    repo.add("/inv/iterated", device);

    auto entry = repo.begin();
    ASSERT_NE(entry, repo.end());
    EXPECT_EQ(entry->first, "/inv/iterated");
    EXPECT_EQ(entry->second, device);
    EXPECT_EQ(++entry, repo.end());
}

TEST(MCTPDeviceRepository, getNameForEidPropagatesDeviceFailure)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<ThrowingNameMCTPDevice>();
    repo.add("/inv/throwing-name", device);

    EXPECT_THROW(repo.getNameForEid(9), std::runtime_error);
}

static std::filesystem::path findRepositoryHeaderForTest()
{
    const auto testSource = std::filesystem::path(__FILE__);
    std::vector<std::filesystem::path> candidates{
        testSource.parent_path().parent_path() / "MCTPDeviceRepository.hpp",
        "src/mctp/MCTPDeviceRepository.hpp",
        "../src/mctp/MCTPDeviceRepository.hpp", "MCTPDeviceRepository.hpp"};

    for (const auto& candidate : candidates)
    {
        if (std::ifstream candidateFile(candidate); candidateFile.good())
        {
            return candidate;
        }
    }

    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    for (int depth = 0; !ec && depth < 8; ++depth)
    {
        auto candidate = cwd / "src/mctp/MCTPDeviceRepository.hpp";
        if (std::ifstream candidateFile(candidate); candidateFile.good())
        {
            return candidate;
        }

        auto parent = cwd.parent_path();
        if (parent == cwd)
        {
            break;
        }
        cwd = parent;
    }

    return candidates.front();
}

TEST(MCTPDeviceRepository, DR09lookupUsesReverseIndexNotLinearSourceScan)
{
    const auto headerPath = findRepositoryHeaderForTest();
    std::ifstream header(headerPath);
    ASSERT_TRUE(header.good()) << "Unable to inspect " << headerPath;

    std::ostringstream buffer;
    buffer << header.rdbuf();
    const auto source = buffer.str();

    const bool hasLegacyLinearLookup =
        source.find("std::ranges::find_if(devices") != std::string::npos ||
        source.find("return it.second == device") != std::string::npos;
    const bool hasReverseLookup =
        source.find("inventoryByDevice.find(device)") != std::string::npos;

    std::cout << "DR-09 repro: inspected \"" << headerPath.string()
              << "\", legacy linear lookup=" << hasLegacyLinearLookup
              << ", reverse index lookup=" << hasReverseLookup << "\n";
    std::cout << "DR-09 repro: old lookup scanned the inventory map for each "
                 "contains(), inventoryFor(), and remove() call\n";

    EXPECT_FALSE(hasLegacyLinearLookup)
        << "DR-09 reproduced: lookup still uses a linear scan over devices.";
    EXPECT_TRUE(hasReverseLookup)
        << "DR-09 reproduced: lookup is not backed by the reverse index.";
}

TEST(MCTPDeviceRepository, DR09largeRepoLifecycleLookupStaysSynchronized)
{
    MCTPDeviceRepository repo;
    std::vector<std::shared_ptr<MockMCTPDevice>> devices;
    devices.reserve(64);

    std::cout << "DR-09 repro: adding 64 MCTP inventory entries\n";
    for (int i = 0; i < 64; ++i)
    {
        auto device = std::make_shared<MockMCTPDevice>();
        devices.push_back(device);
        repo.add("/inv/dr09/device" + std::to_string(i), device);
    }

    auto target = devices.back();
    const std::string targetPath = "/inv/dr09/device63";
    std::cout << "DR-09 repro: resolving last device through contains(), "
                 "inventoryFor(), and remove(); legacy code performed up to "
                 "64 pointer comparisons per lookup\n";
    EXPECT_TRUE(repo.contains(target));
    EXPECT_EQ(repo.inventoryFor(target).value_or(""), targetPath);

    repo.remove(target);
    const bool targetPresent = repo.contains(target);
    const bool firstDeviceStillPresent = repo.contains(devices.front());
    std::cout << "DR-09 repro: removed " << targetPath
              << ", target present=" << targetPresent
              << ", first device still present=" << firstDeviceStillPresent
              << "\n";
    EXPECT_FALSE(targetPresent);
    EXPECT_TRUE(firstDeviceStillPresent);

    auto duplicate = std::make_shared<MockMCTPDevice>();
    repo.add("/inv/dr09/duplicateA", duplicate);
    repo.add("/inv/dr09/duplicateB", duplicate);
    auto selectedPath = repo.inventoryFor(duplicate).value_or("");
    std::cout << "DR-09 repro: registered one device under duplicate "
                 "inventory paths; selected path="
              << selectedPath << "\n";
    EXPECT_EQ(selectedPath, "/inv/dr09/duplicateA");

    repo.remove(duplicate);
    const bool duplicateAPresent =
        repo.deviceFor("/inv/dr09/duplicateA") != nullptr;
    const bool duplicateBPresent =
        repo.deviceFor("/inv/dr09/duplicateB") != nullptr;
    const bool sharedLookupPresent = repo.contains(duplicate);
    std::cout << "DR-09 repro: after first duplicate remove, duplicateA "
                 "present="
              << duplicateAPresent
              << ", duplicateB present=" << duplicateBPresent
              << ", shared lookup present=" << sharedLookupPresent << "\n";
    EXPECT_FALSE(duplicateAPresent);
    EXPECT_TRUE(duplicateBPresent);
    EXPECT_TRUE(sharedLookupPresent);
    EXPECT_EQ(repo.inventoryFor(duplicate).value_or(""),
              "/inv/dr09/duplicateB");
}

class ThrowingDescribeMCTPDevice : public MCTPDevice
{
  public:
    void setup(std::function<void(const std::error_code&,
                                  const std::shared_ptr<MCTPEndpoint>&)>&&
               /*added*/) override
    {}

    void remove() override {}

    std::string describe() const override
    {
        throw std::runtime_error("describe failed");
    }

    std::size_t id() const override
    {
        return 0;
    }
};

TEST(MCTPDeviceRepository, addConflictPropagatesDescribeFailure)
{
    MCTPDeviceRepository repo;
    auto existing = std::make_shared<MockMCTPDevice>();
    auto conflicting = std::make_shared<ThrowingDescribeMCTPDevice>();

    repo.add("/test/inventory", existing);

    EXPECT_THROW(repo.add("/test/inventory", conflicting), std::runtime_error);
    EXPECT_EQ(repo.deviceFor("/test/inventory"), existing);
}

TEST(MCTPDeviceRepository, removeUnknownDeviceThrowsNoSuchDevice)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*device, describe())
        .WillOnce(testing::Return("unknown device"));

    try
    {
        repo.remove(device);
        FAIL() << "Removing an unknown device should throw";
    }
    catch (const std::system_error& error)
    {
        EXPECT_EQ(error.code(),
                  std::make_error_code(std::errc::no_such_device));
    }

    EXPECT_FALSE(repo.contains(device));
}

TEST(MCTPDeviceRepository, removeUnknownPropagatesDescribeFailure)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<ThrowingDescribeMCTPDevice>();

    EXPECT_THROW(repo.remove(device), std::runtime_error);
    EXPECT_FALSE(repo.contains(device));
}

// G552: removing a device registered under multiple inventory paths keeps the
// reverse lookup synchronized with the remaining path.
TEST(MCTPDeviceRepository, G552removeSamePointerDifferentPathKeepsLookupSynced)
{
    MCTPDeviceRepository repo;
    auto device = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*device, describe()).WillRepeatedly(testing::Return("shared"));

    repo.add("/inv/pathA", device);
    repo.add("/inv/pathB", device);

    repo.remove(device);
    EXPECT_FALSE(repo.deviceFor("/inv/pathA"));
    EXPECT_EQ(repo.deviceFor("/inv/pathB").get(), device.get());
    EXPECT_TRUE(repo.contains(device));
    EXPECT_EQ(repo.inventoryFor(device).value_or(""), "/inv/pathB");

    repo.remove(device);
    EXPECT_FALSE(repo.contains(device));
    EXPECT_FALSE(repo.inventoryFor(device).has_value());
    EXPECT_FALSE(repo.deviceFor("/inv/pathB"));
}
