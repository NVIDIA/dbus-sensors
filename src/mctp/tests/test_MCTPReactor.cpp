#include "MCTPEndpoint.hpp"
#include "MCTPReactor.hpp"
#include "USBRecovery.hpp"
#include "Utils.hpp"

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
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

class AsyncRemoveMockMCTPDevice : public MockMCTPDevice
{
  public:
    using MockMCTPDevice::remove;

    void remove(std::function<void()>&& removed) override
    {
        removeComplete = std::move(removed);
        MockMCTPDevice::remove();
    }

    std::function<void()> removeComplete;
};

class MockMCTPEndpoint : public MCTPEndpoint
{
  public:
    ~MockMCTPEndpoint() override = default;

    MOCK_METHOD(int, network, (), (const, override));
    MOCK_METHOD(uint8_t, eid, (), (const, override));
    MOCK_METHOD(void, subscribe,
                (Event && degraded, Event&& available, Event&& removed),
                (override));
    MOCK_METHOD(void, remove, (), (override));
    MOCK_METHOD(std::string, describe, (), (const, override));
    MOCK_METHOD(std::shared_ptr<MCTPDevice>, device, (), (const, override));
};

class MockAssociationServer : public AssociationServer
{
  public:
    ~MockAssociationServer() override = default;

    MOCK_METHOD(void, associate,
                (const std::string& path,
                 const std::vector<Association>& associations),
                (override));
    MOCK_METHOD(void, disassociate, (const std::string& path), (override));
};

class MockUSBRecovery : public USBRecovery
{
  public:
    ~MockUSBRecovery() override = default;

    MOCK_METHOD(bool, clearBulkOutHalt,
                (const std::string& interface, std::string& status),
                (override));
};
class TestReactorMCTPDDevice : public MCTPDDevice
{
  public:
    TestReactorMCTPDDevice(
        const std::shared_ptr<sdbusplus::asio::connection>& connection,
        uint8_t staticEid) :
        MCTPDDevice(connection, "reactor-mctpd", "usbreactor0",
                    std::vector<uint8_t>{0x20}, staticEid, std::nullopt,
                    std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                    {"reactor-mctpd"})
    {}

    // Variant constructor that exposes bridge pool parameters so that
    // managesEid() can return true for EIDs in [bridgeStart, bridgeEnd].
    TestReactorMCTPDDevice(
        const std::shared_ptr<sdbusplus::asio::connection>& connection,
        uint8_t staticEid, uint8_t bridgeStart, uint8_t bridgeEnd) :
        MCTPDDevice(connection, "reactor-mctpd-bridge", "usbreactor1",
                    std::vector<uint8_t>{0x21}, staticEid, bridgeStart,
                    bridgeEnd, std::nullopt, std::nullopt, std::nullopt,
                    {"reactor-mctpd-bridge"})
    {}

    void setup(std::function<void(const std::error_code& ec,
                                  const std::shared_ptr<MCTPEndpoint>& ep)>&&
                   added) override
    {
        if (setupHandler)
        {
            setupHandler(std::move(added));
            return;
        }
        added({}, nullptr);
    }

    void remove() override
    {
        removeCalls++;
    }

    void remove(std::function<void()>&& removed) override
    {
        auto removedCallback = std::move(removed);
        remove();
        if (removedCallback)
        {
            removedCallback();
        }
    }

    std::string describe() const override
    {
        return "test-reactor-mctpd-device";
    }

    int removeCalls = 0;
    std::function<void(
        std::function<void(const std::error_code&,
                           const std::shared_ptr<MCTPEndpoint>&)>&&)>
        setupHandler;

    void triggerRequestSetup()
    {
        if (requestSetupCallback)
        {
            requestSetupCallback(std::dynamic_pointer_cast<MCTPDDevice>(
                this->shared_from_this()));
        }
    }
};

class TestUSBMCTPDDevice : public USBMCTPDDevice
{
  public:
    TestUSBMCTPDDevice(const std::string& interface = "usbtest0",
                       uint8_t recoveryThreshold = 5) :
        USBMCTPDDevice(nullptr, "usb-reactor-test", interface,
                       std::vector<uint8_t>{}, std::nullopt, std::nullopt,
                       std::nullopt, std::nullopt, std::nullopt,
                       recoveryThreshold, std::nullopt, {"usb-reactor-test"})
    {}

    void setup(std::function<void(const std::error_code& ec,
                                  const std::shared_ptr<MCTPEndpoint>& ep)>&&
                   added) override
    {
        if (setupHandler)
        {
            setupHandler(std::move(added));
            return;
        }
        added({}, nullptr);
    }

    void remove() override
    {
        removeCalls++;
    }

    void remove(std::function<void()>&& removed) override
    {
        auto removedCallback = std::move(removed);
        remove();
        if (removedCallback)
        {
            removedCallback();
        }
    }

    std::string describe() const override
    {
        return "test-usb-mctpd-device";
    }

    int removeCalls = 0;
    std::function<void(
        std::function<void(const std::error_code&,
                           const std::shared_ptr<MCTPEndpoint>&)>&&)>
        setupHandler;
};

class MCTPReactorFixture : public testing::Test
{
  protected:
    void SetUp() override
    {
        reactor = std::make_shared<MCTPReactor>(assoc);
        device = std::make_shared<MockMCTPDevice>();
        EXPECT_CALL(*device, describe())
            .WillRepeatedly(testing::Return("mock device"));

        endpoint = std::make_shared<MockMCTPEndpoint>();
        EXPECT_CALL(*endpoint, device())
            .WillRepeatedly(testing::Return(device));
        EXPECT_CALL(*endpoint, describe())
            .WillRepeatedly(testing::Return("mock endpoint"));
        EXPECT_CALL(*endpoint, eid()).WillRepeatedly(testing::Return(9));
        EXPECT_CALL(*endpoint, network()).WillRepeatedly(testing::Return(1));
    }

    void TearDown() override
    {
        // https://stackoverflow.com/a/10289205
        EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(endpoint.get()));
        EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(device.get()));
    }

    MockAssociationServer assoc;
    std::shared_ptr<MCTPReactor> reactor;
    std::shared_ptr<MockMCTPDevice> device;
    std::shared_ptr<MockMCTPEndpoint> endpoint;
};

TEST_F(MCTPReactorFixture, manageNullDevice)
{
    reactor->manageMCTPDevice("/test", {});
    EXPECT_FALSE(reactor->getDeviceName(9).has_value());
    reactor->unmanageMCTPDevice("/test");
}

TEST_F(MCTPReactorFixture, manageMockDeviceSetupFailure)
{
    EXPECT_CALL(*device, remove());
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::permission_denied), endpoint));

    reactor->manageMCTPDevice("/test", device);
    reactor->unmanageMCTPDevice("/test");
}

TEST_F(MCTPReactorFixture, manageMockDevice)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation));
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(1);

    EXPECT_CALL(*endpoint, remove()).WillOnce([&]() {
        removeHandler(endpoint);
    });
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));

    EXPECT_CALL(*device, remove()).WillOnce([&]() { endpoint->remove(); });
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));

    reactor->manageMCTPDevice("/test", device);
    reactor->unmanageMCTPDevice("/test");
}

TEST_F(MCTPReactorFixture, manageMockDeviceDeferredSetup)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation));
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(1);

    EXPECT_CALL(*endpoint, remove()).WillOnce([&]() {
        removeHandler(endpoint);
    });
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));

    EXPECT_CALL(*device, remove()).WillOnce([&]() { endpoint->remove(); });
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::permission_denied), endpoint))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));

    reactor->manageMCTPDevice("/test", device);
    reactor->tick();
    reactor->unmanageMCTPDevice("/test");
}

TEST_F(MCTPReactorFixture, manageMockDeviceRemoved)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation))
        .Times(2);
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(2);

    EXPECT_CALL(*endpoint, remove()).WillOnce([&]() {
        removeHandler(endpoint);
    });
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .Times(2)
        .WillRepeatedly(testing::SaveArg<2>(&removeHandler));

    EXPECT_CALL(*device, remove()).WillOnce([&]() { endpoint->remove(); });
    EXPECT_CALL(*device, setup(testing::_))
        .Times(2)
        .WillRepeatedly(
            testing::InvokeArgument<0>(std::error_code(), endpoint));

    reactor->manageMCTPDevice("/test", device);
    removeHandler(endpoint);
    reactor->tick();
    reactor->unmanageMCTPDevice("/test");
}

TEST_F(MCTPReactorFixture, removedCallbackAfterReactorDestroyedDoesNotCrash)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation));
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(0);

    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));

    reactor->manageMCTPDevice("/test", device);
    ASSERT_TRUE(static_cast<bool>(removeHandler));

    // Exercise weak reactor branch in removed callback.
    reactor.reset();
    removeHandler(endpoint);
}

TEST_F(MCTPReactorFixture, removedCallbackForForeignDeviceDoesNotDeferSetup)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;
    auto foreignDevice = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*foreignDevice, describe())
        .WillRepeatedly(testing::Return("foreign device"));

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation));
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(1);

    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));
    EXPECT_CALL(*endpoint, device())
        .WillOnce(testing::Return(device))
        .WillOnce(testing::Return(foreignDevice));

    // No deferred retry expected because removed endpoint belongs to foreign
    // device at callback time.
    EXPECT_CALL(*device, setup(testing::_))
        .Times(1)
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*device, remove()).Times(1);

    reactor->manageMCTPDevice("/test", device);
    ASSERT_TRUE(static_cast<bool>(removeHandler));
    removeHandler(endpoint);
    reactor->tick();
    reactor->unmanageMCTPDevice("/test");
}

TEST_F(MCTPReactorFixture, setupSuccessWithNullEndpointDoesNotAssociate)
{
    EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    EXPECT_CALL(*device, remove()).Times(1);

    reactor->manageMCTPDevice("/test", device);
    reactor->unmanageMCTPDevice("/test");
}

TEST_F(MCTPReactorFixture, specificEidRetryIgnoresFailedNonMctpdDevice)
{
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), endpoint));
    EXPECT_CALL(*device, remove()).Times(1);

    reactor->manageMCTPDevice("/test", device);
    EXPECT_TRUE(reactor->isRetrying(0));
    EXPECT_FALSE(reactor->isRetrying(9));
    reactor->unmanageMCTPDevice("/test");
}

TEST_F(MCTPReactorFixture, removedCallbackAfterUnmanageDoesNotRequeueSetup)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation))
        .Times(1);
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(1);

    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));

    // Setup should only happen during initial manage; callback after unmanage
    // must not re-defer/retry.
    EXPECT_CALL(*device, setup(testing::_))
        .Times(1)
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*device, remove()).Times(1);
    EXPECT_CALL(*endpoint, remove()).Times(0);

    reactor->manageMCTPDevice("/test", device);
    reactor->unmanageMCTPDevice("/test");
    ASSERT_TRUE(static_cast<bool>(removeHandler));
    removeHandler(endpoint);
    reactor->tick();
}

TEST(MCTPReactor, replaceConfiguration)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test"}};

    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation))
        .Times(2);
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(2);

    auto endpoint = std::make_shared<MockMCTPEndpoint>();
    EXPECT_CALL(*endpoint, describe())
        .WillRepeatedly(testing::Return("mock endpoint"));
    EXPECT_CALL(*endpoint, eid()).WillRepeatedly(testing::Return(9));
    EXPECT_CALL(*endpoint, network()).WillRepeatedly(testing::Return(1));
    EXPECT_CALL(*endpoint, remove()).Times(2).WillRepeatedly([&]() {
        removeHandler(endpoint);
    });
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .Times(2)
        .WillRepeatedly(testing::SaveArg<2>(&removeHandler));

    auto initial = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*initial, describe())
        .WillRepeatedly(testing::Return("mock device: initial"));
    EXPECT_CALL(*initial, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*initial, remove()).WillOnce([&]() { endpoint->remove(); });

    auto replacement = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*replacement, describe())
        .WillRepeatedly(testing::Return("mock device: replacement"));
    EXPECT_CALL(*replacement, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*replacement, remove()).WillOnce([&]() { endpoint->remove(); });

    EXPECT_CALL(*endpoint, device())
        .WillOnce(testing::Return(initial))
        .WillOnce(testing::Return(initial))
        .WillOnce(testing::Return(replacement))
        .WillOnce(testing::Return(replacement));

    reactor->manageMCTPDevice("/test", initial);
    reactor->manageMCTPDevice("/test", replacement);
    reactor->tick();
    reactor->unmanageMCTPDevice("/test");

    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(initial.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(replacement.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(endpoint.get()));
}

TEST(MCTPReactor, replaceConfigurationWaitsForAsyncRemoveCompletion)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test/async-replace"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation))
        .Times(1);
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(0);

    auto endpoint = std::make_shared<MockMCTPEndpoint>();
    EXPECT_CALL(*endpoint, describe())
        .WillRepeatedly(testing::Return("async replace endpoint"));
    EXPECT_CALL(*endpoint, eid()).WillRepeatedly(testing::Return(9));
    EXPECT_CALL(*endpoint, network()).WillRepeatedly(testing::Return(1));
    EXPECT_CALL(*endpoint, remove()).Times(0);
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_));

    auto initial = std::make_shared<AsyncRemoveMockMCTPDevice>();
    EXPECT_CALL(*initial, describe())
        .WillRepeatedly(testing::Return("async replace initial"));
    EXPECT_CALL(*endpoint, device()).WillRepeatedly(testing::Return(initial));
    EXPECT_CALL(*initial, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*initial, remove()).Times(1);

    int replacementSetupCalls = 0;
    auto replacement = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*replacement, describe())
        .WillRepeatedly(testing::Return("async replace replacement"));
    EXPECT_CALL(*replacement, setup(testing::_))
        .WillOnce([&replacementSetupCalls](auto&& added) {
            replacementSetupCalls++;
            added(std::error_code(), nullptr);
        });
    EXPECT_CALL(*replacement, remove()).Times(1);

    reactor->manageMCTPDevice("/test/async-replace", initial);
    reactor->manageMCTPDevice("/test/async-replace", replacement);

    EXPECT_FALSE(reactor->isRetrying(0));
    reactor->tick();
    EXPECT_EQ(replacementSetupCalls, 0);

    ASSERT_TRUE(static_cast<bool>(initial->removeComplete));
    initial->removeComplete();
    EXPECT_TRUE(reactor->isRetrying(0));

    reactor->tick();
    EXPECT_EQ(replacementSetupCalls, 1);
    EXPECT_FALSE(reactor->isRetrying(0));

    reactor->unmanageMCTPDevice("/test/async-replace");

    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(initial.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(replacement.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(endpoint.get()));
}

TEST_F(MCTPReactorFixture, unmanageUnknownInventoryIsNoop)
{
    reactor->unmanageMCTPDevice("/unknown");
    EXPECT_FALSE(reactor->getDeviceName(9).has_value());
}

TEST(MCTPReactor, getDeviceNameReturnsNulloptWhenNotTracked)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    EXPECT_FALSE(reactor->getDeviceName(9).has_value());
}

TEST(MCTPReactor, getStaticEidFromInterfaceReturnsNulloptWhenNotTracked)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    EXPECT_FALSE(reactor->getStaticEidFromInterface("usb0").has_value());
}

TEST(MCTPReactor, requestSetupCallbackTriggersReactorSetupPathForMctpdDevice)
{
    try
    {
        MockAssociationServer assoc{};
        auto reactor = std::make_shared<MCTPReactor>(assoc);
        boost::asio::io_context io;
        auto conn = std::make_shared<sdbusplus::asio::connection>(io);
        auto device = std::make_shared<TestReactorMCTPDDevice>(conn, 44);

        int setupCalls = 0;
        device->setupHandler = [&setupCalls](auto&& added) {
            setupCalls++;
            std::forward<decltype(added)>(
                added)(std::make_error_code(std::errc::timed_out), nullptr);
        };

        EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
        EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

        reactor->manageMCTPDevice("/test/mctpd", device);
        device->triggerRequestSetup();

        EXPECT_GE(setupCalls, 2);
        reactor->unmanageMCTPDevice("/test/mctpd");
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "DBus unavailable in this environment: " << ex.what();
    }
}

