#include "MCTPEndpoint.hpp"
#include "MCTPReactor.hpp"
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

    EXPECT_CALL(*endpoint, remove()).WillOnce(testing::Invoke([&]() {
        removeHandler(endpoint);
    }));
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));

    EXPECT_CALL(*device, remove()).WillOnce(testing::Invoke([&]() {
        endpoint->remove();
    }));
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

    EXPECT_CALL(*endpoint, remove()).WillOnce(testing::Invoke([&]() {
        removeHandler(endpoint);
    }));
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));

    EXPECT_CALL(*device, remove()).WillOnce(testing::Invoke([&]() {
        endpoint->remove();
    }));
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

    EXPECT_CALL(*endpoint, remove()).WillOnce(testing::Invoke([&]() {
        removeHandler(endpoint);
    }));
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .Times(2)
        .WillRepeatedly(testing::SaveArg<2>(&removeHandler));

    EXPECT_CALL(*device, remove()).WillOnce(testing::Invoke([&]() {
        endpoint->remove();
    }));
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
    EXPECT_CALL(*endpoint, remove())
        .Times(2)
        .WillRepeatedly(testing::Invoke([&]() { removeHandler(endpoint); }));
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .Times(2)
        .WillRepeatedly(testing::SaveArg<2>(&removeHandler));

    auto initial = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*initial, describe())
        .WillRepeatedly(testing::Return("mock device: initial"));
    EXPECT_CALL(*initial, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*initial, remove()).WillOnce(testing::Invoke([&]() {
        endpoint->remove();
    }));

    auto replacement = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*replacement, describe())
        .WillRepeatedly(testing::Return("mock device: replacement"));
    EXPECT_CALL(*replacement, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*replacement, remove()).WillOnce(testing::Invoke([&]() {
        endpoint->remove();
    }));

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
    EXPECT_CALL(*endpoint, remove()).WillOnce(testing::Invoke([&]() {
        removeHandler(endpoint);
    }));
    EXPECT_CALL(*device, remove()).WillOnce(testing::Invoke([&]() {
        endpoint->remove();
    }));
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
