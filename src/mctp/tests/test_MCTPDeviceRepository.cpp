#include "MCTPDeviceRepository.hpp"
#include "MCTPEndpoint.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