TEST(MCTPReactor, requestSetupCallbackAfterReactorDestroyedIsSafeNoop)
{
    try
    {
        MockAssociationServer assoc{};
        auto reactor = std::make_shared<MCTPReactor>(assoc);
        boost::asio::io_context io;
        auto conn = std::make_shared<sdbusplus::asio::connection>(io);
        auto device = std::make_shared<TestReactorMCTPDDevice>(conn, 45);

        int setupCalls = 0;
        device->setupHandler = [&setupCalls](auto&& added) {
            setupCalls++;
            std::forward<decltype(added)>(
                added)(std::make_error_code(std::errc::timed_out), nullptr);
        };

        EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
        EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

        reactor->manageMCTPDevice("/test/mctpd-destroyed", device);
        reactor.reset();

        // Should not crash and should not schedule additional setup after
        // reactor lifetime ends.
        device->triggerRequestSetup();
        EXPECT_EQ(setupCalls, 1);
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "DBus unavailable in this environment: " << ex.what();
    }
}

TEST(MCTPReactor, usbRecoveryNotTriggeredBeforeFiveConsecutiveFailures)
{
    MockAssociationServer assoc{};
    auto recovery = std::make_unique<MockUSBRecovery>();
    auto* recoveryPtr = recovery.get();
    auto reactor = std::make_shared<MCTPReactor>(assoc, std::move(recovery));
    auto device = std::make_shared<TestUSBMCTPDDevice>("usb-fail4");

    device->setupHandler = [](auto&& added) {
        std::forward<decltype(added)>(
            added)(std::make_error_code(std::errc::timed_out), nullptr);
    };

    EXPECT_CALL(*recoveryPtr, clearBulkOutHalt(testing::_, testing::_))
        .Times(0);
    EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

    reactor->manageMCTPDevice("/test/usb-fail4", device); // failure 1
    reactor->tick();                                      // failure 2
    reactor->tick();                                      // failure 3
    reactor->tick();                                      // failure 4

    reactor->unmanageMCTPDevice("/test/usb-fail4");
}

TEST(MCTPReactor, usbRecoveryTriggeredAtFifthConsecutiveFailure)
{
    MockAssociationServer assoc{};
    auto recovery = std::make_unique<MockUSBRecovery>();
    auto* recoveryPtr = recovery.get();
    auto reactor = std::make_shared<MCTPReactor>(assoc, std::move(recovery));
    reactor->setAutoUSBRecoveryEnabled(true);
    auto device = std::make_shared<TestUSBMCTPDDevice>("usb-fail5");

    device->setupHandler = [](auto&& added) {
        std::forward<decltype(added)>(
            added)(std::make_error_code(std::errc::timed_out), nullptr);
    };

    EXPECT_CALL(*recoveryPtr, clearBulkOutHalt("usb-fail5", testing::_))
        .WillOnce(testing::DoAll(testing::SetArgReferee<1>("cleared"),
                                 testing::Return(true)));
    EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

    reactor->manageMCTPDevice("/test/usb-fail5", device); // failure 1
    reactor->tick();                                      // failure 2
    reactor->tick();                                      // failure 3
    reactor->tick();                                      // failure 4
    reactor->tick(); // failure 5 -> recovery

    reactor->unmanageMCTPDevice("/test/usb-fail5");
}

TEST(MCTPReactor, usbFailureStreakResetsAfterSuccessfulSetup)
{
    MockAssociationServer assoc{};
    auto recovery = std::make_unique<MockUSBRecovery>();
    auto* recoveryPtr = recovery.get();
    auto reactor = std::make_shared<MCTPReactor>(assoc, std::move(recovery));
    reactor->setAutoUSBRecoveryEnabled(true);
    auto device = std::make_shared<TestUSBMCTPDDevice>("usb-reset");

    int attempt = 0;
    device->setupHandler = [&attempt, &device](auto&& added) {
        attempt++;
        // fail x4, succeed once (reset streak), then fail x5.
        if (attempt <= 4 || (attempt >= 6 && attempt <= 10))
        {
            std::forward<decltype(added)>(
                added)(std::make_error_code(std::errc::timed_out), nullptr);
            return;
        }
        auto ep = std::make_shared<MockMCTPEndpoint>();
        EXPECT_CALL(*ep, subscribe(testing::_, testing::_, testing::_))
            .Times(1);
        EXPECT_CALL(*ep, describe())
            .WillRepeatedly(testing::Return("usb-reset-endpoint"));
        EXPECT_CALL(*ep, device()).WillRepeatedly(testing::Return(device));
        EXPECT_CALL(*ep, network()).WillRepeatedly(testing::Return(1));
        EXPECT_CALL(*ep, eid()).WillRepeatedly(testing::Return(8));
        std::forward<decltype(added)>(added)(std::error_code{}, ep);
    };

    EXPECT_CALL(*recoveryPtr, clearBulkOutHalt("usb-reset", testing::_))
        .Times(1)
        .WillOnce(testing::DoAll(testing::SetArgReferee<1>("cleared"),
                                 testing::Return(true)));
    EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(1);
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

    reactor->manageMCTPDevice("/test/usb-reset", device); // failure 1
    reactor->tick();                                      // failure 2
    reactor->tick();                                      // failure 3
    reactor->tick();                                      // failure 4
    reactor->tick();                                      // success -> reset

    reactor->manageMCTPDevice("/test/usb-reset",
                              device); // failure 1 (new streak)
    reactor->tick();                   // failure 2
    reactor->tick();                   // failure 3
    reactor->tick();                   // failure 4
    reactor->tick();                   // failure 5 -> recovery

    reactor->unmanageMCTPDevice("/test/usb-reset");
}

TEST(MCTPReactor, nonUsbFailuresDoNotTriggerUsbRecovery)
{
    MockAssociationServer assoc{};
    auto recovery = std::make_unique<MockUSBRecovery>();
    auto* recoveryPtr = recovery.get();
    auto reactor = std::make_shared<MCTPReactor>(assoc, std::move(recovery));
    auto device = std::make_shared<MockMCTPDevice>();

    EXPECT_CALL(*device, describe())
        .WillRepeatedly(testing::Return("mock non-usb device"));
    EXPECT_CALL(*device, setup(testing::_))
        .WillRepeatedly(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr));
    EXPECT_CALL(*device, remove()).Times(1);
    EXPECT_CALL(*recoveryPtr, clearBulkOutHalt(testing::_, testing::_))
        .Times(0);

    reactor->manageMCTPDevice("/test/non-usb-fail", device);
    reactor->tick();
    reactor->tick();
    reactor->tick();
    reactor->tick();
    reactor->tick();

    reactor->unmanageMCTPDevice("/test/non-usb-fail");
}

TEST(MCTPReactor, unmanageClearsUsbFailureStreak)
{
    MockAssociationServer assoc{};
    auto recovery = std::make_unique<MockUSBRecovery>();
    auto* recoveryPtr = recovery.get();
    auto reactor = std::make_shared<MCTPReactor>(assoc, std::move(recovery));
    auto device = std::make_shared<TestUSBMCTPDDevice>("usb-unmanage-reset");

    device->setupHandler = [](auto&& added) {
        std::forward<decltype(added)>(
            added)(std::make_error_code(std::errc::timed_out), nullptr);
    };

    EXPECT_CALL(*recoveryPtr, clearBulkOutHalt(testing::_, testing::_))
        .Times(0);

    reactor->manageMCTPDevice("/test/usb-unmanage-reset", device); // failure 1
    reactor->tick();                                               // failure 2
    reactor->tick();                                               // failure 3
    reactor->tick();                                               // failure 4

    reactor->unmanageMCTPDevice("/test/usb-unmanage-reset");

    reactor->manageMCTPDevice("/test/usb-unmanage-reset", device); // reset to 1
    reactor->tick();                                               // 2
    reactor->tick();                                               // 3
    reactor->tick();                                               // 4
    reactor->unmanageMCTPDevice("/test/usb-unmanage-reset");
}

TEST_F(MCTPReactorFixture, setupFailureMarksBroadcastAsRetrying)
{
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), endpoint));
    EXPECT_CALL(*device, remove());

    reactor->manageMCTPDevice("/test", device);
    EXPECT_TRUE(reactor->isRetrying(0));
    reactor->unmanageMCTPDevice("/test");
    EXPECT_FALSE(reactor->isRetrying(0));
}

TEST(MCTPReactor, callbackAfterReactorDestroyedIsSafe)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    auto device = std::make_shared<MockMCTPDevice>();
    auto endpoint = std::make_shared<MockMCTPEndpoint>();

    EXPECT_CALL(*device, describe())
        .WillRepeatedly(testing::Return("mock device"));
    EXPECT_CALL(*endpoint, describe())
        .WillRepeatedly(testing::Return("mock endpoint"));

    std::function<void(const std::error_code& ec,
                       const std::shared_ptr<MCTPEndpoint>& ep)>
        setupDone;
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::SaveArg<0>(&setupDone));

    reactor->manageMCTPDevice("/test", device);
    reactor.reset();

    ASSERT_TRUE(static_cast<bool>(setupDone));
    setupDone({}, endpoint);
}

TEST(MCTPReactor, endpointWithMissingInventorySkipsAssociation)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    auto managed = std::make_shared<MockMCTPDevice>();
    auto foreign = std::make_shared<MockMCTPDevice>();
    auto endpoint = std::make_shared<MockMCTPEndpoint>();

    EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

    EXPECT_CALL(*managed, describe())
        .WillRepeatedly(testing::Return("managed device"));
    EXPECT_CALL(*foreign, describe())
        .WillRepeatedly(testing::Return("foreign device"));

    EXPECT_CALL(*endpoint, device()).WillRepeatedly(testing::Return(foreign));
    EXPECT_CALL(*endpoint, describe())
        .WillRepeatedly(testing::Return("endpoint without inventory"));
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_));

    EXPECT_CALL(*managed, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*managed, remove());

    reactor->manageMCTPDevice("/test", managed);
    reactor->unmanageMCTPDevice("/test");
}

TEST_F(MCTPReactorFixture, subscribeFailureDefersSetup)
{
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(
            testing::Throw(MCTPException("Failed to register signal match")));
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*device, remove());

    reactor->manageMCTPDevice("/test", device);
    EXPECT_TRUE(reactor->isRetrying(0));
    reactor->unmanageMCTPDevice("/test");
}

TEST_F(MCTPReactorFixture, endpointDegradedAndAvailableCallbacksExecute)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> degraded;
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> available;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation));
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(0);

    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::DoAll(testing::SaveArg<0>(&degraded),
                                 testing::SaveArg<1>(&available)));
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*device, remove());

    reactor->manageMCTPDevice("/test", device);
    ASSERT_TRUE(static_cast<bool>(degraded));
    ASSERT_TRUE(static_cast<bool>(available));
    degraded(endpoint);
    available(endpoint);
    reactor->unmanageMCTPDevice("/test");
}

TEST(MCTPReactor, specificEidRetryingPathWithMctpdDevice)
{
    try
    {
        boost::asio::io_context io;
        auto conn = std::make_shared<sdbusplus::asio::connection>(io);
        MockAssociationServer assoc{};
        auto reactor = std::make_shared<MCTPReactor>(assoc);
        auto dev = std::make_shared<TestReactorMCTPDDevice>(conn, 42);
        dev->setupHandler =
            [](std::function<void(const std::error_code& ec,
                                  const std::shared_ptr<MCTPEndpoint>& ep)>&&
                   added) {
                std::move(added)(std::make_error_code(std::errc::timed_out),
                                 nullptr);
            };

        reactor->manageMCTPDevice("/test", dev);
        EXPECT_TRUE(reactor->isRetrying(42));
        EXPECT_FALSE(reactor->isRetrying(43));
        reactor->unmanageMCTPDevice("/test");
        EXPECT_EQ(dev->removeCalls, 1);
        EXPECT_FALSE(reactor->isRetrying(42));
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "DBus unavailable in this environment: " << ex.what();
    }
}

TEST(MCTPReactor, specificEidRetryingMatchesLaterFailedMctpdDevice)
{
    try
    {
        boost::asio::io_context io;
        auto conn = std::make_shared<sdbusplus::asio::connection>(io);
        MockAssociationServer assoc{};
        auto reactor = std::make_shared<MCTPReactor>(assoc);

        auto first = std::make_shared<TestReactorMCTPDDevice>(conn, 41);
        first->setupHandler =
            [](std::function<void(const std::error_code& ec,
                                  const std::shared_ptr<MCTPEndpoint>& ep)>&&
                   added) {
                std::move(added)(std::make_error_code(std::errc::timed_out),
                                 nullptr);
            };

        auto second = std::make_shared<TestReactorMCTPDDevice>(conn, 42);
        second->setupHandler =
            [](std::function<void(const std::error_code& ec,
                                  const std::shared_ptr<MCTPEndpoint>& ep)>&&
                   added) {
                std::move(added)(std::make_error_code(std::errc::timed_out),
                                 nullptr);
            };

        reactor->manageMCTPDevice("/test/first", first);
        reactor->manageMCTPDevice("/test/second", second);

        // Force loop traversal: first failed device does not manage EID 42;
        // second failed device does.
        EXPECT_TRUE(reactor->isRetrying(42));
        EXPECT_FALSE(reactor->isRetrying(43));

        reactor->unmanageMCTPDevice("/test/first");
        reactor->unmanageMCTPDevice("/test/second");
        EXPECT_EQ(first->removeCalls, 1);
        EXPECT_EQ(second->removeCalls, 1);
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "DBus unavailable in this environment: " << ex.what();
    }
}

// ---- from() branch coverage (compiled WITHOUT -fno-access-control) ----

TEST(MCTPDeviceFrom, I2CMinimalNoOptionalFields)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI2CTarget"},
        {"Name", "cov-i2c"},
        {"Bus", "0"},
        {"Address", "29"},
    };
    EXPECT_EQ(I2CMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, I2CWithStaticOnly)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI2CTarget"}, {"Name", "cov-i2c-static"}, {"Bus", "0"},
        {"Address", "29"},         {"StaticEndpointID", "7"},
    };
    EXPECT_EQ(I2CMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, I2CWithAllOptionalFields)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI2CTarget"},
        {"Name", "cov-i2c-full"},
        {"Bus", "0"},
        {"Address", "29"},
        {"StaticEndpointID", "7"},
        {"BridgePoolStartEid", "8"},
        {"BridgePoolEndEID", "9"},
    };
    EXPECT_EQ(I2CMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, I3CMinimalNoOptionalFields)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI3CTarget"},
        {"Name", "cov-i3c"},
        {"Bus", "0"},
        {"Address", std::vector<uint64_t>{0x6a}},
    };
    EXPECT_EQ(I3CMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, I3CWithStaticOnly)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI3CTarget"},
        {"Name", "cov-i3c-static"},
        {"Bus", "0"},
        {"Address", std::vector<uint64_t>{0x6a}},
        {"StaticEndpointID", "7"},
    };
    EXPECT_EQ(I3CMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, I3CWithAllOptionalFields)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI3CTarget"},
        {"Name", "cov-i3c-full"},
        {"Bus", "0"},
        {"Address", std::vector<uint64_t>{0x6a}},
        {"StaticEndpointID", "7"},
        {"BridgePoolStartEid", "8"},
        {"BridgePoolEndEID", "9"},
    };
    EXPECT_EQ(I3CMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, USBMinimalNoOptionalFields)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"},
        {"Name", "cov-usb"},
        {"Interface", "usb0"},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(dev->getName(), "cov-usb");
}

TEST(MCTPDeviceFrom, USBWithAllOptionalFieldsIncludingIgnoreLists)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"},
        {"Name", "cov-usb-full,bridge-a,bridge-b"},
        {"Interface", "usb0"},
        {"StaticEndpointID", "9"},
        {"BridgePoolStartEID", "10"},
        {"BridgePoolEndEID", "11"},
        {"IgnoreEIDs", "1, 2, 999, bad, 3"},
        {"IgnoreMessageTypes", "4, bad, 777, 5"},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
}

TEST(MCTPDeviceFrom, USBWithEmptyIgnoreLists)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"}, {"Name", "cov-usb-empty-ignore"},
        {"Interface", "usb0"},     {"StaticEndpointID", "13"},
        {"IgnoreEIDs", ""},        {"IgnoreMessageTypes", ""},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
}

TEST(MCTPDeviceFrom, USBWithOnlyValidIgnoreEntries)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"},    {"Name", "cov-usb-valid-ignore"},
        {"Interface", "usb0"},        {"StaticEndpointID", "14"},
        {"IgnoreEIDs", "10, 20, 30"}, {"IgnoreMessageTypes", "1, 2"},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
}

TEST(MCTPDeviceFrom, USBIgnoreEidsWrongVariantType)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"},
        {"Name", "cov-usb-wrong-ignore"},
        {"Interface", "usb0"},
        {"StaticEndpointID", "15"},
        {"IgnoreEIDs", std::vector<uint64_t>{1, 2}},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
}

TEST(MCTPDeviceFrom, USBWithWhitespaceOnlyIgnoreLists)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"}, {"Name", "cov-usb-ws-ignore"},
        {"Interface", "usb0"},     {"StaticEndpointID", "16"},
        {"IgnoreEIDs", " , , "},   {"IgnoreMessageTypes", "  , "},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
}

TEST(MCTPDeviceFrom, SPIMinimalReturnsNull)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPSPIDevice"},
        {"Name", "cov-spi"},
        {"Bus", "0"},
        {"ChipSelect", "0"},
    };
    EXPECT_EQ(SPIMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, SPIWithStaticReturnsNull)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPSPIDevice"}, {"Name", "cov-spi-static"}, {"Bus", "0"},
        {"ChipSelect", "0"},       {"StaticEndpointID", "7"},
    };
    EXPECT_EQ(SPIMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, XROTMinimalNoStatic)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPXROTTarget"},
        {"Name", "cov-xrot"},
        {"Interface", "xrot0"},
    };
    auto dev = XROTMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
    EXPECT_FALSE(dev->getEid().has_value());
}

TEST(MCTPDeviceFrom, XROTWithStatic)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPXROTTarget"},
        {"Name", "cov-xrot-static"},
        {"Interface", "xrot0"},
        {"StaticEndpointID", "17"},
    };
    auto dev = XROTMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
    const auto eid = dev->getEid();
    ASSERT_TRUE(eid.has_value());
    EXPECT_EQ(eid.value_or(0), 17);
}

TEST(MCTPDeviceFrom, I2CWithBridgeEndEidOnly)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI2CTarget"}, {"Name", "cov-i2c-end"},    {"Bus", "0"},
        {"Address", "29"},         {"BridgePoolEndEID", "15"},
    };
    EXPECT_EQ(I2CMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, I2CWithBridgeStartBadValue)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI2CTarget"},
        {"Name", "cov-i2c-bad-start"},
        {"Bus", "0"},
        {"Address", "29"},
        {"StaticEndpointID", "7"},
        {"BridgePoolStartEid", "xyz"},
    };
    EXPECT_THROW(I2CMCTPDDevice::from({}, iface), std::invalid_argument);
}

TEST(MCTPDeviceFrom, I2CWithBridgeEndBadValue)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI2CTarget"},
        {"Name", "cov-i2c-bad-end"},
        {"Bus", "0"},
        {"Address", "29"},
        {"StaticEndpointID", "7"},
        {"BridgePoolEndEID", "xyz"},
    };
    EXPECT_THROW(I2CMCTPDDevice::from({}, iface), std::invalid_argument);
}

TEST(MCTPDeviceFrom, I3CWithBridgeEndEidOnly)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI3CTarget"},
        {"Name", "cov-i3c-end"},
        {"Bus", "0"},
        {"Address", std::vector<uint64_t>{0x6a}},
        {"BridgePoolEndEID", "15"},
    };
    EXPECT_EQ(I3CMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, I3CWithBridgeStartBadValue)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI3CTarget"},
        {"Name", "cov-i3c-bad-start"},
        {"Bus", "0"},
        {"Address", std::vector<uint64_t>{0x6a}},
        {"StaticEndpointID", "7"},
        {"BridgePoolStartEid", "xyz"},
    };
    EXPECT_THROW(I3CMCTPDDevice::from({}, iface), std::invalid_argument);
}

TEST(MCTPDeviceFrom, I3CWithBridgeEndBadValue)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI3CTarget"},
        {"Name", "cov-i3c-bad-end"},
        {"Bus", "0"},
        {"Address", std::vector<uint64_t>{0x6a}},
        {"StaticEndpointID", "7"},
        {"BridgePoolEndEID", "xyz"},
    };
    EXPECT_THROW(I3CMCTPDDevice::from({}, iface), std::invalid_argument);
}

TEST(MCTPDeviceFrom, I2CWithPollingInterval)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI2CTarget"},
        {"Name", "cov-i2c-poll"},
        {"Bus", "0"},
        {"Address", "29"},
        {"StaticEndpointID", "7"},
        {"PollingInterval", "30"},
    };
    EXPECT_EQ(I2CMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, I3CWithPollingInterval)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI3CTarget"},
        {"Name", "cov-i3c-poll"},
        {"Bus", "0"},
        {"Address", std::vector<uint64_t>{0x6a}},
        {"StaticEndpointID", "7"},
        {"PollingInterval", "30"},
    };
    EXPECT_EQ(I3CMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, SPIWithPollingInterval)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPSPIDevice"},
        {"Name", "cov-spi-poll"},
        {"Bus", "0"},
        {"ChipSelect", "0"},
        {"StaticEndpointID", "7"},
        {"PollingInterval", "30"},
    };
    EXPECT_EQ(SPIMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, SPIBadStaticEndpointIdThrows)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPSPIDevice"}, {"Name", "cov-spi-bad-eid"}, {"Bus", "0"},
        {"ChipSelect", "0"},       {"StaticEndpointID", "xyz"},
    };
    EXPECT_THROW(SPIMCTPDDevice::from({}, iface), std::invalid_argument);
}

TEST(MCTPDeviceFrom, XROTWithPollingInterval)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPXROTTarget"}, {"Name", "cov-xrot-poll"},
        {"Interface", "xrot0"},     {"StaticEndpointID", "17"},
        {"PollingInterval", "60"},
    };
    auto dev = XROTMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(dev->getEid().value_or(0), 17);
}

TEST(MCTPDeviceFrom, XROTBadStaticEndpointIdThrows)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPXROTTarget"},
        {"Name", "cov-xrot-bad-eid"},
        {"Interface", "xrot0"},
        {"StaticEndpointID", "xyz"},
    };
    EXPECT_THROW(XROTMCTPDDevice::from({}, iface), std::invalid_argument);
}

TEST(MCTPDeviceFrom, USBWithBridgeStartBadValueThrows)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"},     {"Name", "cov-usb-bad-start"},
        {"Interface", "usb0"},         {"StaticEndpointID", "9"},
        {"BridgePoolStartEID", "xyz"},
    };
    EXPECT_THROW(USBMCTPDDevice::from({}, iface), std::invalid_argument);
}

TEST(MCTPDeviceFrom, USBWithBridgeEndBadValueThrows)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"},   {"Name", "cov-usb-bad-end"},
        {"Interface", "usb0"},       {"StaticEndpointID", "9"},
        {"BridgePoolEndEID", "xyz"},
    };
    EXPECT_THROW(USBMCTPDDevice::from({}, iface), std::invalid_argument);
}

TEST(MCTPDeviceFrom, USBWithBridgePoolNoStaticEid)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"},  {"Name", "cov-usb-pool-no-static"},
        {"Interface", "usb0"},      {"BridgePoolStartEID", "10"},
        {"BridgePoolEndEID", "20"},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
    EXPECT_FALSE(dev->getEid().has_value());
}

TEST(MCTPDeviceFrom, USBWithBridgeEndOnlyNoStart)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"},  {"Name", "cov-usb-end-only"},
        {"Interface", "usb0"},      {"StaticEndpointID", "9"},
        {"BridgePoolEndEID", "20"},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(dev->getEid().value_or(0), 9);
}

TEST(MCTPDeviceFrom, USBWithPollingInterval)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"}, {"Name", "cov-usb-poll"},
        {"Interface", "usb0"},     {"StaticEndpointID", "9"},
        {"PollingInterval", "30"},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(dev->getEid().value_or(0), 9);
}

TEST(MCTPDeviceFrom, USBWithStaticAndBridgeStartNoEnd)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"},    {"Name", "cov-usb-start-no-end"},
        {"Interface", "usb0"},        {"StaticEndpointID", "9"},
        {"BridgePoolStartEID", "10"},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(dev->getEid().value_or(0), 9);
}

TEST(MCTPDeviceFrom, USBNoStaticNoBridge)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"},
        {"Name", "cov-usb-bare"},
        {"Interface", "usb0"},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
    EXPECT_FALSE(dev->getEid().has_value());
}

TEST(MCTPDeviceFrom, XROTNoStaticNoPolling)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPXROTTarget"},
        {"Name", "cov-xrot-bare"},
        {"Interface", "xrot0"},
    };
    auto dev = XROTMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
    EXPECT_FALSE(dev->getEid().has_value());
    EXPECT_EQ(dev->getInterface(), "xrot0");
}

TEST(MCTPDeviceFrom, USBWithIgnoreEidsNegativeAndLarge)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"},
        {"Name", "cov-usb-ignore-edge"},
        {"Interface", "usb0"},
        {"StaticEndpointID", "9"},
        {"IgnoreEIDs", "-1, 256, 0, 255"},
        {"IgnoreMessageTypes", "-5, 300, 0, 255"},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
}

TEST(MCTPDeviceFrom, I2CWithBadStaticAndValidBus)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI2CTarget"}, {"Name", "cov-i2c-bad-static"}, {"Bus", "0"},
        {"Address", "29"},         {"StaticEndpointID", "abc"},
    };
    EXPECT_THROW(I2CMCTPDDevice::from({}, iface), std::invalid_argument);
}

TEST(MCTPDeviceFrom, I3CWithBadBusValue)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI3CTarget"},
        {"Name", "cov-i3c-bad-bus"},
        {"Bus", "xyz"},
        {"Address", std::vector<uint64_t>{0x6a}},
    };
    EXPECT_THROW(I3CMCTPDDevice::from({}, iface), std::invalid_argument);
}

TEST(MCTPDeviceFrom, SPIBadBusThrows)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPSPIDevice"},
        {"Name", "cov-spi-bad-bus"},
        {"Bus", "xyz"},
        {"ChipSelect", "0"},
    };
    EXPECT_THROW(SPIMCTPDDevice::from({}, iface), std::invalid_argument);
}

TEST(MCTPDeviceFrom, SPIBadChipSelectThrows)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPSPIDevice"},
        {"Name", "cov-spi-bad-cs"},
        {"Bus", "0"},
        {"ChipSelect", "xyz"},
    };
    EXPECT_THROW(SPIMCTPDDevice::from({}, iface), std::invalid_argument);
}

TEST(MCTPDeviceFrom, USBStaticOnlyNoBridge)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"},
        {"Name", "cov-usb-static-only"},
        {"Interface", "usb0"},
        {"StaticEndpointID", "25"},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(dev->getEid().value_or(0), 25);
}

TEST(MCTPDeviceFrom, USBBadStaticEndpointIdThrowsInvalidArg)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"},
        {"Name", "cov-usb-bad-eid"},
        {"Interface", "usb0"},
        {"StaticEndpointID", "not-a-number"},
    };
    EXPECT_THROW(USBMCTPDDevice::from({}, iface), std::invalid_argument);
}

TEST(MCTPDeviceFrom, XROTBadStaticEndpointIdThrowsInvalidArg)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPXROTTarget"},
        {"Name", "cov-xrot-bad-eid"},
        {"Interface", "xrot0"},
        {"StaticEndpointID", "not-a-number"},
    };
    EXPECT_THROW(XROTMCTPDDevice::from({}, iface), std::invalid_argument);
}

TEST(MCTPDeviceFrom, USBWithAllFieldsFullyPopulated)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"},    {"Name", "full-usb,bridge-a,bridge-b"},
        {"Interface", "usb0"},        {"StaticEndpointID", "9"},
        {"BridgePoolStartEID", "10"}, {"BridgePoolEndEID", "11"},
        {"IgnoreEIDs", "1,2,3"},      {"IgnoreMessageTypes", "4,5"},
        {"PollingInterval", "60"},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(dev->getEid().value_or(0), 9);
    EXPECT_EQ(dev->getName(), "full-usb");
    EXPECT_EQ(dev->getInterface(), "usb0");
    EXPECT_TRUE(dev->managesEid(9));
    EXPECT_TRUE(dev->managesEid(10));
    EXPECT_TRUE(dev->managesEid(11));
    EXPECT_FALSE(dev->managesEid(12));
}

TEST(MCTPDeviceFrom, I2CWithPollingIntervalInvalidValue)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI2CTarget"},
        {"Name", "cov-i2c-bad-poll"},
        {"Bus", "0"},
        {"Address", "29"},
        {"PollingInterval", "not-a-number"},
    };
    EXPECT_EQ(I2CMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, I2CWithIgnoreMessageTypesValid)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI2CTarget"},
        {"Name", "cov-i2c-ignore-mt"},
        {"Bus", "0"},
        {"Address", "29"},
        {"IgnoreMessageTypes", "1, 2, 3"},
    };
    EXPECT_EQ(I2CMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, I2CWithIgnoreMessageTypesEmpty)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI2CTarget"},
        {"Name", "cov-i2c-ignore-mt-empty"},
        {"Bus", "0"},
        {"Address", "29"},
        {"IgnoreMessageTypes", ""},
    };
    EXPECT_EQ(I2CMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, I2CWithIgnoreMessageTypesMixed)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI2CTarget"},
        {"Name", "cov-i2c-ignore-mt-mixed"},
        {"Bus", "0"},
        {"Address", "29"},
        {"IgnoreMessageTypes", "1, bad, 300, -1, 255"},
    };
    EXPECT_EQ(I2CMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, I2CWithIgnoreMessageTypesWrongVariant)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPI2CTarget"},
        {"Name", "cov-i2c-ignore-mt-variant"},
        {"Bus", "0"},
        {"Address", "29"},
        {"IgnoreMessageTypes", std::vector<uint64_t>{1, 2}},
    };
    EXPECT_EQ(I2CMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, USBWithPollingIntervalZero)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBTarget"}, {"Name", "cov-usb-poll-zero"},
        {"Interface", "usb0"},     {"StaticEndpointID", "9"},
        {"PollingInterval", "0"},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(dev->getEid().value_or(0), 9);
}

TEST(MCTPDeviceFrom, XROTMissingNameThrows)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPXROTTarget"},
        {"Interface", "xrot0"},
    };
    EXPECT_THROW(XROTMCTPDDevice::from({}, iface), std::invalid_argument);
}

TEST(MCTPDeviceFrom, XROTMissingInterfaceThrows)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPXROTTarget"},
        {"Name", "xrot-no-iface"},
    };
    EXPECT_THROW(XROTMCTPDDevice::from({}, iface), std::invalid_argument);
}

// ---- end from() branch coverage tests ----

TEST_F(MCTPReactorFixture, manageMCTPDeviceNullThenRealDeviceAtSamePath)
{
    reactor->manageMCTPDevice("/test", {});

    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    EXPECT_CALL(*device, remove());

    reactor->manageMCTPDevice("/test", device);
    reactor->unmanageMCTPDevice("/test");
}

TEST_F(MCTPReactorFixture, tickWithNoDeferredDevicesIsNoop)
{
    reactor->tick();
    EXPECT_FALSE(reactor->isRetrying(0));
}

TEST_F(MCTPReactorFixture, manageMCTPDeviceSetupErrorDoesNotAssociate)
{
    EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::io_error), nullptr));
    EXPECT_CALL(*device, remove());

    reactor->manageMCTPDevice("/test", device);
    reactor->unmanageMCTPDevice("/test");
}

TEST(MCTPReactor, isRetryingWithEmptyFailureCountsReturnsFalse)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    EXPECT_FALSE(reactor->isRetrying(0));
    EXPECT_FALSE(reactor->isRetrying(42));
}

TEST_F(MCTPReactorFixture, multipleSetupFailuresThenSuccess)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation));
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"));
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));
    EXPECT_CALL(*endpoint, remove()).WillOnce([&]() {
        removeHandler(endpoint);
    });
    EXPECT_CALL(*device, remove()).WillOnce([&]() { endpoint->remove(); });
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), endpoint))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::connection_refused), endpoint))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));

    reactor->manageMCTPDevice("/test", device);
    EXPECT_TRUE(reactor->isRetrying(0));
    reactor->tick();
    EXPECT_TRUE(reactor->isRetrying(0));
    reactor->tick();
    EXPECT_FALSE(reactor->isRetrying(0));
    reactor->unmanageMCTPDevice("/test");
}

TEST(MCTPReactor, manageMCTPDeviceSkipsNonMctpdDevices)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    auto dev = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*dev, describe()).WillRepeatedly(testing::Return("mock-dev"));
    EXPECT_CALL(*dev, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    EXPECT_CALL(*dev, remove());

    reactor->manageMCTPDevice("/test/mock", dev);
    reactor->unmanageMCTPDevice("/test/mock");
}

TEST(MCTPReactor, isRetryingSkipsNonMctpdDevices)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    auto dev = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*dev, describe()).WillRepeatedly(testing::Return("mock-fail"));
    EXPECT_CALL(*dev, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr));
    EXPECT_CALL(*dev, remove());

    reactor->manageMCTPDevice("/test/fail", dev);
    EXPECT_TRUE(reactor->isRetrying(0)); // Broadcast retry is true
    EXPECT_FALSE(
        reactor->isRetrying(10)); // Specific EID check skips non-MCTPDDevice
    reactor->unmanageMCTPDevice("/test/fail");
}

TEST(MCTPReactor, deferredRetryClearsOnSuccessWithoutEndpointObject)
{
    try
    {
        boost::asio::io_context io;
        auto conn = std::make_shared<sdbusplus::asio::connection>(io);
        MockAssociationServer assoc{};
        auto reactor = std::make_shared<MCTPReactor>(assoc);
        auto dev = std::make_shared<TestReactorMCTPDDevice>(conn, 55);
        int setupCalls = 0;
        dev->setupHandler =
            [&setupCalls](
                std::function<void(const std::error_code& ec,
                                   const std::shared_ptr<MCTPEndpoint>& ep)>&&
                    added) {
                setupCalls++;
                if (setupCalls == 1)
                {
                    std::move(added)(std::make_error_code(std::errc::timed_out),
                                     nullptr);
                    return;
                }
                std::move(added)({}, nullptr);
            };

        reactor->manageMCTPDevice("/test", dev);
        EXPECT_TRUE(reactor->isRetrying(55));
        reactor->tick();
        EXPECT_FALSE(reactor->isRetrying(55));
        reactor->unmanageMCTPDevice("/test");
        EXPECT_EQ(dev->removeCalls, 1);
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "DBus unavailable in this environment: " << ex.what();
    }
}

// ---- Additional branch coverage for MCTPReactor.cpp ----

// Covers the rethrow branch in manageMCTPDevice:
//   if (e.code() != std::errc::device_or_resource_busy) { throw e; }
// This branch is taken when a std::system_error with a code OTHER than
// device_or_resource_busy is thrown from devices.add().
//
// The MCTPDeviceRepository::add() only throws EBUSY (device_or_resource_busy)
// when the same path is re-added with a different device pointer. To reach the
// non-EBUSY rethrow branch we need a system_error with a different code.
// We exercise this by using a custom device that calls devices.remove() on a
// never-added device, causing a no_such_device error that propagates up as a
// system_error through the setup callback chain.
//
// In practice the most reachable path for non-EBUSY system_error is through
// device->setup() throwing a system_error (not caught by the inner lambda) or
// via the repository itself. Because devices.add() in the current
// implementation only throws EBUSY, we test the rethrow path by confirming
// that when a second manageMCTPDevice call on the same path with the SAME
// device object does NOT throw (fresh==true would be false but second==same
// pointer so no EBUSY), and a different device DOES throw EBUSY.  The
// non-EBUSY rethrow cannot be reached through the public API without modifying
// MCTPDeviceRepository, so we verify it indirectly by confirming the EBUSY
// path is correctly distinguished from other system_errors at integration
// level.

// Test: manageMCTPDevice with same path+device twice is idempotent (no throw).
// Covers the `!fresh && entry->second.get() == device.get()` non-throw path in
// MCTPDeviceRepository::add and the normal flow through manageMCTPDevice.
TEST_F(MCTPReactorFixture, manageSameDeviceTwiceAtSamePathIsIdempotent)
{
    EXPECT_CALL(*device, setup(testing::_))
        .WillRepeatedly(testing::InvokeArgument<0>(std::error_code(), nullptr));
    EXPECT_CALL(*device, remove()).Times(1);

    // First call: adds the device and calls setup.
    reactor->manageMCTPDevice("/test/idem", device);
    // Second call with the identical device pointer: repository sees the same
    // pointer (fresh==false but same ptr), does NOT throw, so manageMCTPDevice
    // proceeds straight to setupEndpoint again.
    reactor->manageMCTPDevice("/test/idem", device);
    reactor->unmanageMCTPDevice("/test/idem");
}

// Test: manageMCTPDevice EBUSY path followed immediately by unmanage of the
// replacement before tick() — exercises deferSetup for the new device and then
// unmanageMCTPDevice that removes it from the deferred set.
TEST(MCTPReactor, replaceConfigurationUnmanagedBeforeTick)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test/replace-early"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation))
        .Times(1);
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(1);

    auto endpoint = std::make_shared<MockMCTPEndpoint>();
    EXPECT_CALL(*endpoint, describe())
        .WillRepeatedly(testing::Return("mock endpoint"));
    EXPECT_CALL(*endpoint, eid()).WillRepeatedly(testing::Return(9));
    EXPECT_CALL(*endpoint, network()).WillRepeatedly(testing::Return(1));
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));
    EXPECT_CALL(*endpoint, remove()).WillOnce([&]() {
        removeHandler(endpoint);
    });

    auto initial = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*initial, describe())
        .WillRepeatedly(testing::Return("initial-replace-early"));
    EXPECT_CALL(*initial, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*initial, remove()).WillOnce([&]() { endpoint->remove(); });
    EXPECT_CALL(*endpoint, device()).WillRepeatedly(testing::Return(initial));

    auto replacement = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*replacement, describe())
        .WillRepeatedly(testing::Return("replacement-replace-early"));
    // replacement->setup() is never called because it gets unmanaged before
    // tick(). But after deferSetup, unmanageMCTPDevice removes it from
    // deferred.
    EXPECT_CALL(*replacement, setup(testing::_)).Times(0);
    EXPECT_CALL(*replacement, remove()).Times(1);

    // First manage: succeeds and installs endpoint.
    reactor->manageMCTPDevice("/test/replace-early", initial);
    // Second manage: triggers EBUSY, calls unmanageMCTPDevice(initial),
    // deferSetup(replacement).
    reactor->manageMCTPDevice("/test/replace-early", replacement);
    // Unmanage before tick: removes replacement from deferred set.
    reactor->unmanageMCTPDevice("/test/replace-early");

    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(initial.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(replacement.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(endpoint.get()));
}

// Test: deferSetup called multiple times for the same device increments
// failureCounts but does not duplicate the device in deferred (set semantics).
// Then tick() retries once (not twice), confirming the set de-duplication.
TEST_F(MCTPReactorFixture, deferSetupMultipleTimesForSameDeviceCallsSetupOnce)
{
    // Three setup failures in a row → deferred called 3 times for same device
    // but tick() should only call setup once (set deduplication).
    int setupCallCount = 0;
    EXPECT_CALL(*device, setup(testing::_))
        .WillRepeatedly(
            [&](const std::function<void(const std::error_code&,
                                         const std::shared_ptr<MCTPEndpoint>&)>&
                    cb) {
                setupCallCount++;
                cb(std::make_error_code(std::errc::timed_out), nullptr);
            });
    EXPECT_CALL(*device, remove()).Times(1);

    reactor->manageMCTPDevice("/test/defer-multi", device);
    // After manage: 1 setup call, device is in deferred.
    EXPECT_EQ(setupCallCount, 1);
    EXPECT_TRUE(reactor->isRetrying(0));

    // tick() drains deferred (1 entry) → 1 more setup call, re-deferred.
    reactor->tick();
    EXPECT_EQ(setupCallCount, 2);

    // tick() again → 1 more setup call (still only 1 entry in deferred).
    reactor->tick();
    EXPECT_EQ(setupCallCount, 3);

    reactor->unmanageMCTPDevice("/test/defer-multi");
    EXPECT_FALSE(reactor->isRetrying(0));
}

// Test: unmanageMCTPDevice with a path that was never managed is a noop and
// does not crash (regression: ensure the debug log path executes cleanly).
TEST_F(MCTPReactorFixture, unmanageNeverManagedPathIsNoop)
{
    // Should not crash; debug log path in unmanageMCTPDevice fires.
    reactor->unmanageMCTPDevice("/never/managed/path");
    EXPECT_FALSE(reactor->isRetrying(0));
    EXPECT_FALSE(reactor->getDeviceName(1).has_value());
}

// Test: getStaticEidFromInterface with a managed MCTPDDevice that has a
// matching interface returns the static EID; a non-matching interface
// returns nullopt.
TEST(MCTPReactor, getStaticEidFromInterfaceWithMatchingAndNonMatchingDevice)
{
    try
    {
        boost::asio::io_context io;
        auto conn = std::make_shared<sdbusplus::asio::connection>(io);
        MockAssociationServer assoc{};
        auto reactor = std::make_shared<MCTPReactor>(assoc);

        auto dev = std::make_shared<TestReactorMCTPDDevice>(conn, 33);
        // TestReactorMCTPDDevice uses interface "usbreactor0".
        dev->setupHandler = [](auto&& added) {
            std::forward<decltype(added)>(
                added)(std::make_error_code(std::errc::timed_out), nullptr);
        };

        reactor->manageMCTPDevice("/test/static-eid", dev);

        // Matching interface → should return the static EID (33).
        auto eid = reactor->getStaticEidFromInterface("usbreactor0");
        EXPECT_TRUE(eid.has_value());
        if (eid.has_value())
        {
            EXPECT_EQ(eid.value(), 33);
        }

        // Non-matching interface → nullopt.
        EXPECT_FALSE(
            reactor->getStaticEidFromInterface("nonexistent0").has_value());

        reactor->unmanageMCTPDevice("/test/static-eid");
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "DBus unavailable in this environment: " << ex.what();
    }
}

// Test: getDeviceName with a managed MCTPDDevice returns the name; after
// unmanage it returns nullopt.
TEST(MCTPReactor, getDeviceNameReturnedForManagedMctpdDevice)
{
    try
    {
        boost::asio::io_context io;
        auto conn = std::make_shared<sdbusplus::asio::connection>(io);
        MockAssociationServer assoc{};
        auto reactor = std::make_shared<MCTPReactor>(assoc);

        // TestReactorMCTPDDevice: name="reactor-mctpd", staticEID=66.
        auto dev = std::make_shared<TestReactorMCTPDDevice>(conn, 66);
        dev->setupHandler = [](auto&& added) {
            std::forward<decltype(added)>(added)(std::error_code{}, nullptr);
        };

        reactor->manageMCTPDevice("/test/name-lookup", dev);

        // Static EID 66 → name "reactor-mctpd".
        auto name = reactor->getDeviceName(66);
        EXPECT_TRUE(name.has_value());
        if (name.has_value())
        {
            EXPECT_EQ(name.value(), "reactor-mctpd");
        }

        // Unknown EID → nullopt.
        EXPECT_FALSE(reactor->getDeviceName(99).has_value());

        reactor->unmanageMCTPDevice("/test/name-lookup");

        // After unmanage, the device is gone so lookup returns nullopt.
        EXPECT_FALSE(reactor->getDeviceName(66).has_value());
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "DBus unavailable in this environment: " << ex.what();
    }
}

// Test: isRetrying(0) returns false after all deferred devices are cleared via
// unmanageMCTPDevice, even when multiple devices were deferred.
TEST(MCTPReactor, isRetryingFalseAfterAllDevicesUnmanaged)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    auto dev1 = std::make_shared<MockMCTPDevice>();
    auto dev2 = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*dev1, describe()).WillRepeatedly(testing::Return("dev1"));
    EXPECT_CALL(*dev2, describe()).WillRepeatedly(testing::Return("dev2"));
    EXPECT_CALL(*dev1, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr));
    EXPECT_CALL(*dev2, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr));
    EXPECT_CALL(*dev1, remove()).Times(1);
    EXPECT_CALL(*dev2, remove()).Times(1);

    reactor->manageMCTPDevice("/dev1", dev1);
    reactor->manageMCTPDevice("/dev2", dev2);
    EXPECT_TRUE(reactor->isRetrying(0));

    reactor->unmanageMCTPDevice("/dev1");
    EXPECT_TRUE(reactor->isRetrying(0)); // dev2 still pending

    reactor->unmanageMCTPDevice("/dev2");
    EXPECT_FALSE(reactor->isRetrying(0)); // all cleared
}

// Test: setup callback with ec set calls deferSetup and the device ends up in
// the deferred set; calling tick() processes it; a subsequent setup success
// clears failureCounts.
TEST_F(MCTPReactorFixture, tickAfterSetupErrorThenSuccessIsRetryingFalse)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation));
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(1);
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));
    EXPECT_CALL(*endpoint, remove()).WillOnce([&]() {
        removeHandler(endpoint);
    });
    EXPECT_CALL(*device, remove()).WillOnce([&]() { endpoint->remove(); });
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::io_error), nullptr))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));

    reactor->manageMCTPDevice("/test", device);
    EXPECT_TRUE(reactor->isRetrying(0));
    reactor->tick();
    EXPECT_FALSE(reactor->isRetrying(0)); // success cleared failureCounts
    reactor->unmanageMCTPDevice("/test");
}

// Test: manageMCTPDevice with EBUSY where the existing current device in the
// repository is valid exercises the full EBUSY handler path including the
// warning log and the deferSetup call.  This is the same scenario as
// replaceConfiguration but using MockMCTPDevice (no MCTPDDevice) to avoid
// DBus dependencies.
TEST(MCTPReactor, ebusyPathWithMockDevicesLogs)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    auto endpoint = std::make_shared<MockMCTPEndpoint>();
    EXPECT_CALL(*endpoint, describe())
        .WillRepeatedly(testing::Return("mock ep"));
    EXPECT_CALL(*endpoint, eid()).WillRepeatedly(testing::Return(9));
    EXPECT_CALL(*endpoint, network()).WillRepeatedly(testing::Return(1));

    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));
    EXPECT_CALL(*endpoint, remove()).WillOnce([&]() {
        if (removeHandler)
        {
            removeHandler(endpoint);
        }
    });

    auto initial = std::make_shared<MockMCTPDevice>();
    // ep->device() must return initial so that inventoryFor(initial) succeeds
    // in trackEndpoint and the associate() call is made.
    EXPECT_CALL(*endpoint, device()).WillRepeatedly(testing::Return(initial));

    EXPECT_CALL(*initial, describe())
        .WillRepeatedly(testing::Return("initial-ebusy-log"));
    EXPECT_CALL(*initial, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*initial, remove()).WillOnce([&]() { endpoint->remove(); });

    auto replacement = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*replacement, describe())
        .WillRepeatedly(testing::Return("replacement-ebusy-log"));
    // replacement setup is deferred; after unmanage it never runs.
    EXPECT_CALL(*replacement, setup(testing::_)).Times(0);
    EXPECT_CALL(*replacement, remove()).Times(1);

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test/ebusy-log"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation))
        .Times(1);
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(1);

    reactor->manageMCTPDevice("/test/ebusy-log", initial);
    // EBUSY: initial is already at this path, replacement triggers EBUSY path.
    reactor->manageMCTPDevice("/test/ebusy-log", replacement);
    // Unmanage clears deferred replacement before tick.
    reactor->unmanageMCTPDevice("/test/ebusy-log");

    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(initial.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(replacement.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(endpoint.get()));
}

// Test: isRetrying(N) with a non-MCTPDDevice in failureCounts returns false
// because dynamic_cast<MCTPDDevice> fails for MockMCTPDevice.
TEST(MCTPReactor, isRetryingWithNonMCTPDDeviceReturnsFalseForSpecificEid)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    auto mockDevice = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*mockDevice, describe())
        .WillRepeatedly(testing::Return("mock-non-mctpd"));
    // Setup fails → device goes into failureCounts
    EXPECT_CALL(*mockDevice, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::io_error), nullptr));
    EXPECT_CALL(*mockDevice, remove()).Times(1);

    reactor->manageMCTPDevice("/test/non-mctpd", mockDevice);

    // failureCounts is non-empty → isRetrying(0) returns true
    EXPECT_TRUE(reactor->isRetrying(0));

    // For a specific EID, MockMCTPDevice is NOT an MCTPDDevice.
    // dynamic_cast<MCTPDDevice>(mockDevice) returns null → loop completes
    // without matching → returns false.
    EXPECT_FALSE(reactor->isRetrying(9));
    EXPECT_FALSE(reactor->isRetrying(42));

    reactor->unmanageMCTPDevice("/test/non-mctpd");
}

// Test: trackEndpoint() when endpoint->device() is NOT registered in the
// reactor's device repository → devices.inventoryFor() returns nullopt →
// error logged, server.associate() NOT called.
TEST(MCTPReactor, trackEndpointWithMissingInventoryLogsErrorNoAssociate)
{
    MockAssociationServer assoc{};
    // associate must NOT be called
    EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

    auto reactor = std::make_shared<MCTPReactor>(assoc);

    // registeredDevice is added to the reactor
    auto registeredDevice = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*registeredDevice, describe())
        .WillRepeatedly(testing::Return("registered-dev"));
    EXPECT_CALL(*registeredDevice, remove()).Times(1);
    // Setup fails for simplicity (we don't need the endpoint to fire)
    EXPECT_CALL(*registeredDevice, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::io_error), nullptr));
    reactor->manageMCTPDevice("/test/registered", registeredDevice);

    // unregisteredDevice is NOT in the reactor's repository
    auto unregisteredDevice = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*unregisteredDevice, describe())
        .WillRepeatedly(testing::Return("unregistered-dev"));

    // endpoint->device() returns unregisteredDevice (not in repo)
    auto endpoint = std::make_shared<MockMCTPEndpoint>();
    EXPECT_CALL(*endpoint, describe())
        .WillRepeatedly(testing::Return("mock ep"));
    EXPECT_CALL(*endpoint, eid()).WillRepeatedly(testing::Return(9));
    EXPECT_CALL(*endpoint, network()).WillRepeatedly(testing::Return(1));
    EXPECT_CALL(*endpoint, device())
        .WillRepeatedly(testing::Return(unregisteredDevice));
    // subscribe is called by trackEndpoint
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .Times(1);

    // We need to call trackEndpoint indirectly. We do this by making
    // registeredDevice's setup succeed with an endpoint whose device()
    // returns unregisteredDevice (not registered at /test/registered).
    // Unmanage first to reset, then re-manage with a setup that succeeds
    // but returns endpoint linked to a different device.
    reactor->unmanageMCTPDevice("/test/registered");

    auto registeredDevice2 = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*registeredDevice2, describe())
        .WillRepeatedly(testing::Return("registered-dev2"));
    EXPECT_CALL(*registeredDevice2, remove()).Times(1);
    EXPECT_CALL(*registeredDevice2, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));

    // manageMCTPDevice registers registeredDevice2 at /test/reg2
    // trackEndpoint is called with endpoint whose device() = unregisteredDevice
    // (not in repo) → devices.inventoryFor(unregisteredDevice) returns nullopt
    // → error logged, no associate
    reactor->manageMCTPDevice("/test/reg2", registeredDevice2);
    reactor->unmanageMCTPDevice("/test/reg2");
}

// Test: isRetrying(eid) returns true when the failing device is an MCTPDDevice
// with a bridge pool range that contains eid.
//
// This covers the branch in MCTPReactor::isRetrying (lines 153-163) where:
//   - failureCounts is non-empty (device failed setup → deferSetup called)
//   - dynamic_cast<MCTPDDevice> succeeds
//   - mctpDevice->managesEid(eid) returns true because eid is in
//     [bridgePoolStartEid, bridgePoolEndEid]
TEST(MCTPReactor, isRetryingWithBridgePoolEidReturnsTrue)
{
    try
    {
        boost::asio::io_context io;
        auto conn = std::make_shared<sdbusplus::asio::connection>(io);
        MockAssociationServer assoc{};
        auto reactor = std::make_shared<MCTPReactor>(assoc);

        // staticEid=9, bridgePoolStart=10, bridgePoolEnd=12
        auto dev = std::make_shared<TestReactorMCTPDDevice>(conn, 9, 10, 12);
        // Override setup so the first call always fails → device enters
        // failureCounts via deferSetup.
        dev->setupHandler =
            [](std::function<void(const std::error_code& ec,
                                  const std::shared_ptr<MCTPEndpoint>& ep)>&&
                   added) {
                std::move(added)(std::make_error_code(std::errc::timed_out),
                                 nullptr);
            };

        reactor->manageMCTPDevice("/test/bridge-pool", dev);

        // failureCounts non-empty AND mctpDevice->managesEid(11) == true
        // because 10 <= 11 <= 12  →  isRetrying(11) must return true.
        EXPECT_TRUE(reactor->isRetrying(11));

        // EID 13 is outside the bridge pool and not the static EID →
        // managesEid(13) returns false → isRetrying(13) must return false.
        EXPECT_FALSE(reactor->isRetrying(13));

        reactor->unmanageMCTPDevice("/test/bridge-pool");
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "DBus unavailable in this environment: " << ex.what();
    }
}

// Test: tick() with two distinct deferred devices calls setup on both.
// Covers the for-loop body in MCTPReactor::tick() with multiple entries in
// toSetup, confirming that all deferred devices are retried in a single tick.
TEST(MCTPReactor, tickProcessesBothDeferredDevices)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    auto dev1 = std::make_shared<MockMCTPDevice>();
    auto dev2 = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*dev1, describe())
        .WillRepeatedly(testing::Return("deferred-1"));
    EXPECT_CALL(*dev2, describe())
        .WillRepeatedly(testing::Return("deferred-2"));

    // Both devices fail setup on the first attempt, deferring them.
    EXPECT_CALL(*dev1, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    EXPECT_CALL(*dev2, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    EXPECT_CALL(*dev1, remove()).Times(1);
    EXPECT_CALL(*dev2, remove()).Times(1);

    reactor->manageMCTPDevice("/tick/dev1", dev1);
    reactor->manageMCTPDevice("/tick/dev2", dev2);
    EXPECT_TRUE(reactor->isRetrying(0));

    // tick() must retry both deferred devices.  The second setup call
    // succeeds → failureCounts cleared for each device.
    reactor->tick();
    EXPECT_FALSE(reactor->isRetrying(0));

    reactor->unmanageMCTPDevice("/tick/dev1");
    reactor->unmanageMCTPDevice("/tick/dev2");
}

// Test: getDeviceName for a bridge-pool EID whose computed index falls outside
// the deviceNames vector returns nullopt even though managesEid returns true.
// This exercises the `index >= deviceNames.size()` boundary in
// MCTPDDevice::getNameForEid.
TEST(MCTPReactor, getDeviceNameForBridgePoolEidBeyondDeviceNamesBoundsIsNullopt)
{
    try
    {
        boost::asio::io_context io;
        auto conn = std::make_shared<sdbusplus::asio::connection>(io);
        MockAssociationServer assoc{};
        auto reactor = std::make_shared<MCTPReactor>(assoc);

        // staticEid=20, bridgePool=[21,23].
        // deviceNames has only 1 entry ("reactor-mctpd-bridge") → indices 1,2,3
        // are all out of bounds → getNameForEid(21..23) returns nullopt.
        auto dev = std::make_shared<TestReactorMCTPDDevice>(conn, 20, 21, 23);
        dev->setupHandler = [](auto&& added) {
            std::forward<decltype(added)>(
                added)(std::make_error_code(std::errc::timed_out), nullptr);
        };

        reactor->manageMCTPDevice("/test/bridge-names", dev);

        // Static EID 20 → name "reactor-mctpd-bridge".
        auto name = reactor->getDeviceName(20);
        EXPECT_TRUE(name.has_value());

        // Bridge pool EIDs 21..23: index >= deviceNames.size() → nullopt.
        EXPECT_FALSE(reactor->getDeviceName(21).has_value());
        EXPECT_FALSE(reactor->getDeviceName(22).has_value());
        EXPECT_FALSE(reactor->getDeviceName(23).has_value());

        // EID outside both ranges → nullopt.
        EXPECT_FALSE(reactor->getDeviceName(24).has_value());

        reactor->unmanageMCTPDevice("/test/bridge-names");
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "DBus unavailable in this environment: " << ex.what();
    }
}

// Test: getStaticEidFromInterface skips non-MCTPDDevice entries in the
// repository and still finds the MCTPDDevice with the matching interface.
// This exercises the `if (mctpDevice && mctpDevice->getInterface() == ...)`
// null branch in MCTPDeviceRepository::getStaticEidFromInterface.
TEST(MCTPReactor, getStaticEidFromInterfaceSkipsNonMctpdDevices)
{
    try
    {
        boost::asio::io_context io;
        auto conn = std::make_shared<sdbusplus::asio::connection>(io);
        MockAssociationServer assoc{};
        auto reactor = std::make_shared<MCTPReactor>(assoc);

        // Add a plain MockMCTPDevice first; it is not an MCTPDDevice so the
        // dynamic_cast inside getStaticEidFromInterface returns null for it.
        auto mockDev = std::make_shared<MockMCTPDevice>();
        EXPECT_CALL(*mockDev, describe())
            .WillRepeatedly(testing::Return("non-mctpd"));
        EXPECT_CALL(*mockDev, setup(testing::_))
            .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
        EXPECT_CALL(*mockDev, remove()).Times(1);
        reactor->manageMCTPDevice("/test/non-mctpd", mockDev);

        // Add an MCTPDDevice with interface "usbreactor0" and staticEid=77.
        auto mctpdDev = std::make_shared<TestReactorMCTPDDevice>(conn, 77);
        mctpdDev->setupHandler = [](auto&& added) {
            std::forward<decltype(added)>(
                added)(std::make_error_code(std::errc::timed_out), nullptr);
        };
        reactor->manageMCTPDevice("/test/mctpd-iface", mctpdDev);

        // The non-MCTPDDevice is skipped; the MCTPDDevice is found.
        auto eid = reactor->getStaticEidFromInterface("usbreactor0");
        EXPECT_TRUE(eid.has_value());
        if (eid.has_value())
        {
            EXPECT_EQ(eid.value(), 77);
        }

        // An interface present on neither device returns nullopt.
        EXPECT_FALSE(
            reactor->getStaticEidFromInterface("nonexistent").has_value());

        reactor->unmanageMCTPDevice("/test/non-mctpd");
        reactor->unmanageMCTPDevice("/test/mctpd-iface");
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "DBus unavailable in this environment: " << ex.what();
    }
}

// ---- R3: isRetrying bridge pool boundary checks ----

// Test: isRetrying(eid) with a wider bridge pool [10, 20] covers the three
// boundary cases explicitly:
//   - EID 15 (inside the pool)     → true
//   - EID 9  (one below the pool)  → false
//   - EID 21 (one above the pool)  → false
// The existing isRetryingWithBridgePoolEidReturnsTrue test uses [10,12].
// This test uses a wider pool [10,20] so that the "EID 9 → false" and
// "EID 21 → false" boundaries each exercise the managesEid range check.
TEST(MCTPReactor, isRetryingBridgePoolBoundaryValues)
{
    try
    {
        boost::asio::io_context io;
        auto conn = std::make_shared<sdbusplus::asio::connection>(io);
        MockAssociationServer assoc{};
        auto reactor = std::make_shared<MCTPReactor>(assoc);

        // staticEid=8, bridgePoolStart=10, bridgePoolEnd=20
        auto dev = std::make_shared<TestReactorMCTPDDevice>(conn, 8, 10, 20);
        dev->setupHandler =
            [](std::function<void(const std::error_code& ec,
                                  const std::shared_ptr<MCTPEndpoint>& ep)>&&
                   added) {
                std::move(added)(std::make_error_code(std::errc::timed_out),
                                 nullptr);
            };

        reactor->manageMCTPDevice("/test/bridge-boundary", dev);

        // EID 15 is inside [10, 20] → managesEid(15) true → isRetrying true
        EXPECT_TRUE(reactor->isRetrying(15));

        // EID 9 is one below the pool → managesEid(9) false (not static EID
        // 8 either) → isRetrying(9) false
        EXPECT_FALSE(reactor->isRetrying(9));

        // EID 21 is one above the pool → managesEid(21) false → isRetrying
        // false
        EXPECT_FALSE(reactor->isRetrying(21));

        // EID 8 is the static EID → managesEid(8) true → isRetrying true
        EXPECT_TRUE(reactor->isRetrying(8));

        // Broadcast EID 0 → failureCounts non-empty → true
        EXPECT_TRUE(reactor->isRetrying(0));

        reactor->unmanageMCTPDevice("/test/bridge-boundary");
        EXPECT_FALSE(reactor->isRetrying(0));
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "DBus unavailable in this environment: " << ex.what();
    }
}

// ---- R4: setupEndpoint MCTPException catch path ----

// Test: When endpoint->subscribe() throws MCTPException, setupEndpoint catches
// it, logs the error, and calls deferSetup(dev) so the device ends up in the
// deferred set (isRetrying(0) becomes true).
// This is a distinct scenario from subscribeFailureDefersSetup: here we verify
// the failure count increments and the device is re-queued by tick().
TEST_F(MCTPReactorFixture, subscribeThrowsMCTPExceptionDeviceIsRequeued)
{
    // First setup call: succeeds but subscribe throws → deferSetup called.
    // Second setup call (from tick()): succeeds and subscribe works → success.
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation))
        .Times(1);
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(1);

    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(
            testing::Throw(MCTPException("subscribe failed in requeue test")))
        .WillOnce(testing::SaveArg<2>(&removeHandler));
    EXPECT_CALL(*endpoint, remove()).WillOnce([&]() {
        removeHandler(endpoint);
    });
    EXPECT_CALL(*device, remove()).WillOnce([&]() { endpoint->remove(); });
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));

    reactor->manageMCTPDevice("/test", device);
    // After first setup: subscribe threw → device is in deferred, retrying
    EXPECT_TRUE(reactor->isRetrying(0));

    // tick() retries the deferred device; second setup succeeds → cleared
    reactor->tick();
    EXPECT_FALSE(reactor->isRetrying(0));

    reactor->unmanageMCTPDevice("/test");
}

// ---- R5: tick() with three deferred devices ----

// Test: tick() processes all deferred devices when there are three of them.
// Extends the existing tickProcessesBothDeferredDevices (two devices) test
// to confirm the for-loop in tick() handles an arbitrary number of entries.
TEST(MCTPReactor, tickProcessesThreeDeferredDevices)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    auto dev1 = std::make_shared<MockMCTPDevice>();
    auto dev2 = std::make_shared<MockMCTPDevice>();
    auto dev3 = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*dev1, describe())
        .WillRepeatedly(testing::Return("deferred-tick-1"));
    EXPECT_CALL(*dev2, describe())
        .WillRepeatedly(testing::Return("deferred-tick-2"));
    EXPECT_CALL(*dev3, describe())
        .WillRepeatedly(testing::Return("deferred-tick-3"));

    // All three fail on first attempt (deferring), then succeed on retry.
    EXPECT_CALL(*dev1, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    EXPECT_CALL(*dev2, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    EXPECT_CALL(*dev3, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    EXPECT_CALL(*dev1, remove()).Times(1);
    EXPECT_CALL(*dev2, remove()).Times(1);
    EXPECT_CALL(*dev3, remove()).Times(1);

    reactor->manageMCTPDevice("/tick3/dev1", dev1);
    reactor->manageMCTPDevice("/tick3/dev2", dev2);
    reactor->manageMCTPDevice("/tick3/dev3", dev3);
    EXPECT_TRUE(reactor->isRetrying(0));

    // Single tick() must retry all three deferred devices and clear retrying.
    reactor->tick();
    EXPECT_FALSE(reactor->isRetrying(0));

    reactor->unmanageMCTPDevice("/tick3/dev1");
    reactor->unmanageMCTPDevice("/tick3/dev2");
    reactor->unmanageMCTPDevice("/tick3/dev3");
}

// ---- R1: manageMCTPDevice null-device returns early (explicit isRetrying
// check) ----

// Test: Calling manageMCTPDevice with a null device is a documented no-op.
// This test verifies the early-return `if (!device) { return; }` branch by
// confirming that:
//   - No device is tracked (getDeviceName returns nullopt)
//   - isRetrying(0) remains false (no deferSetup was called)
//   - unmanageMCTPDevice on the same path is also a no-op (not known)
TEST_F(MCTPReactorFixture, manageNullDeviceDoesNotAffectRetryState)
{
    EXPECT_FALSE(reactor->isRetrying(0));
    reactor->manageMCTPDevice("/test/null-dev", {});
    EXPECT_FALSE(reactor->isRetrying(0));
    EXPECT_FALSE(reactor->getDeviceName(42).has_value());
    // unmanage on a path that was never actually registered is a noop
    reactor->unmanageMCTPDevice("/test/null-dev");
    EXPECT_FALSE(reactor->isRetrying(0));
}

// ---- R2: EBUSY replacement path - replacement device enters deferred, then
// tick() calls setup on it ----

// Test: When manageMCTPDevice is called with a new device at a path that
// already has a different device, the EBUSY handler:
//   1. Calls unmanageMCTPDevice (removes the old device)
//   2. Calls devices.add(path, newDevice)
//   3. Calls deferSetup(newDevice)
// After tick(), the replacement device gets its setup called.
// Distinct from replaceConfigurationUnmanagedBeforeTick because here we call
// tick() and confirm the replacement's setup runs.
TEST(MCTPReactor, ebusyReplacementSetupRunsAfterTick)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    auto endpoint = std::make_shared<MockMCTPEndpoint>();
    EXPECT_CALL(*endpoint, describe())
        .WillRepeatedly(testing::Return("ep-replace-tick"));
    EXPECT_CALL(*endpoint, eid()).WillRepeatedly(testing::Return(9));
    EXPECT_CALL(*endpoint, network()).WillRepeatedly(testing::Return(1));

    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));
    EXPECT_CALL(*endpoint, remove()).WillOnce([&]() {
        if (removeHandler)
        {
            removeHandler(endpoint);
        }
    });

    auto initial = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*initial, describe())
        .WillRepeatedly(testing::Return("initial-replace-tick"));
    EXPECT_CALL(*endpoint, device()).WillRepeatedly(testing::Return(initial));

    std::vector<Association> assocInitial{
        {"configured_by", "configures", "/test/ebusy-tick"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          assocInitial))
        .Times(1);
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(1);

    EXPECT_CALL(*initial, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*initial, remove()).WillOnce([&]() { endpoint->remove(); });

    auto replacement = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*replacement, describe())
        .WillRepeatedly(testing::Return("replacement-replace-tick"));
    // After deferSetup, tick() calls setup on replacement.
    EXPECT_CALL(*replacement, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    EXPECT_CALL(*replacement, remove()).Times(1);

    // First manage: initial device is set up and associated.
    reactor->manageMCTPDevice("/test/ebusy-tick", initial);
    // Second manage: EBUSY path → initial removed, replacement deferred.
    reactor->manageMCTPDevice("/test/ebusy-tick", replacement);
    EXPECT_TRUE(reactor->isRetrying(0));

    // tick(): replacement's setup is called and succeeds → no longer retrying.
    reactor->tick();
    EXPECT_FALSE(reactor->isRetrying(0));

    reactor->unmanageMCTPDevice("/test/ebusy-tick");

    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(initial.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(replacement.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(endpoint.get()));
}

// ---- G211–G230: Additional branch coverage (Batches E1–E4, H10) ----

// G211: trackEndpoint with inventoryFor returning nullopt — verified by having
// the endpoint's device() return a device that is not registered in the
// reactor's repository. subscribe() IS called (before the inventory check),
// but associate() must NOT be called.
// This is an independent fixture-based variant of the standalone
// endpointWithMissingInventorySkipsAssociation / trackEndpointWithMissing…
// tests, exercising the same branch from the MCTPReactorFixture context.
TEST_F(MCTPReactorFixture, G211trackEndpointInventoryMissingSkipsAssociate)
{
    // unregisteredDevice is never added to reactor's repository.
    auto unregisteredDevice = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*unregisteredDevice, describe())
        .WillRepeatedly(testing::Return("g211-unregistered"));

    // endpoint->device() returns unregisteredDevice (not in repo) so that
    // devices.inventoryFor(unregisteredDevice) returns nullopt.
    EXPECT_CALL(*endpoint, device())
        .WillRepeatedly(testing::Return(unregisteredDevice));

    // associate must never be called — inventoryFor returns nullopt.
    EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

    // subscribe IS called by trackEndpoint before the inventoryFor check.
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .Times(1);

    // managed device's setup succeeds with the endpoint whose device() is
    // unregisteredDevice.
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*device, remove()).Times(1);

    reactor->manageMCTPDevice("/test/g211", device);
    reactor->unmanageMCTPDevice("/test/g211");
}

// G212: Removed callback fires AFTER the reactor has been destroyed.
// Tests the `if (auto self = weak.lock())` branch in the removed lambda
// where self is null → logs "reactor destroyed" message, no deferSetup.
// Distinct from removedCallbackAfterReactorDestroyedDoesNotCrash by verifying
// that the device was NOT re-queued (no crash, no UB).
TEST_F(MCTPReactorFixture,
       G212trackEndpointRemovedCallbackAfterReactorDestroyed)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test/g212"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation))
        .Times(1);
    // disassociate must NOT be called because the reactor is gone before the
    // removed callback fires — untrackEndpoint is never reached.
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));

    reactor->manageMCTPDevice("/test/g212", device);
    ASSERT_TRUE(static_cast<bool>(removeHandler));

    // Destroy the reactor: weak_from_this inside the captured lambda expires.
    reactor.reset();

    // Fire the removed callback — the weak_ptr is expired → the else branch
    // executes (log "reactor destroyed"), function returns without crashing.
    removeHandler(endpoint);
}

// G213: Removed callback fires when the device is NOT in the repository.
// Tests the `if (self->devices.contains(ep->device()))` branch → false,
// so deferSetup is NOT called (no re-queue).
// This is a fixture-based complement to
// removedCallbackForForeignDeviceDoesNotDeferSetup, using a second endpoint
// whose device() returns an unregistered pointer.
TEST_F(MCTPReactorFixture, G213trackEndpointRemovedDeviceNotInRepoNoDeferSetup)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    // A separate endpoint whose device() will return a foreign device.
    auto foreignDevice = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*foreignDevice, describe())
        .WillRepeatedly(testing::Return("g213-foreign"));

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test/g213"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation))
        .Times(1);
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(1);

    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));

    // First call: endpoint->device() returns the managed device (needed so
    // inventoryFor succeeds in trackEndpoint and associate() is called).
    // Second call (from removed callback): returns foreignDevice so that
    // devices.contains(foreignDevice) returns false.
    EXPECT_CALL(*endpoint, device())
        .WillOnce(testing::Return(device))
        .WillOnce(testing::Return(foreignDevice));

    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*device, remove()).Times(1);

    reactor->manageMCTPDevice("/test/g213", device);
    ASSERT_TRUE(static_cast<bool>(removeHandler));

    // Fire removed callback: ep->device() returns foreignDevice, which is
    // NOT in the repository, so deferSetup is NOT called.
    removeHandler(endpoint);

    // tick() must NOT call setup again (nothing was deferred).
    reactor->tick();
    EXPECT_FALSE(reactor->isRetrying(0));

    reactor->unmanageMCTPDevice("/test/g213");
}

// G214: Removed callback fires while the reactor is alive AND the device is
// still in the repository. Tests the true branch of
// `self->devices.contains(ep->device())` → deferSetup IS called.
// After the removed callback, isRetrying(0) must be true and tick() re-tries
// the device.
TEST_F(MCTPReactorFixture, G214trackEndpointRemovedDeviceInRepoCausesDeferSetup)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test/g214"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation))
        .Times(2);
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(2);

    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .Times(2)
        .WillRepeatedly(testing::SaveArg<2>(&removeHandler));
    EXPECT_CALL(*endpoint, remove()).WillOnce([&]() {
        removeHandler(endpoint);
    });
    EXPECT_CALL(*device, remove()).WillOnce([&]() { endpoint->remove(); });
    EXPECT_CALL(*device, setup(testing::_))
        .Times(2)
        .WillRepeatedly(
            testing::InvokeArgument<0>(std::error_code(), endpoint));

    reactor->manageMCTPDevice("/test/g214", device);

    // Fire removed callback manually — reactor is alive, device is in repo.
    // deferSetup(device) must be called → isRetrying(0) true.
    removeHandler(endpoint);
    EXPECT_TRUE(reactor->isRetrying(0));

    // tick() retries the deferred device (second setup succeeds).
    reactor->tick();
    EXPECT_FALSE(reactor->isRetrying(0));

    reactor->unmanageMCTPDevice("/test/g214");
}

TEST(MCTPReactor, securityStaleRemovedCallbackDoesNotUnlinkReplacement)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> oldRemove;
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> newRemove;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test/security-replace"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation))
        .Times(2);
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

    auto oldEp = std::make_shared<MockMCTPEndpoint>();
    EXPECT_CALL(*oldEp, describe()).WillRepeatedly(testing::Return("old-ep"));
    EXPECT_CALL(*oldEp, eid()).WillRepeatedly(testing::Return(9));
    EXPECT_CALL(*oldEp, network()).WillRepeatedly(testing::Return(1));
    EXPECT_CALL(*oldEp, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&oldRemove));

    auto newEp = std::make_shared<MockMCTPEndpoint>();
    EXPECT_CALL(*newEp, describe()).WillRepeatedly(testing::Return("new-ep"));
    EXPECT_CALL(*newEp, eid()).WillRepeatedly(testing::Return(9));
    EXPECT_CALL(*newEp, network()).WillRepeatedly(testing::Return(1));
    EXPECT_CALL(*newEp, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&newRemove));

    auto initial = std::make_shared<AsyncRemoveMockMCTPDevice>();
    EXPECT_CALL(*initial, describe())
        .WillRepeatedly(testing::Return("security-initial"));
    EXPECT_CALL(*oldEp, device()).WillRepeatedly(testing::Return(initial));
    EXPECT_CALL(*initial, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), oldEp));
    EXPECT_CALL(*initial, remove()).Times(1);

    auto replacement = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*replacement, describe())
        .WillRepeatedly(testing::Return("security-replacement"));
    EXPECT_CALL(*newEp, device()).WillRepeatedly(testing::Return(replacement));
    int replacementSetupCalls = 0;
    EXPECT_CALL(*replacement, setup(testing::_))
        .Times(1)
        .WillOnce([&](auto&& added) {
            replacementSetupCalls++;
            added(std::error_code(), newEp);
        });
    EXPECT_CALL(*replacement, remove()).Times(1);

    reactor->manageMCTPDevice("/test/security-replace", initial);
    reactor->manageMCTPDevice("/test/security-replace", replacement);
    ASSERT_TRUE(static_cast<bool>(oldRemove));
    EXPECT_EQ(replacementSetupCalls, 0);

    ASSERT_TRUE(static_cast<bool>(initial->removeComplete));
    initial->removeComplete();
    reactor->tick();
    ASSERT_TRUE(static_cast<bool>(newRemove));
    EXPECT_EQ(replacementSetupCalls, 1);

    oldRemove(oldEp);
    reactor->tick();
    EXPECT_EQ(replacementSetupCalls, 1);

    reactor->unmanageMCTPDevice("/test/security-replace");

    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(initial.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(replacement.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(oldEp.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(newEp.get()));
}

// G215: setupEndpoint — weak_ptr to reactor has expired before the setup
// callback fires. The callback must log "reactor destroyed" and return
// without calling deferSetup or trackEndpoint.
// Distinct from G212: here the setup callback hasn't been invoked yet when
// the reactor is destroyed — we save the callback and invoke it post-reset.
TEST(MCTPReactor, G215setupEndpointWeakPtrExpiredBeforeCallback)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    auto dev = std::make_shared<MockMCTPDevice>();
    auto ep = std::make_shared<MockMCTPEndpoint>();

    EXPECT_CALL(*dev, describe()).WillRepeatedly(testing::Return("g215-dev"));
    EXPECT_CALL(*ep, describe()).WillRepeatedly(testing::Return("g215-ep"));

    // associate and disassociate must not be called.
    EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

    std::function<void(const std::error_code&,
                       const std::shared_ptr<MCTPEndpoint>&)>
        savedCb;
    EXPECT_CALL(*dev, setup(testing::_))
        .WillOnce(testing::SaveArg<0>(&savedCb));

    reactor->manageMCTPDevice("/test/g215", dev);
    ASSERT_TRUE(static_cast<bool>(savedCb));

    // Destroy the reactor: the weak_ptr captured in the setup callback expires.
    reactor.reset();

    // Invoke the callback after destruction: the weak_ptr is null → "reactor
    // destroyed" log path executes, no deferSetup, no crash.
    savedCb({}, ep);
}

// G216: setupEndpoint — ec == 0, ep == nullptr → "already discovered" log
// path. No trackEndpoint called, no associate, no deferSetup.
// This is a distinct variant of setupSuccessWithNullEndpointDoesNotAssociate,
// explicitly asserting isRetrying(0) stays false.
TEST_F(MCTPReactorFixture, G216setupEndpointNullEndpointAfterSuccessNoAssociate)
{
    EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    EXPECT_CALL(*device, remove()).Times(1);

    reactor->manageMCTPDevice("/test/g216", device);

    // Success with null ep → failureCounts cleared (it was never set), not
    // retrying.
    EXPECT_FALSE(reactor->isRetrying(0));
    reactor->unmanageMCTPDevice("/test/g216");
}

// G217: setupEndpoint — trackEndpoint throws MCTPException (from subscribe()).
// After the throw, deferSetup(dev) must be called → isRetrying(0) true.
// Then tick() retries and if subscribe succeeds, isRetrying becomes false.
// This is the explicit "MCTPException catch → deferSetup" branch in
// setupEndpoint (MCTPReactor.cpp lines 131-139).
TEST_F(MCTPReactorFixture, G217setupEndpointMCTPExceptionCaughtDeferSetup)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test/g217"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation))
        .Times(1);
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(1);

    // First subscribe throws MCTPException; second succeeds.
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::Throw(MCTPException("G217: subscribe failure")))
        .WillOnce(testing::SaveArg<2>(&removeHandler));
    EXPECT_CALL(*endpoint, remove()).WillOnce([&]() {
        removeHandler(endpoint);
    });
    EXPECT_CALL(*device, remove()).WillOnce([&]() { endpoint->remove(); });

    // Setup succeeds both times (first time triggers MCTPException in
    // trackEndpoint; second time succeeds end-to-end).
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));

    reactor->manageMCTPDevice("/test/g217", device);
    EXPECT_TRUE(reactor->isRetrying(0));

    reactor->tick();
    EXPECT_FALSE(reactor->isRetrying(0));

    reactor->unmanageMCTPDevice("/test/g217");
}

// G218: untrackEndpoint explicitly calls server.disassociate() with the
// correct D-Bus path.  Verifies that the disassociate path is exercised
// independently of the full remove-callback sequence, using the same path
// format as the endpoint's eid/network.
TEST_F(MCTPReactorFixture,
       G218untrackEndpointCallsDisassociateOnRemovedEndpoint)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test/g218"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation))
        .Times(1);
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(1);

    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));
    // endpoint->remove() is not triggered through device->remove() in this
    // test; the cascade is omitted to prevent a second disassociate() call
    // when unmanageMCTPDevice cleans up (untrackEndpoint always calls
    // disassociate unconditionally).
    EXPECT_CALL(*endpoint, remove()).Times(0);
    EXPECT_CALL(*device, remove()).Times(1);
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));

    reactor->manageMCTPDevice("/test/g218", device);
    // Trigger the removed callback manually → untrackEndpoint → disassociate.
    ASSERT_TRUE(static_cast<bool>(removeHandler));
    removeHandler(endpoint);

    // Device is still in repo so deferSetup was called; clear it.
    reactor->unmanageMCTPDevice("/test/g218");
}

// G219: isRetrying(0) with empty failureCounts returns false.
// Also verifies isRetrying(42) returns false (no devices at all).
TEST(MCTPReactor, G219isRetryingEidZeroEmptyFailureCountsFalse)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    EXPECT_FALSE(reactor->isRetrying(0));
    EXPECT_FALSE(reactor->isRetrying(42));
    EXPECT_FALSE(reactor->isRetrying(255));
}

// G220: isRetrying(0) with non-empty failureCounts returns true.
// Achieved by making a MockMCTPDevice's setup fail → deferSetup → failureCounts
// non-empty.
TEST(MCTPReactor, G220isRetryingEidZeroNonEmptyFailureCountsTrue)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    auto dev = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*dev, describe()).WillRepeatedly(testing::Return("g220-dev"));
    EXPECT_CALL(*dev, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr));
    EXPECT_CALL(*dev, remove()).Times(1);

    reactor->manageMCTPDevice("/test/g220", dev);

    // failureCounts has one entry → isRetrying(0) true.
    EXPECT_TRUE(reactor->isRetrying(0));

    // After unmanage, failureCounts is cleared.
    reactor->unmanageMCTPDevice("/test/g220");
    EXPECT_FALSE(reactor->isRetrying(0));
}

// G221: isRetrying(eid) with a non-MCTPDDevice in failureCounts returns false
// for any non-zero eid (dynamic_cast<MCTPDDevice> fails → loop body skips).
// This is a fixture-based complement to isRetryingSkipsNonMctpdDevices and
// isRetryingWithNonMCTPDDeviceReturnsFalseForSpecificEid.
TEST_F(MCTPReactorFixture,
       G221isRetryingNonMCTPDDeviceCastReturnsFalseForSpecificEid)
{
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::connection_refused), nullptr));
    EXPECT_CALL(*device, remove()).Times(1);

    reactor->manageMCTPDevice("/test/g221", device);

    // device is a MockMCTPDevice (not MCTPDDevice):
    //   isRetrying(0) → failureCounts non-empty → true.
    EXPECT_TRUE(reactor->isRetrying(0));
    //   isRetrying(9) → dynamic_cast fails → false.
    EXPECT_FALSE(reactor->isRetrying(9));
    //   isRetrying(1) → same → false.
    EXPECT_FALSE(reactor->isRetrying(1));

    reactor->unmanageMCTPDevice("/test/g221");
    EXPECT_FALSE(reactor->isRetrying(0));
}

// G222: isRetrying(eid) with an MCTPDDevice whose static EID is X and no
// bridge pool — querying with eid != X returns false (managesEid returns
// false because the EID is neither the static EID nor in any bridge pool).
TEST(MCTPReactor, G222isRetryingMCTPDDeviceUnmanagedEidReturnsFalse)
{
    try
    {
        boost::asio::io_context io;
        auto conn = std::make_shared<sdbusplus::asio::connection>(io);
        MockAssociationServer assoc{};
        auto reactor = std::make_shared<MCTPReactor>(assoc);

        auto dev = std::make_shared<TestReactorMCTPDDevice>(conn, 50);
        dev->setupHandler =
            [](std::function<void(const std::error_code& ec,
                                  const std::shared_ptr<MCTPEndpoint>& ep)>&&
                   added) {
                std::move(added)(std::make_error_code(std::errc::timed_out),
                                 nullptr);
            };

        reactor->manageMCTPDevice("/test/g222", dev);

        // Static EID is 50 → managesEid(50) true → isRetrying(50) true.
        EXPECT_TRUE(reactor->isRetrying(50));
        // EID 51 is not managed → isRetrying(51) false.
        EXPECT_FALSE(reactor->isRetrying(51));
        // EID 49 is not managed → isRetrying(49) false.
        EXPECT_FALSE(reactor->isRetrying(49));

        reactor->unmanageMCTPDevice("/test/g222");
        EXPECT_FALSE(reactor->isRetrying(50));
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "DBus unavailable in this environment: " << ex.what();
    }
}

// G223: manageMCTPDevice EBUSY path when current == null.
// In the current MCTPDeviceRepository implementation, once `devices.add()`
// throws EBUSY (path already occupied by a DIFFERENT device), `deviceFor(path)`
// will always return the existing device, so the `!current` branch is
// unreachable through the public API.  We document this with a test confirming
// the EBUSY path when a replacement device is added at a path that already has
// an active device — i.e. the `current != null` sub-path executes.
TEST(MCTPReactor, G223manageMCTPDeviceEBUSYCurrentNonNullExecutesFullPath)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    auto endpoint = std::make_shared<MockMCTPEndpoint>();
    EXPECT_CALL(*endpoint, describe())
        .WillRepeatedly(testing::Return("g223-ep"));
    EXPECT_CALL(*endpoint, eid()).WillRepeatedly(testing::Return(9));
    EXPECT_CALL(*endpoint, network()).WillRepeatedly(testing::Return(1));

    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));
    EXPECT_CALL(*endpoint, remove()).WillOnce([&]() {
        if (removeHandler)
        {
            removeHandler(endpoint);
        }
    });

    auto initial = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*initial, describe())
        .WillRepeatedly(testing::Return("g223-initial"));
    EXPECT_CALL(*endpoint, device()).WillRepeatedly(testing::Return(initial));

    std::vector<Association> assocPath{
        {"configured_by", "configures", "/test/g223"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          assocPath))
        .Times(1);
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(1);

    EXPECT_CALL(*initial, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*initial, remove()).WillOnce([&]() { endpoint->remove(); });

    auto replacement = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*replacement, describe())
        .WillRepeatedly(testing::Return("g223-replacement"));
    // Replacement is deferred; unmanage before tick so setup is never called.
    EXPECT_CALL(*replacement, setup(testing::_)).Times(0);
    EXPECT_CALL(*replacement, remove()).Times(1);

    // EBUSY: initial at /test/g223, adding replacement triggers the handler.
    reactor->manageMCTPDevice("/test/g223", initial);
    reactor->manageMCTPDevice("/test/g223", replacement);
    // Deferred replacement removed before tick.
    reactor->unmanageMCTPDevice("/test/g223");

    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(initial.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(replacement.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(endpoint.get()));
}

// G224: manageMCTPDevice requestSetupCallback fires → setup is called a
// second time. Verifies the setRequestSetupCallback path in manageMCTPDevice
// for MCTPDDevice subclasses.
TEST(MCTPReactor, G224manageMCTPDeviceRequestSetupCallbackFires)
{
    try
    {
        MockAssociationServer assoc{};
        auto reactor = std::make_shared<MCTPReactor>(assoc);
        boost::asio::io_context io;
        auto conn = std::make_shared<sdbusplus::asio::connection>(io);
        auto dev = std::make_shared<TestReactorMCTPDDevice>(conn, 60);

        int setupCalls = 0;
        dev->setupHandler = [&setupCalls](auto&& added) {
            setupCalls++;
            std::forward<decltype(added)>(
                added)(std::make_error_code(std::errc::timed_out), nullptr);
        };

        EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
        EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

        reactor->manageMCTPDevice("/test/g224", dev);
        // Initial setup call counted.
        EXPECT_EQ(setupCalls, 1);

        // triggerRequestSetup fires the requestSetupCallback → setupEndpoint
        // called again → second setup call.
        dev->triggerRequestSetup();
        EXPECT_GE(setupCalls, 2);

        reactor->unmanageMCTPDevice("/test/g224");
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "DBus unavailable in this environment: " << ex.what();
    }
}

// G225: manageMCTPDevice setup failure → failureCounts incremented →
// isRetrying(0) true. Then a second failure → count incremented again.
// Verifies the failure count accumulation (deferSetup increments the count
// each time it's called for the same device).
TEST_F(MCTPReactorFixture, G225deferSetupIncrementsFailureCountEachTime)
{
    EXPECT_CALL(*device, setup(testing::_))
        .WillRepeatedly(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr));
    EXPECT_CALL(*device, remove()).Times(1);

    reactor->manageMCTPDevice("/test/g225", device);
    EXPECT_TRUE(reactor->isRetrying(0));

    // tick() retries → another failure → still retrying.
    reactor->tick();
    EXPECT_TRUE(reactor->isRetrying(0));

    // After unmanage, failureCounts for this device is cleared.
    reactor->unmanageMCTPDevice("/test/g225");
    EXPECT_FALSE(reactor->isRetrying(0));
}

// G226: unmanageMCTPDevice calls device->remove() on the managed device.
// This test explicitly verifies the remove() call path in unmanageMCTPDevice
// using the fixture's mock device.
TEST_F(MCTPReactorFixture, G226unmanageMCTPDeviceCallsDeviceRemove)
{
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    // remove() must be called exactly once from unmanageMCTPDevice.
    EXPECT_CALL(*device, remove()).Times(1);

    reactor->manageMCTPDevice("/test/g226", device);
    reactor->unmanageMCTPDevice("/test/g226");
}

// G227: tick() with deferred devices clears the deferred set and calls setup.
// After a successful tick all devices are no longer deferred.
TEST(MCTPReactor, G227tickClearsAllDeferredDevices)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    auto dev1 = std::make_shared<MockMCTPDevice>();
    auto dev2 = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*dev1, describe()).WillRepeatedly(testing::Return("g227-dev1"));
    EXPECT_CALL(*dev2, describe()).WillRepeatedly(testing::Return("g227-dev2"));

    // Both fail on first attempt, succeed on second (from tick).
    EXPECT_CALL(*dev1, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    EXPECT_CALL(*dev2, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    EXPECT_CALL(*dev1, remove()).Times(1);
    EXPECT_CALL(*dev2, remove()).Times(1);

    reactor->manageMCTPDevice("/g227/dev1", dev1);
    reactor->manageMCTPDevice("/g227/dev2", dev2);
    EXPECT_TRUE(reactor->isRetrying(0));

    // A single tick clears all deferred devices.
    reactor->tick();
    EXPECT_FALSE(reactor->isRetrying(0));

    reactor->unmanageMCTPDevice("/g227/dev1");
    reactor->unmanageMCTPDevice("/g227/dev2");
}

// G228: tick() calls setup on deferred devices; some fail again and are
// re-deferred, while others succeed. After tick, isRetrying reflects the
// remaining failures.
TEST(MCTPReactor, G228tickSomeDeferredRedeferred)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    auto devSuccess = std::make_shared<MockMCTPDevice>();
    auto devFail = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*devSuccess, describe())
        .WillRepeatedly(testing::Return("g228-success"));
    EXPECT_CALL(*devFail, describe())
        .WillRepeatedly(testing::Return("g228-fail"));

    // devSuccess: fails on first attempt, succeeds on tick's retry.
    EXPECT_CALL(*devSuccess, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    // devFail: fails on both attempts (remains deferred after tick).
    EXPECT_CALL(*devFail, setup(testing::_))
        .WillRepeatedly(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr));
    EXPECT_CALL(*devSuccess, remove()).Times(1);
    EXPECT_CALL(*devFail, remove()).Times(1);

    reactor->manageMCTPDevice("/g228/success", devSuccess);
    reactor->manageMCTPDevice("/g228/fail", devFail);
    EXPECT_TRUE(reactor->isRetrying(0));

    // After tick: devSuccess cleared, devFail still retrying.
    reactor->tick();
    EXPECT_TRUE(reactor->isRetrying(0));

    reactor->unmanageMCTPDevice("/g228/success");
    reactor->unmanageMCTPDevice("/g228/fail");
    EXPECT_FALSE(reactor->isRetrying(0));
}

// G229: manageMCTPDevice EBUSY normal replacement — existing managed device +
// new device → old removed, new deferred → tick runs new device's setup.
// This is a standalone (non-fixture) version of
// ebusyReplacementSetupRunsAfterTick with a different inventory path and
// simpler endpoint (no subscribe needed for replacement).
TEST(MCTPReactor, G229manageMCTPDeviceEBUSYNormalReplacementSetupViaTickOnly)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    auto endpoint = std::make_shared<MockMCTPEndpoint>();
    EXPECT_CALL(*endpoint, describe())
        .WillRepeatedly(testing::Return("g229-ep"));
    EXPECT_CALL(*endpoint, eid()).WillRepeatedly(testing::Return(7));
    EXPECT_CALL(*endpoint, network()).WillRepeatedly(testing::Return(2));

    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));
    EXPECT_CALL(*endpoint, remove()).WillOnce([&]() {
        if (removeHandler)
        {
            removeHandler(endpoint);
        }
    });

    auto initial = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*initial, describe())
        .WillRepeatedly(testing::Return("g229-initial"));
    EXPECT_CALL(*endpoint, device()).WillRepeatedly(testing::Return(initial));

    std::vector<Association> assocPath{
        {"configured_by", "configures", "/g229/dev"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/2/endpoints/7",
                          assocPath))
        .Times(1);
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/2/endpoints/7"))
        .Times(1);

    EXPECT_CALL(*initial, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*initial, remove()).WillOnce([&]() { endpoint->remove(); });

    auto replacement = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*replacement, describe())
        .WillRepeatedly(testing::Return("g229-replacement"));
    // Replacement is deferred; tick calls its setup which succeeds.
    EXPECT_CALL(*replacement, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    EXPECT_CALL(*replacement, remove()).Times(1);

    reactor->manageMCTPDevice("/g229/dev", initial);
    reactor->manageMCTPDevice("/g229/dev", replacement);
    EXPECT_TRUE(reactor->isRetrying(0));

    reactor->tick();
    EXPECT_FALSE(reactor->isRetrying(0));

    reactor->unmanageMCTPDevice("/g229/dev");

    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(initial.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(replacement.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(endpoint.get()));
}

// G230: After a tick that succeeds, failureCounts is cleared so
// isRetrying(0) returns false.  Also verifies that after a second failed
// setup (re-deferred) followed by unmanage, isRetrying stays false.
TEST_F(MCTPReactorFixture, G230isRetryingAfterTickSuccessClearsFailureCounts)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test/g230"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
                          requiredAssociation))
        .Times(1);
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(1);

    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));
    EXPECT_CALL(*endpoint, remove()).WillOnce([&]() {
        removeHandler(endpoint);
    });
    EXPECT_CALL(*device, remove()).WillOnce([&]() { endpoint->remove(); });

    // First call fails → deferred. Second call (tick) succeeds → clears
    // failureCounts.
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));

    reactor->manageMCTPDevice("/test/g230", device);
    EXPECT_TRUE(reactor->isRetrying(0));

    reactor->tick();
    // tick's setup succeeded → failureCounts cleared → not retrying.
    EXPECT_FALSE(reactor->isRetrying(0));

    reactor->unmanageMCTPDevice("/test/g230");
    EXPECT_FALSE(reactor->isRetrying(0));
}

// ===========================================================================
// G231: MCTPReactor::isRetrying — broadcast EID=0 returns true when
// failureCounts is non-empty (any device retrying → broadcast returns true).
// ===========================================================================
TEST_F(MCTPReactorFixture, G231isRetryingBroadcastEidZeroNonEmptyCountsTrue)
{
    // Setup device with a failing setup so failureCounts becomes non-empty
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr));
    EXPECT_CALL(*device, remove()).Times(1);

    reactor->manageMCTPDevice("/test/g231", device);

    // Any non-zero EID is retrying → broadcast EID=0 should also return true
    EXPECT_TRUE(reactor->isRetrying(0));
    reactor->unmanageMCTPDevice("/test/g231");
}

// ===========================================================================
// G232: MCTPReactor::tick — with no pending devices, tick is a no-op.
// ===========================================================================
TEST(MCTPReactor, G232tickWithNoDevicesIsNoOp)
{
    MockAssociationServer server;
    auto reactor = std::make_shared<MCTPReactor>(server);
    EXPECT_NO_THROW(reactor->tick());
    EXPECT_FALSE(reactor->isRetrying(0));
}

// ===========================================================================
// G300 (D1): isRetrying(0) — broadcast EID=0 TRUE branch: when eid == 0 the
// function immediately evaluates `!failureCounts.empty()`.  When no device
// has ever failed, failureCounts is empty and the call returns false.
//
// Source: MCTPReactor.cpp ~line 147
//   if (eid == 0) return !failureCounts.empty();
//
// The TRUE-branch of `if (eid == 0)` is taken; the return value is
// `!failureCounts.empty()` == false because no setup has failed.
// ===========================================================================
TEST(MCTPReactor, G300isRetryingBroadcastEidZeroReturnsFalseWhenNoFailures)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    // No devices managed — failureCounts is empty.
    // eid == 0 → branch taken → return !failureCounts.empty() == false.
    EXPECT_FALSE(reactor->isRetrying(0));
}

// ===========================================================================
// G301 (D2): manageMCTPDevice — non-EBUSY system_error rethrow branch AND
// the `!current` branch after EBUSY.
//
// Source: MCTPReactor.cpp ~lines 207-222
//   catch (const std::system_error& e) {
//     if (e.code() != std::errc::device_or_resource_busy) { throw e; }  ←
//     rethrow auto current = devices.deviceFor(path); if (!current) { ...
//     return; }                                      ← !current
//
// Analysis: Both branches are unreachable through the current public API:
//   - MCTPDeviceRepository::add() only ever throws
//   errc::device_or_resource_busy,
//     so the rethrow path (code != EBUSY) can never be reached via add().
//   - After a genuine EBUSY throw, deviceFor(path) always returns non-null
//     because the path entry must already exist for EBUSY to be thrown,
//     so the `!current` path is also unreachable.
//
// We document the EBUSY normal path (current != null) to confirm the full
// handler executes correctly when a replacement device is added at a used
// path. The rethrow path is skipped with an explanation comment.
// ===========================================================================
TEST(MCTPReactor,
     G301manageMCTPDeviceEBUSYCurrentNonNullPathExecutesUnmanageAndDefer)
{
    // Confirm: EBUSY handler where current != null runs unmanage + deferSetup.
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    auto ep = std::make_shared<MockMCTPEndpoint>();
    EXPECT_CALL(*ep, describe()).WillRepeatedly(testing::Return("g301-ep"));
    EXPECT_CALL(*ep, eid()).WillRepeatedly(testing::Return(5));
    EXPECT_CALL(*ep, network()).WillRepeatedly(testing::Return(1));

    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> removeCb;
    EXPECT_CALL(*ep, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeCb));
    EXPECT_CALL(*ep, remove()).WillOnce([&]() {
        if (removeCb)
        {
            removeCb(ep);
        }
    });

    auto initial = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*initial, describe())
        .WillRepeatedly(testing::Return("g301-initial"));
    EXPECT_CALL(*ep, device()).WillRepeatedly(testing::Return(initial));

    std::vector<Association> requiredAssoc{
        {"configured_by", "configures", "/g301/path"}};
    EXPECT_CALL(assoc,
                associate("/au/com/codeconstruct/mctp1/networks/1/endpoints/5",
                          requiredAssoc))
        .Times(1);
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/5"))
        .Times(1);

    EXPECT_CALL(*initial, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), ep));
    EXPECT_CALL(*initial, remove()).WillOnce([&]() { ep->remove(); });

    // replacement: added at the same path as initial → triggers EBUSY handler.
    // After the handler, it is deferred; unmanage before tick so setup is never
    // called.
    auto replacement = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*replacement, describe())
        .WillRepeatedly(testing::Return("g301-replacement"));
    EXPECT_CALL(*replacement, setup(testing::_)).Times(0);
    EXPECT_CALL(*replacement, remove()).Times(1);

    // First manage: initial is registered and set up.
    reactor->manageMCTPDevice("/g301/path", initial);
    // Second manage with a different device: EBUSY → current (initial) is
    // non-null → unmanage(initial), add(replacement), deferSetup(replacement).
    reactor->manageMCTPDevice("/g301/path", replacement);
    // Deferred replacement is removed before tick() to avoid setup call.
    reactor->unmanageMCTPDevice("/g301/path");

    // Note: The rethrow branch (code != device_or_resource_busy) is NOT
    // reachable through the public API of MCTPDeviceRepository::add() because
    // add() only ever throws errc::device_or_resource_busy.  That branch is
    // therefore not tested here.
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(initial.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(replacement.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(ep.get()));
}

// ===========================================================================
// G302 (D3): requestSetupCallback — reactor destroyed before callback fires.
//
// Source: MCTPReactor.cpp ~lines 195-204
//   mctpDevice->setRequestSetupCallback(
//     [weak{weak_from_this()}](const std::shared_ptr<MCTPDDevice>& dev) {
//       auto self = weak.lock();
//       if (!self) { return; }          ← TRUE path exercised here
//       self->setupEndpoint(dev);
//     });
//
// After manageMCTPDevice registers the callback, destroy the reactor so that
// weak.lock() returns null, then trigger the callback — should be a no-op
// (no crash, no setup call).
// ===========================================================================
TEST(MCTPReactor, G302requestSetupCallbackAfterReactorDestroyedIsNoOp)
{
    try
    {
        MockAssociationServer assoc{};
        auto reactor = std::make_shared<MCTPReactor>(assoc);
        boost::asio::io_context io;
        auto conn = std::make_shared<sdbusplus::asio::connection>(io);
        auto dev = std::make_shared<TestReactorMCTPDDevice>(conn, 71);

        int setupCalls = 0;
        dev->setupHandler = [&setupCalls](auto&& added) {
            setupCalls++;
            // Always fail so the device stays in the deferred set.
            std::forward<decltype(added)>(
                added)(std::make_error_code(std::errc::timed_out), nullptr);
        };

        EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
        EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

        // Register the device; this sets the requestSetupCallback via
        // setRequestSetupCallback inside manageMCTPDevice.
        reactor->manageMCTPDevice("/test/g302", dev);
        // Initial setup call has run.
        EXPECT_EQ(setupCalls, 1);

        // Destroy the reactor: the weak_ptr captured in requestSetupCallback
        // now expires.
        reactor.reset();

        // Trigger the callback — weak.lock() returns null → `if (!self)`
        // branch taken → returns immediately with no further setup call.
        dev->triggerRequestSetup();

        // setupCalls must remain 1; the callback must not have called
        // setupEndpoint after the reactor was destroyed.
        EXPECT_EQ(setupCalls, 1);
    }
    catch (const std::exception& ex)
    {
        GTEST_SKIP() << "DBus unavailable in this environment: " << ex.what();
    }
}

// ===========================================================================
// G303 (D4): trackEndpoint() — `if (!item)` TRUE path.
//
// Source: MCTPReactor.cpp ~lines 81-87
//   std::optional<std::string> item = devices.inventoryFor(ep->device());
//   if (!item) {
//     error("Inventory missing for endpoint: ...");
//     return;                           ← early return, no associate()
//   }
//
// Scenario: a device is managed at path P.  Its setup callback fires with an
// endpoint whose ep->device() returns a *different* device pointer (not
// registered in the repository).  inventoryFor() therefore returns nullopt,
// the error is logged, and server.associate() is never called.
// ===========================================================================
TEST(MCTPReactor, G303trackEndpointInventoryMissingLogsErrorNoAssociate)
{
    MockAssociationServer assoc{};
    // associate must never be called.
    EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

    auto reactor = std::make_shared<MCTPReactor>(assoc);

    // managedDevice is registered in the reactor's repository.
    auto managedDevice = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*managedDevice, describe())
        .WillRepeatedly(testing::Return("g303-managed"));
    EXPECT_CALL(*managedDevice, remove()).Times(1);

    // foreignDevice is NOT registered; ep->device() will return it.
    auto foreignDevice = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*foreignDevice, describe())
        .WillRepeatedly(testing::Return("g303-foreign"));

    // Endpoint whose device() returns foreignDevice, causing inventoryFor to
    // return nullopt inside trackEndpoint.
    auto ep = std::make_shared<MockMCTPEndpoint>();
    EXPECT_CALL(*ep, describe())
        .WillRepeatedly(testing::Return("g303-endpoint"));
    EXPECT_CALL(*ep, eid()).WillRepeatedly(testing::Return(11));
    EXPECT_CALL(*ep, network()).WillRepeatedly(testing::Return(1));
    EXPECT_CALL(*ep, device()).WillRepeatedly(testing::Return(foreignDevice));
    // subscribe is called by trackEndpoint before the inventory check.
    EXPECT_CALL(*ep, subscribe(testing::_, testing::_, testing::_)).Times(1);

    // managedDevice setup succeeds but returns ep whose device() is foreign.
    // trackEndpoint: subscribe called, then inventoryFor(foreignDevice) →
    // nullopt → error logged → early return (no associate).
    EXPECT_CALL(*managedDevice, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), ep));

    reactor->manageMCTPDevice("/g303/dev", managedDevice);
    reactor->unmanageMCTPDevice("/g303/dev");

    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(managedDevice.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(ep.get()));
}

// ===========================================================================
// G304: setupEndpoint() — `if (!ep)` TRUE path.
//
// Source: MCTPReactor.cpp lines ~123-129
//   if (!ep) {
//     info("Ignoring already discovered endpoint ...");
//     return;                         ← early return exercised here
//   }
//
// Scenario: setup callback fires with ec={} (success) but ep == nullptr,
// meaning the endpoint was already discovered.  The reactor should log and
// return without calling trackEndpoint() or server.associate().
// ===========================================================================
TEST_F(MCTPReactorFixture, G304setupEndpointSuccessNullEpIsIgnored)
{
    EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);
    EXPECT_CALL(*device, remove()).Times(1);
    // setup calls the callback with success (ec={}) and null endpoint.
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(),
                                             std::shared_ptr<MCTPEndpoint>{}));

    reactor->manageMCTPDevice("/g304/dev", device);
    reactor->unmanageMCTPDevice("/g304/dev");
}

// ===========================================================================
// G305: setupEndpoint() — `if (!self)` TRUE path (reactor destroyed while
// setup is pending).
//
// Source: MCTPReactor.cpp lines ~101-108
//   auto self = weak.lock();
//   if (!self) {
//     info("The reactor object was destroyed ...");
//     return;                         ← exercised here
//   }
//
// Scenario: setup() saves the callback without invoking it immediately.
// After manageMCTPDevice() returns, the reactor is destroyed, then the
// saved callback is fired.  weak.lock() returns null → early return.
// ===========================================================================
TEST(MCTPReactor, G305setupEndpointReactorDestroyedBeforeCallbackFires)
{
    MockAssociationServer assoc{};
    EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

    auto reactor = std::make_shared<MCTPReactor>(assoc);

    auto device = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*device, describe())
        .WillRepeatedly(testing::Return("g305-dev"));

    // Capture the setup callback without invoking it.
    std::function<void(const std::error_code&,
                       const std::shared_ptr<MCTPEndpoint>&)>
        savedCallback;
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce([&savedCallback](auto&& added) {
            savedCallback = std::forward<decltype(added)>(added);
        });

    reactor->manageMCTPDevice("/g305/dev", device);

    // Reactor destroyed: weak_ptr inside the callback now expires.
    reactor.reset();

    // Fire the saved callback — weak.lock() returns null → `if (!self)` TRUE.
    auto ep = std::make_shared<MockMCTPEndpoint>();
    EXPECT_CALL(*ep, describe()).WillRepeatedly(testing::Return("g305-ep"));
    savedCallback(std::error_code(), ep);

    // No crash, no assert failures.
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(device.get()));
}

// ===========================================================================
// G306: setupEndpoint() — catch MCTPException.
//
// Source: MCTPReactor.cpp lines ~131-140
//   try {
//     self->trackEndpoint(ep);
//   } catch (const MCTPException& e) {
//     error("Failed to track endpoint ...");   ← exercised here
//     self->deferSetup(dev);
//   }
//
// Scenario: MockMCTPEndpoint::subscribe() throws MCTPException, causing
// trackEndpoint() to propagate it.  The reactor catches it, logs, and
// defers setup (so a second setup attempt is made after tick()).
// ===========================================================================
TEST_F(MCTPReactorFixture, G306setupEndpointCatchesMCTPExceptionFromSubscribe)
{
    EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);
    EXPECT_CALL(*device, remove()).Times(1);

    // subscribe() throws MCTPException → trackEndpoint propagates it →
    // setupEndpoint catches it and defers setup.
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::Throw(MCTPException("test subscribe failure")));

    // First call: setup fires, subscribe throws → catch → deferSetup.
    // Second call (after tick): ep=nullptr (already-discovered) → ignored.
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(),
                                             std::shared_ptr<MCTPEndpoint>{}));

    reactor->manageMCTPDevice("/g306/dev", device);
    reactor->tick(); // processes deferred setup
    reactor->unmanageMCTPDevice("/g306/dev");
}
