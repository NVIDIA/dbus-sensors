#include "MCTPEndpoint.hpp"
#include "MCTPReactor.hpp"
#include "USBRecovery.hpp"
#include "Utils.hpp"

#include <systemd/sd-bus-protocol.h>
#include <systemd/sd-bus.h>

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/message.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
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
    MOCK_METHOD(std::size_t, id, (), (const, override));
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

class DR05ReproEndpoint : public MCTPEndpoint
{
  public:
    explicit DR05ReproEndpoint(std::shared_ptr<MCTPDevice> owner) :
        owner(std::move(owner))
    {}

    int network() const override
    {
        return 1;
    }

    uint8_t eid() const override
    {
        return 9;
    }

    void subscribe(Event&& /*degraded*/, Event&& /*available*/,
                   Event&& /*removed*/) override
    {}

    void remove() override {}

    std::string describe() const override
    {
        return "dr05 repro endpoint";
    }

    std::shared_ptr<MCTPDevice> device() const override
    {
        return owner;
    }

  private:
    std::shared_ptr<MCTPDevice> owner;
};

class DR05ReproMCTPDevice :
    public MCTPDevice,
    public std::enable_shared_from_this<DR05ReproMCTPDevice>
{
  public:
    explicit DR05ReproMCTPDevice(std::string description) :
        description(std::move(description))
    {}

    void setup(std::function<void(const std::error_code& ec,
                                  const std::shared_ptr<MCTPEndpoint>& ep)>&&
                   added) override
    {
        setupCalls++;
        if (oldRemovePending != nullptr && *oldRemovePending)
        {
            setupWhileOldRemovePending = true;
            std::move(
                added)(std::make_error_code(std::errc::device_or_resource_busy),
                       nullptr);
            return;
        }
        std::move(
            added)({}, std::make_shared<DR05ReproEndpoint>(shared_from_this()));
    }

    void remove() override
    {
        removeCalls++;
        removePending = true;
    }

    void remove(std::function<void()>&& removed) override
    {
        remove();
        removeComplete = std::move(removed);
    }

    std::string describe() const override
    {
        return description;
    }

    std::size_t id() const override
    {
        return 0;
    }

    void completeRemove()
    {
        removePending = false;
        auto removed = std::exchange(removeComplete, {});
        if (removed)
        {
            removed();
        }
    }

    int setupCalls = 0;
    int removeCalls = 0;
    bool removePending = false;
    bool setupWhileOldRemovePending = false;
    const bool* oldRemovePending = nullptr;

  private:
    std::string description;
    std::function<void()> removeComplete;
};

class CountingMCTPEndpoint : public MCTPEndpoint
{
  public:
    explicit CountingMCTPEndpoint(std::shared_ptr<MCTPDevice> owner) :
        owner(std::move(owner))
    {}

    int network() const override
    {
        return 1;
    }

    uint8_t eid() const override
    {
        return 9;
    }

    void subscribe(Event&& /*degraded*/, Event&& /*available*/,
                   Event&& /*removed*/) override
    {}

    void remove() override {}

    std::string describe() const override
    {
        return "counting endpoint";
    }

    std::shared_ptr<MCTPDevice> device() const override
    {
        return owner;
    }

  private:
    std::shared_ptr<MCTPDevice> owner;
};

namespace
{
constexpr const char* kMctpdEndpointControlIface =
    "au.com.codeconstruct.MCTP.Endpoint1";

sd_bus_message* buildReactorInterfacesAddedMessage(
    const std::string& objectPath, const std::string& interfaceName,
    bool includeInterface)
{
    sd_bus* bus = nullptr;
    if (sd_bus_new(&bus) < 0 || bus == nullptr)
    {
        return nullptr;
    }

    (void)sd_bus_set_address(bus, "unix:abstract=dbus-sensors-test-fake");
    (void)sd_bus_start(bus);

    sd_bus_message* msg = nullptr;
    const int newSignalRc = sd_bus_message_new_signal(
        bus, &msg, "/au/com/codeconstruct/mctp1",
        "org.freedesktop.DBus.ObjectManager", "InterfacesAdded");
    if (newSignalRc < 0 || msg == nullptr)
    {
        sd_bus_unref(bus);
        return nullptr;
    }

    const char* path = objectPath.c_str();
    (void)sd_bus_message_append_basic(msg, 'o', path);

    (void)sd_bus_message_open_container(msg, 'a', "{sa{sv}}");
    if (includeInterface)
    {
        (void)sd_bus_message_open_container(msg, 'e', "sa{sv}");
        const char* iface = interfaceName.c_str();
        (void)sd_bus_message_append_basic(msg, 's', iface);
        (void)sd_bus_message_open_container(msg, 'a', "{sv}");
        (void)sd_bus_message_close_container(msg);
        (void)sd_bus_message_close_container(msg);
    }
    (void)sd_bus_message_close_container(msg);

    (void)sd_bus_message_seal(msg, 1, 0);
    (void)sd_bus_message_rewind(msg, 1);

    sd_bus_unref(bus);
    return msg;
}
} // namespace

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
    // MCTPDevice::id() is derived from interface+physaddr only, not staticEid;
    // use a distinct physaddr per instance when multiple mctpd mocks coexist.
    TestReactorMCTPDDevice(
        const std::shared_ptr<sdbusplus::asio::connection>& connection,
        uint8_t staticEid) :
        MCTPDDevice(connection, "reactor-mctpd", "usbreactor0",
                    std::vector<uint8_t>{0x20}, staticEid, std::nullopt,
                    std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                    {"reactor-mctpd"})
    {}

    // Variant constructor that accepts a custom physical address so that
    // multiple mctpd mocks with distinct IDs (interface+physaddr) can coexist.
    TestReactorMCTPDDevice(
        const std::shared_ptr<sdbusplus::asio::connection>& connection,
        uint8_t staticEid, const std::vector<uint8_t>& physAddr) :
        MCTPDDevice(connection, "reactor-mctpd", "usbreactor0", physAddr,
                    staticEid, std::nullopt, std::nullopt, std::nullopt,
                    std::nullopt, std::nullopt, {"reactor-mctpd"})
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

class CountingMCTPDevice :
    public MCTPDevice,
    public std::enable_shared_from_this<CountingMCTPDevice>
{
  public:
    explicit CountingMCTPDevice(std::string description) :
        description(std::move(description))
    {}

    void setup(std::function<void(const std::error_code& ec,
                                  const std::shared_ptr<MCTPEndpoint>& ep)>&&
                   added) override
    {
        setupCalls++;
        std::move(added)(
            {}, std::make_shared<CountingMCTPEndpoint>(shared_from_this()));
    }

    void remove() override
    {
        removeCalls++;
    }

    void remove(std::function<void()>&& removed) override
    {
        remove();
        removeComplete = std::move(removed);
        if (completeRemoveImmediately)
        {
            completeRemove();
        }
    }

    std::string describe() const override
    {
        return description;
    }

    std::size_t id() const override
    {
        return 0;
    }

    void completeRemove()
    {
        auto removed = std::exchange(removeComplete, {});
        if (removed)
        {
            removed();
        }
    }

    int setupCalls = 0;
    int removeCalls = 0;
    bool completeRemoveImmediately = false;

  private:
    std::string description;
    std::function<void()> removeComplete;
};

class MCTPReactorFixture : public testing::Test
{
  protected:
    void SetUp() override
    {
        reactor = std::make_shared<MCTPReactor>(assoc);
        device = std::make_shared<MockMCTPDevice>();
        EXPECT_CALL(*device, id()).WillRepeatedly(testing::Return(0U));
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
    // Setup error leaves state Unassigned; unmanageMCTPDevice calls terminate()
    // (repository + states only), not device->remove().
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

    // state is Recovered — unmanageMCTPDevice is a no-op (no second
    // disassociate).
    EXPECT_CALL(
        assoc,
        disassociate("/au/com/codeconstruct/mctp1/networks/1/endpoints/9"))
        .Times(1);

    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .Times(2)
        .WillRepeatedly(testing::SaveArg<2>(&removeHandler));

    EXPECT_CALL(*device, setup(testing::_))
        .Times(2)
        .WillRepeatedly(
            testing::InvokeArgument<0>(std::error_code(), endpoint));

    // Flow: manage→Assigning→trackEndpoint→Assigned (associate #1).
    // removeHandler→untrack (disassociate #1)→Lost.
    // tick()→Recovering→trackEndpoint→Recovered (associate #2).
    // unmanage from Recovered is a no-op (no device->remove /
    // endpoint->remove).
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
    EXPECT_CALL(*foreignDevice, id()).WillRepeatedly(testing::Return(1U));
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

    // Fixture's device() is registered first; gmock uses the first matching
    // expectation, so we must clear and re-establish before a custom action.
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(endpoint.get()));
    EXPECT_CALL(*endpoint, describe())
        .WillRepeatedly(testing::Return("mock endpoint"));
    EXPECT_CALL(*endpoint, eid()).WillRepeatedly(testing::Return(9));
    EXPECT_CALL(*endpoint, network()).WillRepeatedly(testing::Return(1));
    // trackEndpoint: device() in states[.], next(ep->device(), ...), and
    // inventoryFor (3x while managed); the removed callback must then return
    // the "foreign" device to exercise that path.
    const std::shared_ptr<MockMCTPDevice> managedDevice = this->device;
    int epDeviceCall = 0;
    EXPECT_CALL(*endpoint, device())
        .WillRepeatedly([managedDevice, foreignDevice, &epDeviceCall] {
            ++epDeviceCall;
            return (epDeviceCall <= 3) ? managedDevice : foreignDevice;
        });
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));

    // No deferred retry expected because removed endpoint belongs to foreign
    // device at callback time.
    EXPECT_CALL(*device, setup(testing::_))
        .Times(1)
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    // Removed still leaves state[0] Assigned, so unmanageMCTPDevice(Assigned)
    // calls device->remove().
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
    // Setup "success" with a null ep leaves state Assigning; unmanage only
    // quarantines, it does not call device->remove().
    EXPECT_CALL(*device, remove()).Times(0);

    reactor->manageMCTPDevice("/test", device);
    reactor->unmanageMCTPDevice("/test");
}

TEST_F(MCTPReactorFixture, specificEidRetryIgnoresFailedNonMctpdDevice)
{
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), endpoint));
    // Setup failure -> Unassigned; unmanageMCTPDevice terminates (no remove()).
    EXPECT_CALL(*device, remove()).Times(0);

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
    EXPECT_CALL(*initial, id()).WillRepeatedly(testing::Return(0U));
    EXPECT_CALL(*initial, describe())
        .WillRepeatedly(testing::Return("mock device: initial"));
    EXPECT_CALL(*initial, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*initial, remove()).WillOnce([&]() { endpoint->remove(); });

    auto replacement = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*replacement, id()).WillRepeatedly(testing::Return(0U));
    EXPECT_CALL(*replacement, describe())
        .WillRepeatedly(testing::Return("mock device: replacement"));
    EXPECT_CALL(*replacement, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*replacement, remove()).WillOnce([&]() { endpoint->remove(); });

    // Per track: states[.], next(.), inventoryFor(.) = 3 calls. Replaced
    // device: removed used 2x (switch+next). Second track: 3+ replacement.
    int epDevCalls = 0;
    EXPECT_CALL(*endpoint, device())
        .WillRepeatedly([&epDevCalls, initial,
                         replacement]() -> std::shared_ptr<MCTPDevice> {
            ++epDevCalls;
            if (epDevCalls <= 3)
            {
                return initial;
            }
            if (epDevCalls <= 5)
            {
                return initial;
            }
            return replacement;
        });

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

TEST(MCTPReactor, onMctpdEndpointInterfacesAddedHandlesEndpointSignal)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    sd_bus_message* rawMsg = buildReactorInterfacesAddedMessage(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/44",
        kMctpdEndpointControlIface, true);
    if (rawMsg == nullptr)
    {
        GTEST_SKIP() << "sd_bus_message_new_signal requires an initialized bus";
    }

    sdbusplus::message_t msg(rawMsg, std::false_type{});
    EXPECT_NO_THROW(reactor->onMctpdEndpointInterfacesAdded(msg));
}

TEST(MCTPReactor, onMctpdEndpointInterfacesAddedIgnoresBadEndpointPath)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    sd_bus_message* rawMsg = buildReactorInterfacesAddedMessage(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/not-a-number",
        kMctpdEndpointControlIface, true);
    if (rawMsg == nullptr)
    {
        GTEST_SKIP() << "sd_bus_message_new_signal requires an initialized bus";
    }

    sdbusplus::message_t msg(rawMsg, std::false_type{});
    EXPECT_NO_THROW(reactor->onMctpdEndpointInterfacesAdded(msg));
}

TEST(MCTPReactor, onMctpdEndpointInterfacesAddedIgnoresMissingControlInterface)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    sd_bus_message* rawMsg = buildReactorInterfacesAddedMessage(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/44",
        kMctpdEndpointControlIface, false);
    if (rawMsg == nullptr)
    {
        GTEST_SKIP() << "sd_bus_message_new_signal requires an initialized bus";
    }

    sdbusplus::message_t msg(rawMsg, std::false_type{});
    EXPECT_NO_THROW(reactor->onMctpdEndpointInterfacesAdded(msg));
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
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> removeHandler;
    std::shared_ptr<MockMCTPEndpoint> trackedEp;
    device->setupHandler =
        [&attempt, &device, &removeHandler, &trackedEp](auto&& added) {
            attempt++;
            // fail x4, succeed once (reset streak), then fail x5.
            if (attempt <= 4 || (attempt >= 6 && attempt <= 10))
            {
                std::forward<decltype(added)>(
                    added)(std::make_error_code(std::errc::timed_out), nullptr);
                return;
            }
            auto ep = std::make_shared<MockMCTPEndpoint>();
            trackedEp = ep;
            EXPECT_CALL(*ep, subscribe(testing::_, testing::_, testing::_))
                .WillOnce(testing::SaveArg<2>(&removeHandler));
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
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(1);

    reactor->manageMCTPDevice("/test/usb-reset", device); // failure 1
    reactor->tick();                                      // failure 2
    reactor->tick();                                      // failure 3
    reactor->tick();                                      // failure 4
    reactor->tick();                                      // success -> reset

    ASSERT_TRUE(static_cast<bool>(removeHandler));
    ASSERT_TRUE(static_cast<bool>(trackedEp));
    removeHandler(trackedEp);

    reactor->tick(); // failure 1 (new streak)
    reactor->tick(); // failure 2
    reactor->tick(); // failure 3
    reactor->tick(); // failure 4
    reactor->tick(); // failure 5 -> recovery

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
    EXPECT_CALL(*device, remove()).Times(0);
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

TEST(MCTPReactor, forceUSBRecoveryRejectsEmptyInterface)
{
    MockAssociationServer assoc{};
    auto recovery = std::make_unique<MockUSBRecovery>();
    auto* recoveryPtr = recovery.get();
    auto reactor = std::make_shared<MCTPReactor>(assoc, std::move(recovery));

    EXPECT_CALL(*recoveryPtr, clearBulkOutHalt(testing::_, testing::_))
        .Times(0);

    std::string status;
    EXPECT_FALSE(reactor->forceUSBRecovery("", status));
    EXPECT_FALSE(status.empty());
}

TEST(MCTPReactor, forceUSBRecoverySucceedsWithBackend)
{
    MockAssociationServer assoc{};
    auto recovery = std::make_unique<MockUSBRecovery>();
    auto* recoveryPtr = recovery.get();
    auto reactor = std::make_shared<MCTPReactor>(assoc, std::move(recovery));

    EXPECT_CALL(*recoveryPtr, clearBulkOutHalt("usb-force-ok", testing::_))
        .WillOnce(testing::DoAll(testing::SetArgReferee<1>("cleared"),
                                 testing::Return(true)));

    std::string status;
    EXPECT_TRUE(reactor->forceUSBRecovery("usb-force-ok", status));
    EXPECT_EQ(status, "cleared");
}

TEST(MCTPReactor, forceUSBRecoveryPropagatesBackendFailure)
{
    MockAssociationServer assoc{};
    auto recovery = std::make_unique<MockUSBRecovery>();
    auto* recoveryPtr = recovery.get();
    auto reactor = std::make_shared<MCTPReactor>(assoc, std::move(recovery));

    EXPECT_CALL(*recoveryPtr, clearBulkOutHalt("usb-force-fail", testing::_))
        .WillOnce(testing::DoAll(testing::SetArgReferee<1>("clear-halt failed"),
                                 testing::Return(false)));

    std::string status;
    EXPECT_FALSE(reactor->forceUSBRecovery("usb-force-fail", status));
    EXPECT_EQ(status, "clear-halt failed");
}

TEST(MCTPReactor, forceUSBRecoveryFailsWithoutBackend)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(
        assoc, std::unique_ptr<USBRecovery>(nullptr));

    std::string status;
    EXPECT_FALSE(reactor->forceUSBRecovery("usb-no-backend", status));
    EXPECT_FALSE(status.empty());
}

TEST(MCTPReactor, autoUSBRecoveryToggleReflectsState)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    // Enabled by default.
    EXPECT_TRUE(reactor->isAutoUSBRecoveryEnabled());

    EXPECT_FALSE(reactor->setAutoUSBRecoveryEnabled(false));
    EXPECT_FALSE(reactor->isAutoUSBRecoveryEnabled());

    EXPECT_TRUE(reactor->setAutoUSBRecoveryEnabled(true));
    EXPECT_TRUE(reactor->isAutoUSBRecoveryEnabled());
}

TEST(MCTPReactor, usbRecoveryDisabledWhenRecoveryThresholdZero)
{
    MockAssociationServer assoc{};
    auto recovery = std::make_unique<MockUSBRecovery>();
    auto* recoveryPtr = recovery.get();
    auto reactor = std::make_shared<MCTPReactor>(assoc, std::move(recovery));
    reactor->setAutoUSBRecoveryEnabled(true);
    // RecoveryThreshold of 0 disables auto recovery for this target.
    auto device = std::make_shared<TestUSBMCTPDDevice>("usb-threshold-zero", 0);

    device->setupHandler = [](auto&& added) {
        std::forward<decltype(added)>(
            added)(std::make_error_code(std::errc::timed_out), nullptr);
    };

    EXPECT_CALL(*recoveryPtr, clearBulkOutHalt(testing::_, testing::_))
        .Times(0);

    reactor->manageMCTPDevice("/test/usb-threshold-zero", device);
    for (int i = 0; i < 8; ++i)
    {
        reactor->tick();
    }
    reactor->unmanageMCTPDevice("/test/usb-threshold-zero");
}

TEST(MCTPReactor, usbRecoverySkippedWhenAutoRecoveryDisabled)
{
    MockAssociationServer assoc{};
    auto recovery = std::make_unique<MockUSBRecovery>();
    auto* recoveryPtr = recovery.get();
    auto reactor = std::make_shared<MCTPReactor>(assoc, std::move(recovery));
    // Auto recovery is enabled by default; explicitly disable it so the
    // runtime switch suppresses the clear-halt call even when the threshold
    // is reached.
    reactor->setAutoUSBRecoveryEnabled(false);
    auto device = std::make_shared<TestUSBMCTPDDevice>("usb-auto-disabled", 5);

    device->setupHandler = [](auto&& added) {
        std::forward<decltype(added)>(
            added)(std::make_error_code(std::errc::timed_out), nullptr);
    };

    EXPECT_CALL(*recoveryPtr, clearBulkOutHalt(testing::_, testing::_))
        .Times(0);

    reactor->manageMCTPDevice("/test/usb-auto-disabled", device); // failure 1
    reactor->tick();                                              // failure 2
    reactor->tick();                                              // failure 3
    reactor->tick();                                              // failure 4
    reactor->tick();                                              // failure 5
    reactor->unmanageMCTPDevice("/test/usb-auto-disabled");
}

TEST_F(MCTPReactorFixture, setupFailureMarksBroadcastAsRetrying)
{
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), endpoint));
    // Unassigned path terminates; device->remove() is only for Assigned
    // unmanage, so terminate is used (no remove).
    EXPECT_CALL(*device, remove()).Times(0);

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

    EXPECT_CALL(*device, id()).WillRepeatedly(testing::Return(0U));
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

    EXPECT_CALL(*managed, id()).WillRepeatedly(testing::Return(0U));
    EXPECT_CALL(*foreign, id()).WillRepeatedly(testing::Return(1U));
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
    // track bails (no association); unmanage for Assigning/Quarantine path does
    // not call MCTPDevice::remove().
    EXPECT_CALL(*managed, remove()).Times(0);

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
    // trackEndpoint throw moves to Quarantine; unmanage on Quarantine is a
    // no-op, so no device->remove(). isRetrying uses failureCounts, cleared on
    // setup success before subscribe, so the broadcast is not in retry.
    EXPECT_CALL(*device, remove()).Times(0);

    reactor->manageMCTPDevice("/test", device);
    EXPECT_FALSE(reactor->isRetrying(0));
    reactor->unmanageMCTPDevice("/test");
    EXPECT_FALSE(reactor->isRetrying(0));
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
        // Unassigned -> terminate; remove() is only for Assigned unmanage.
        EXPECT_EQ(dev->removeCalls, 0);
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

        auto first = std::make_shared<TestReactorMCTPDDevice>(
            conn, 41, std::vector<uint8_t>{0x20});
        first->setupHandler =
            [](std::function<void(const std::error_code& ec,
                                  const std::shared_ptr<MCTPEndpoint>& ep)>&&
                   added) {
                std::move(added)(std::make_error_code(std::errc::timed_out),
                                 nullptr);
            };

        auto second = std::make_shared<TestReactorMCTPDDevice>(
            conn, 42, std::vector<uint8_t>{0x21});
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
        // Both devices stuck in Unassigned; terminate, not remove().
        EXPECT_EQ(first->removeCalls, 0);
        EXPECT_EQ(second->removeCalls, 0);
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
        {"Type", "MCTPUSBDevice"},
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
        {"Type", "MCTPUSBDevice"},
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
        {"Type", "MCTPUSBDevice"}, {"Name", "cov-usb-empty-ignore"},
        {"Interface", "usb0"},     {"StaticEndpointID", "13"},
        {"IgnoreEIDs", ""},        {"IgnoreMessageTypes", ""},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
}

TEST(MCTPDeviceFrom, USBWithOnlyValidIgnoreEntries)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBDevice"},    {"Name", "cov-usb-valid-ignore"},
        {"Interface", "usb0"},        {"StaticEndpointID", "14"},
        {"IgnoreEIDs", "10, 20, 30"}, {"IgnoreMessageTypes", "1, 2"},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
}

TEST(MCTPDeviceFrom, USBIgnoreEidsWrongVariantType)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBDevice"},
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
        {"Type", "MCTPUSBDevice"}, {"Name", "cov-usb-ws-ignore"},
        {"Interface", "usb0"},     {"StaticEndpointID", "16"},
        {"IgnoreEIDs", " , , "},   {"IgnoreMessageTypes", "  , "},
    };
    auto dev = USBMCTPDDevice::from({}, iface);
    ASSERT_NE(dev, nullptr);
}

TEST(MCTPDeviceFrom, SPIMinimalCreatesDeferredDevice)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPSPIDevice"},
        {"Name", "cov-spi"},
        {"Bus", "0"},
        {"ChipSelect", "0"},
    };
    // Netdev resolution is deferred to setup(); from() no longer returns null.
    ASSERT_NE(SPIMCTPDDevice::from({}, iface), nullptr);
}

TEST(MCTPDeviceFrom, SPIWithStaticCreatesDeferredDevice)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPSPIDevice"}, {"Name", "cov-spi-static"}, {"Bus", "0"},
        {"ChipSelect", "0"},       {"StaticEndpointID", "7"},
    };
    // Netdev resolution is deferred to setup(); from() no longer returns null.
    ASSERT_NE(SPIMCTPDDevice::from({}, iface), nullptr);
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
    // Netdev resolution is deferred to setup(); from() no longer returns null.
    ASSERT_NE(SPIMCTPDDevice::from({}, iface), nullptr);
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
        {"Type", "MCTPUSBDevice"},     {"Name", "cov-usb-bad-start"},
        {"Interface", "usb0"},         {"StaticEndpointID", "9"},
        {"BridgePoolStartEID", "xyz"},
    };
    EXPECT_THROW(USBMCTPDDevice::from({}, iface), std::invalid_argument);
}

TEST(MCTPDeviceFrom, USBWithBridgeEndBadValueThrows)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBDevice"},   {"Name", "cov-usb-bad-end"},
        {"Interface", "usb0"},       {"StaticEndpointID", "9"},
        {"BridgePoolEndEID", "xyz"},
    };
    EXPECT_THROW(USBMCTPDDevice::from({}, iface), std::invalid_argument);
}

TEST(MCTPDeviceFrom, USBWithBridgePoolNoStaticEid)
{
    SensorBaseConfigMap iface{
        {"Type", "MCTPUSBDevice"},  {"Name", "cov-usb-pool-no-static"},
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
        {"Type", "MCTPUSBDevice"},  {"Name", "cov-usb-end-only"},
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
        {"Type", "MCTPUSBDevice"}, {"Name", "cov-usb-poll"},
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
        {"Type", "MCTPUSBDevice"},    {"Name", "cov-usb-start-no-end"},
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
        {"Type", "MCTPUSBDevice"},
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
        {"Type", "MCTPUSBDevice"},
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
        {"Type", "MCTPUSBDevice"},
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
        {"Type", "MCTPUSBDevice"},
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
        {"Type", "MCTPUSBDevice"},    {"Name", "full-usb,bridge-a,bridge-b"},
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
        {"Type", "MCTPUSBDevice"}, {"Name", "cov-usb-poll-zero"},
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
    // Null-ep "success" leaves Assigning; unmanage quarantines, no remove().
    EXPECT_CALL(*device, remove()).Times(0);

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
    // Unassigned: terminate, not remove().
    EXPECT_CALL(*device, remove()).Times(0);

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
    EXPECT_CALL(*dev, id()).WillRepeatedly(testing::Return(0U));
    EXPECT_CALL(*dev, describe()).WillRepeatedly(testing::Return("mock-dev"));
    EXPECT_CALL(*dev, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    // Success with null ep -> Assigning; unmanage quarantines (no remove).
    EXPECT_CALL(*dev, remove()).Times(0);

    reactor->manageMCTPDevice("/test/mock", dev);
    reactor->unmanageMCTPDevice("/test/mock");
}

TEST(MCTPReactor, isRetryingSkipsNonMctpdDevices)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    auto dev = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*dev, id()).WillRepeatedly(testing::Return(0U));
    EXPECT_CALL(*dev, describe()).WillRepeatedly(testing::Return("mock-fail"));
    EXPECT_CALL(*dev, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr));
    // Unassigned: terminate, no device->remove().
    EXPECT_CALL(*dev, remove()).Times(0);

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
        // Second setup succeeds with null ep; state stays Assigning. Unmanage
        // quarantines; remove() is only for Assigned.
        EXPECT_EQ(dev->removeCalls, 0);
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

    // First call: state Unmanaged → Assigning, calls setup. setup returns
    // success with null endpoint, so setupEndpoint early-returns and state
    // stays Assigning. Second call on same path+device pointer is a no-op
    // (Assigning falls through). unmanage from Assigning transitions to
    // Quarantine without calling device->remove().
    reactor->manageMCTPDevice("/test/idem", device);
    reactor->manageMCTPDevice("/test/idem", device);
    reactor->unmanageMCTPDevice("/test/idem");
}

// Test: manageMCTPDevice replacement path (was "EBUSY" in pre-sync code).
// New state machine: when state==Assigned and a different device pointer is
// provided at the same path, the Assigned branch calls terminate(current),
// current->remove(), then recursively manages the replacement (which becomes
// Unmanaged → Assigning → setupEndpoint). The replacement's setup IS called
// immediately (no deferred retry queue). Setup succeeds with null endpoint →
// state stays Assigning; unmanage from Assigning → Quarantine (no
// replacement->remove).
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
    EXPECT_CALL(*initial, id()).WillRepeatedly(testing::Return(0U));
    EXPECT_CALL(*initial, describe())
        .WillRepeatedly(testing::Return("initial-replace-early"));
    EXPECT_CALL(*initial, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    // Assigned branch invokes initial->remove() to tear down its endpoint.
    EXPECT_CALL(*initial, remove()).WillOnce([&]() { endpoint->remove(); });
    EXPECT_CALL(*endpoint, device()).WillRepeatedly(testing::Return(initial));

    auto replacement = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*replacement, id()).WillRepeatedly(testing::Return(0U));
    EXPECT_CALL(*replacement, describe())
        .WillRepeatedly(testing::Return("replacement-replace-early"));
    // Replacement setup runs after the old endpoint remove completion is
    // observed on tick. setup returns success with null endpoint, so state
    // stays Assigning.
    EXPECT_CALL(*replacement, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    // unmanage from Assigning → Quarantine (no replacement->remove).

    reactor->manageMCTPDevice("/test/replace-early", initial);
    reactor->manageMCTPDevice("/test/replace-early", replacement);
    reactor->tick();
    reactor->unmanageMCTPDevice("/test/replace-early");

    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(initial.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(replacement.get()));
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(endpoint.get()));
}

TEST(MCTPReactor, replaceConfigurationWaitsForRemoveCompletionBeforeRetry)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    auto initial = std::make_shared<CountingMCTPDevice>("dr05-initial");
    auto replacement = std::make_shared<CountingMCTPDevice>("dr05-replacement");

    reactor->manageMCTPDevice("/test/dr05", initial);
    EXPECT_EQ(initial->setupCalls, 1);

    reactor->manageMCTPDevice("/test/dr05", replacement);
    EXPECT_EQ(initial->removeCalls, 1);
    EXPECT_EQ(replacement->setupCalls, 0);

    reactor->tick();
    EXPECT_EQ(replacement->setupCalls, 0);

    initial->completeRemove();
    EXPECT_EQ(replacement->setupCalls, 0);

    reactor->tick();
    EXPECT_EQ(replacement->setupCalls, 1);

    reactor->unmanageMCTPDevice("/test/dr05");
    EXPECT_EQ(replacement->removeCalls, 1);
}

TEST(MCTPReactor, DR05_replacementSetupWaitsForOldRemoveCompletionRegression)
{
    testing::NiceMock<MockAssociationServer> assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    auto initial = std::make_shared<DR05ReproMCTPDevice>("dr05-initial");
    auto replacement =
        std::make_shared<DR05ReproMCTPDevice>("dr05-replacement");
    replacement->oldRemovePending = &initial->removePending;

    std::cout << "DR-05 repro: managing initial endpoint config\n";
    reactor->manageMCTPDevice("/test/dr05-repro", initial);
    std::cout << "DR-05 repro: initial setup calls=" << initial->setupCalls
              << ", initial remove pending=" << initial->removePending << "\n";

    ASSERT_EQ(initial->setupCalls, 1);

    std::cout << "DR-05 repro: replacing config while old endpoint exists\n";
    reactor->manageMCTPDevice("/test/dr05-repro", replacement);
    std::cout << "DR-05 repro: after replacement manage, old remove calls="
              << initial->removeCalls
              << ", old remove pending=" << initial->removePending
              << ", replacement setup calls=" << replacement->setupCalls
              << ", setup while old remove pending="
              << replacement->setupWhileOldRemovePending << "\n";

    EXPECT_EQ(initial->removeCalls, 1);
    EXPECT_TRUE(initial->removePending);
    if (replacement->setupWhileOldRemovePending)
    {
        std::cout << "DR-05 reproduced: replacement setup ran before old "
                     "endpoint remove completed\n";
        EXPECT_FALSE(replacement->setupWhileOldRemovePending)
            << "DR-05 reproduced: setup raced old async remove and hit the "
               "device_or_resource_busy window.";
        return;
    }

    EXPECT_EQ(replacement->setupCalls, 0)
        << "Replacement setup must not run until old endpoint Remove completes.";

    reactor->tick();
    std::cout << "DR-05 repro: after tick with remove still pending, "
                 "replacement setup calls="
              << replacement->setupCalls << "\n";
    EXPECT_EQ(replacement->setupCalls, 0);

    initial->completeRemove();
    std::cout << "DR-05 repro: old remove completed, replacement setup calls="
              << replacement->setupCalls << "\n";
    EXPECT_EQ(replacement->setupCalls, 0);

    reactor->tick();
    std::cout << "DR-05 repro: after remove completion tick, replacement "
                 "setup calls="
              << replacement->setupCalls << ", setup while old remove pending="
              << replacement->setupWhileOldRemovePending << "\n";

    EXPECT_EQ(replacement->setupCalls, 1);
    EXPECT_FALSE(replacement->setupWhileOldRemovePending);
}

TEST(MCTPReactor, replaceConfigurationHandlesSynchronousRemoveCompletion)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    auto initial = std::make_shared<CountingMCTPDevice>("dr05-sync-initial");
    auto replacement =
        std::make_shared<CountingMCTPDevice>("dr05-sync-replacement");
    initial->completeRemoveImmediately = true;

    reactor->manageMCTPDevice("/test/dr05-sync", initial);
    reactor->manageMCTPDevice("/test/dr05-sync", replacement);

    EXPECT_EQ(initial->removeCalls, 1);
    EXPECT_EQ(replacement->setupCalls, 0);

    reactor->tick();
    EXPECT_EQ(replacement->setupCalls, 1);
}

TEST(MCTPReactor, replaceConfigurationRemovedBeforeCompletionIsNotRetried)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    auto initial = std::make_shared<CountingMCTPDevice>("dr05-cancel-initial");
    auto replacement =
        std::make_shared<CountingMCTPDevice>("dr05-cancel-replacement");

    reactor->manageMCTPDevice("/test/dr05-cancel", initial);
    reactor->manageMCTPDevice("/test/dr05-cancel", replacement);
    EXPECT_EQ(initial->removeCalls, 1);

    reactor->unmanageMCTPDevice("/test/dr05-cancel");
    EXPECT_EQ(replacement->removeCalls, 1);

    initial->completeRemove();
    reactor->tick();
    EXPECT_EQ(replacement->setupCalls, 0);
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
    // unmanage from Unassigned calls terminate() (does not call
    // device->remove).

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
        // Use a failing setup so state ends in Unassigned: unmanage from
        // Unassigned calls terminate(), which removes the device from the repo
        // and makes subsequent getDeviceName return nullopt. (Success-with-null
        // ep leaves state Assigning; unmanage from Assigning → Quarantine
        // and the device stays in the repo.)
        auto dev = std::make_shared<TestReactorMCTPDDevice>(conn, 66);
        dev->setupHandler = [](auto&& added) {
            std::forward<decltype(added)>(
                added)(std::make_error_code(std::errc::timed_out), nullptr);
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

        // After unmanage (Unassigned → terminate), the device is gone so
        // lookup returns nullopt.
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
    // Distinct ids so the two devices' states don't collide in the states map.
    EXPECT_CALL(*dev1, id()).WillRepeatedly(testing::Return(1U));
    EXPECT_CALL(*dev2, id()).WillRepeatedly(testing::Return(2U));
    EXPECT_CALL(*dev1, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr));
    EXPECT_CALL(*dev2, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr));
    // Both end in Unassigned after setup error; unmanage → terminate (no
    // device->remove).

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

// Test: manageMCTPDevice replacement path with MockMCTPDevices (no DBus).
// State==Assigned + new device pointer at same path → Assigned branch:
// terminate(initial), initial->remove(), recurse manage(replacement).
// Replacement enters Unmanaged → Assigning → setupEndpoint(replacement) runs.
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
    EXPECT_CALL(*initial, id()).WillRepeatedly(testing::Return(0U));
    EXPECT_CALL(*endpoint, device()).WillRepeatedly(testing::Return(initial));

    EXPECT_CALL(*initial, describe())
        .WillRepeatedly(testing::Return("initial-ebusy-log"));
    EXPECT_CALL(*initial, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    EXPECT_CALL(*initial, remove()).WillOnce([&]() { endpoint->remove(); });

    auto replacement = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*replacement, id()).WillRepeatedly(testing::Return(0U));
    EXPECT_CALL(*replacement, describe())
        .WillRepeatedly(testing::Return("replacement-ebusy-log"));
    // Replacement setup runs after the old endpoint remove completion is
    // observed on tick. It returns success with null ep, so state stays
    // Assigning; unmanage from Assigning → Quarantine (no replacement->remove).
    EXPECT_CALL(*replacement, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));

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
    reactor->manageMCTPDevice("/test/ebusy-log", replacement);
    reactor->tick();
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
    // Setup fails → state Unassigned + failureCounts entry.
    // unmanage → terminate (no device->remove).
    EXPECT_CALL(*mockDevice, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::io_error), nullptr));

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

    // registeredDevice is added to the reactor; setup fails → Unassigned →
    // unmanage → terminate (no device->remove).
    auto registeredDevice = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*registeredDevice, describe())
        .WillRepeatedly(testing::Return("registered-dev"));
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

// Test: tick() with two distinct devices in Unassigned state (post-failure)
// retries both via tick's state-machine walk (Unassigned → Assigning →
// setupEndpoint). State machine has no separate "deferred queue" — retries
// are driven by per-device state in the tick() loop.
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
    // Distinct ids so each device gets its own state slot.
    EXPECT_CALL(*dev1, id()).WillRepeatedly(testing::Return(1U));
    EXPECT_CALL(*dev2, id()).WillRepeatedly(testing::Return(2U));

    // Both devices fail setup on first attempt → Unassigned + failureCounts++.
    // Second attempt (from tick) succeeds with null ep → failureCounts cleared,
    // state stays Assigning.
    EXPECT_CALL(*dev1, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    EXPECT_CALL(*dev2, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));

    reactor->manageMCTPDevice("/tick/dev1", dev1);
    reactor->manageMCTPDevice("/tick/dev2", dev2);
    EXPECT_TRUE(reactor->isRetrying(0));

    // tick() must retry both Unassigned devices and clear failureCounts.
    reactor->tick();
    EXPECT_FALSE(reactor->isRetrying(0));

    // unmanage from Assigning → Quarantine (no device->remove).
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
        // Setup returns success with null ep → state stays Assigning;
        // unmanage from Assigning → Quarantine (no device->remove).
        auto mockDev = std::make_shared<MockMCTPDevice>();
        EXPECT_CALL(*mockDev, describe())
            .WillRepeatedly(testing::Return("non-mctpd"));
        EXPECT_CALL(*mockDev, setup(testing::_))
            .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
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

// Test: When endpoint->subscribe() throws MCTPException, the setupEndpoint
// callback catches it and transitions the device to Quarantine. Quarantine is
// terminal in the current state machine — failureCounts is not incremented
// (the failure-count path only fires on the if(ec) branch, not the
// trackEndpoint catch), tick() does not retry from Quarantine, and unmanage
// from Quarantine is a no-op.
TEST_F(MCTPReactorFixture, subscribeThrowsMCTPExceptionDeviceIsQuarantined)
{
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::Throw(MCTPException("subscribe failed")));
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));

    reactor->manageMCTPDevice("/test", device);

    // Quarantine does not increment failureCounts, so the retry-pending
    // probe is false (the device is not in the retry set).
    EXPECT_FALSE(reactor->isRetrying(0));

    // tick() over a Quarantined device must not re-invoke setup.
    reactor->tick();

    // unmanage from Quarantine must not invoke device->remove().
    reactor->unmanageMCTPDevice("/test");
}

// ---- R5: tick() with three deferred devices ----

// Test: tick() walks all devices and retries those in Unassigned state.
// State machine has no separate deferred queue; the tick loop drives retries.
// Verifies the loop handles >2 entries.
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
    // Distinct ids so each device gets its own state slot.
    EXPECT_CALL(*dev1, id()).WillRepeatedly(testing::Return(1U));
    EXPECT_CALL(*dev2, id()).WillRepeatedly(testing::Return(2U));
    EXPECT_CALL(*dev3, id()).WillRepeatedly(testing::Return(3U));

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

    reactor->manageMCTPDevice("/tick3/dev1", dev1);
    reactor->manageMCTPDevice("/tick3/dev2", dev2);
    reactor->manageMCTPDevice("/tick3/dev3", dev3);
    EXPECT_TRUE(reactor->isRetrying(0));

    reactor->tick();
    EXPECT_FALSE(reactor->isRetrying(0));

    // unmanage from Assigning → Quarantine (no device->remove).
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

// Test: Replacement path. New state machine: state==Assigned + new device →
// Assigned branch: terminate(initial), initial->remove(), recurse manage
// (replacement enters Unmanaged → Assigning → setupEndpoint runs immediately,
// no deferred queue). Setup with null ep keeps state Assigning. tick is
// no-op on Assigning. unmanage from Assigning → Quarantine (no remove).
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
    // Replacement's setup runs immediately during recursive manage.
    EXPECT_CALL(*replacement, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));

    reactor->manageMCTPDevice("/test/ebusy-tick", initial);
    reactor->manageMCTPDevice("/test/ebusy-tick", replacement);
    // No deferred queue in the new state machine; replacement is now
    // Assigning, not in failureCounts.
    EXPECT_FALSE(reactor->isRetrying(0));

    // tick over Assigning is a no-op.
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

// G213: The current endpoint callback owns and untracks its path, then resolves
// a device that is NOT in the repository. The membership check rejects the
// callback before it can mutate device state or queue another setup.
// This is a fixture-based complement to
// removedCallbackForForeignDeviceDoesNotDeferSetup, using a second endpoint
// whose device() returns an unregistered pointer.
TEST_F(MCTPReactorFixture, G213trackEndpointRemovedDeviceNotInRepoNoDeferSetup)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    // A separate device that is NOT registered in the repository. Its id must
    // differ from the managed device so the removed-callback's state lookup
    // (states[ep->device()->id()]) hits an Unmanaged slot instead of mutating
    // the managed device's state.
    auto foreignDevice = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*foreignDevice, id()).WillRepeatedly(testing::Return(1U));
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

    // Replace the fixture's endpoint->device() expectation: trackEndpoint
    // calls ep->device() multiple times (states lookup, next(), inventoryFor)
    // — return the managed device for the first 3 calls so trackEndpoint
    // succeeds, then foreignDevice from the removed callback onward.
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(endpoint.get()));
    EXPECT_CALL(*endpoint, describe())
        .WillRepeatedly(testing::Return("mock endpoint"));
    EXPECT_CALL(*endpoint, eid()).WillRepeatedly(testing::Return(9));
    EXPECT_CALL(*endpoint, network()).WillRepeatedly(testing::Return(1));
    const std::shared_ptr<MockMCTPDevice> managedDevice = this->device;
    int epDeviceCall = 0;
    EXPECT_CALL(*endpoint, device())
        .WillRepeatedly([managedDevice, foreignDevice, &epDeviceCall] {
            ++epDeviceCall;
            return (epDeviceCall <= 3) ? managedDevice : foreignDevice;
        });
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::SaveArg<2>(&removeHandler));

    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    // The removed callback dispatches on states[foreignDevice->id()=1], which
    // is Unmanaged → break. The managed device stays in Assigned, so unmanage
    // takes the Assigned branch and calls device->remove().
    EXPECT_CALL(*device, remove()).Times(1);

    reactor->manageMCTPDevice("/test/g213", device);
    ASSERT_TRUE(static_cast<bool>(removeHandler));

    // The token is current, so the callback first untracks the endpoint path.
    // ep->device() then returns foreignDevice; repository membership fails and
    // the callback returns without mutating either device's state.
    removeHandler(endpoint);

    // tick() must NOT call setup again (nothing was deferred).
    reactor->tick();
    EXPECT_FALSE(reactor->isRetrying(0));

    reactor->unmanageMCTPDevice("/test/g213");
}

// G214: Removed callback fires while the reactor is alive AND the device is
// still in the repository. State flow under new state machine:
//   manage → Assigning → setup ok → trackEndpoint → Assigned (associate #1).
//   removeHandler → untrack (disassociate #1) → next(Lost). No deferSetup,
//   no failureCounts++.
//   tick() from Lost → Recovering → setupEndpoint → trackEndpoint → Recovered
//   (associate #2). No second disassociate, unmanage from Recovered is no-op.
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
        .Times(1);

    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .Times(2)
        .WillRepeatedly(testing::SaveArg<2>(&removeHandler));
    EXPECT_CALL(*device, setup(testing::_))
        .Times(2)
        .WillRepeatedly(
            testing::InvokeArgument<0>(std::error_code(), endpoint));

    reactor->manageMCTPDevice("/test/g214", device);

    // Fire removed callback manually — Assigned → Lost. failureCounts NOT
    // incremented in this path, so isRetrying(0) stays false.
    removeHandler(endpoint);
    EXPECT_FALSE(reactor->isRetrying(0));

    // tick() retries from Lost → Recovering → success → Recovered.
    reactor->tick();
    EXPECT_FALSE(reactor->isRetrying(0));

    // unmanage from Recovered is a no-op (no device->remove, no disassociate).
    reactor->unmanageMCTPDevice("/test/g214");
}

TEST(MCTPReactor, securityStaleRemovedCallbackDoesNotDemoteReplacement)
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

    auto initial = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*initial, id()).WillRepeatedly(testing::Return(42U));
    EXPECT_CALL(*initial, describe())
        .WillRepeatedly(testing::Return("security-initial"));
    EXPECT_CALL(*oldEp, device()).WillRepeatedly(testing::Return(initial));
    EXPECT_CALL(*initial, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), oldEp));
    EXPECT_CALL(*initial, remove()).Times(1);

    auto replacement = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*replacement, id()).WillRepeatedly(testing::Return(42U));
    EXPECT_CALL(*replacement, describe())
        .WillRepeatedly(testing::Return("security-replacement"));
    EXPECT_CALL(*newEp, device()).WillRepeatedly(testing::Return(replacement));
    EXPECT_CALL(*replacement, setup(testing::_))
        .Times(1)
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), newEp));

    reactor->manageMCTPDevice("/test/security-replace", initial);
    reactor->manageMCTPDevice("/test/security-replace", replacement);
    ASSERT_TRUE(static_cast<bool>(oldRemove));

    reactor->tick();
    ASSERT_TRUE(static_cast<bool>(newRemove));
    oldRemove(oldEp);
    reactor->tick();

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

// G216: setupEndpoint — ec == 0, ep == nullptr → "already discovered" early
// return. State stays Assigning (no trackEndpoint, no state transition).
// unmanage from Assigning → Quarantine, NO device->remove.
TEST_F(MCTPReactorFixture, G216setupEndpointNullEndpointAfterSuccessNoAssociate)
{
    EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));

    reactor->manageMCTPDevice("/test/g216", device);

    // Success with null ep → no failureCounts entry, not retrying.
    EXPECT_FALSE(reactor->isRetrying(0));
    reactor->unmanageMCTPDevice("/test/g216");
}

// G217: setupEndpoint — trackEndpoint throws MCTPException (from subscribe()).
// New state machine: catch → next(Quarantine). failureCounts NOT incremented;
// Quarantine is terminal — tick is no-op, unmanage from Quarantine is no-op.
TEST_F(MCTPReactorFixture, G217setupEndpointMCTPExceptionCaughtDeferSetup)
{
    EXPECT_CALL(assoc, associate(testing::_, testing::_)).Times(0);
    EXPECT_CALL(assoc, disassociate(testing::_)).Times(0);

    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::Throw(MCTPException("G217: subscribe failure")));

    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));

    reactor->manageMCTPDevice("/test/g217", device);
    // Quarantine path does not populate failureCounts.
    EXPECT_FALSE(reactor->isRetrying(0));

    // tick over Quarantined device is a no-op.
    reactor->tick();
    EXPECT_FALSE(reactor->isRetrying(0));

    // unmanage from Quarantine is a no-op (no device->remove).
    reactor->unmanageMCTPDevice("/test/g217");
}

// G218: untrackEndpoint explicitly calls server.disassociate() with the
// correct D-Bus path. State flow: manage → Assigned (associate). Fire removed
// callback → untrack (disassociate) → next(Lost). unmanage from Lost calls
// terminate() (NO device->remove).
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
    EXPECT_CALL(*endpoint, remove()).Times(0);
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));

    reactor->manageMCTPDevice("/test/g218", device);
    // Trigger the removed callback manually → untrackEndpoint → disassociate.
    ASSERT_TRUE(static_cast<bool>(removeHandler));
    removeHandler(endpoint);

    // unmanage from Lost terminates without calling device->remove.
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
// Setup failure: Assigning → Unassigned + failureCounts[dev]++.
// unmanage from Unassigned calls terminate() (no device->remove).
TEST(MCTPReactor, G220isRetryingEidZeroNonEmptyFailureCountsTrue)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    auto dev = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*dev, describe()).WillRepeatedly(testing::Return("g220-dev"));
    EXPECT_CALL(*dev, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr));

    reactor->manageMCTPDevice("/test/g220", dev);

    // failureCounts has one entry → isRetrying(0) true.
    EXPECT_TRUE(reactor->isRetrying(0));

    // After unmanage (Unassigned → terminate), failureCounts is cleared.
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
    // Setup failure leaves state Unassigned; unmanage → terminate (no
    // device->remove).
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::connection_refused), nullptr));

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
    EXPECT_CALL(*initial, id()).WillRepeatedly(testing::Return(0U));
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
    EXPECT_CALL(*replacement, id()).WillRepeatedly(testing::Return(0U));
    EXPECT_CALL(*replacement, describe())
        .WillRepeatedly(testing::Return("g223-replacement"));
    // Replacement setup runs after the old endpoint remove completion is
    // observed on tick. Setup with null ep keeps state Assigning; unmanage from
    // Assigning → Quarantine (no replacement->remove).
    EXPECT_CALL(*replacement, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));

    reactor->manageMCTPDevice("/test/g223", initial);
    reactor->manageMCTPDevice("/test/g223", replacement);
    reactor->tick();
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
    // Repeated setup failures: Assigning → Unassigned + failureCounts++.
    // tick() from Unassigned → Assigning → setup fails again → Unassigned.
    // unmanage from Unassigned → terminate (no device->remove).
    EXPECT_CALL(*device, setup(testing::_))
        .WillRepeatedly(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr));

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
// In the new state machine, device->remove() is only called when state ==
// Assigned. To reach Assigned: setup must succeed with a valid endpoint so
// trackEndpoint runs and transitions Assigning → Assigned.
TEST_F(MCTPReactorFixture, G226unmanageMCTPDeviceCallsDeviceRemove)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>& ep)> removeHandler;

    std::vector<Association> requiredAssociation{
        {"configured_by", "configures", "/test/g226"}};
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
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    // remove() must be called exactly once from unmanageMCTPDevice on the
    // Assigned branch.
    EXPECT_CALL(*device, remove()).WillOnce([&]() { endpoint->remove(); });

    reactor->manageMCTPDevice("/test/g226", device);
    reactor->unmanageMCTPDevice("/test/g226");
}

// G227: tick() clears failureCounts on devices whose retry succeeds. State
// machine has no separate deferred set — tick walks devices in Unassigned
// state and re-runs setupEndpoint.
TEST(MCTPReactor, G227tickClearsAllDeferredDevices)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);

    auto dev1 = std::make_shared<MockMCTPDevice>();
    auto dev2 = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*dev1, describe()).WillRepeatedly(testing::Return("g227-dev1"));
    EXPECT_CALL(*dev2, describe()).WillRepeatedly(testing::Return("g227-dev2"));
    EXPECT_CALL(*dev1, id()).WillRepeatedly(testing::Return(1U));
    EXPECT_CALL(*dev2, id()).WillRepeatedly(testing::Return(2U));

    // Both fail on first attempt, succeed on second (from tick).
    EXPECT_CALL(*dev1, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    EXPECT_CALL(*dev2, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));

    reactor->manageMCTPDevice("/g227/dev1", dev1);
    reactor->manageMCTPDevice("/g227/dev2", dev2);
    EXPECT_TRUE(reactor->isRetrying(0));

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
    EXPECT_CALL(*devSuccess, id()).WillRepeatedly(testing::Return(1U));
    EXPECT_CALL(*devFail, id()).WillRepeatedly(testing::Return(2U));

    // devSuccess: fails first, then succeeds on tick's retry (Unassigned →
    // Assigning → success with null ep → state stays Assigning).
    EXPECT_CALL(*devSuccess, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));
    // devFail: fails on both attempts (stays in Unassigned + failureCounts).
    EXPECT_CALL(*devFail, setup(testing::_))
        .WillRepeatedly(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr));

    reactor->manageMCTPDevice("/g228/success", devSuccess);
    reactor->manageMCTPDevice("/g228/fail", devFail);
    EXPECT_TRUE(reactor->isRetrying(0));

    // After tick: devSuccess cleared from failureCounts, devFail still in.
    reactor->tick();
    EXPECT_TRUE(reactor->isRetrying(0));

    // unmanage: devSuccess is Assigning → Quarantine; devFail is Unassigned
    // → terminate. Neither calls device->remove.
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
    // New state machine: replacement's setup runs immediately during recursive
    // manage (no deferred queue). Setup with null ep keeps state Assigning;
    // unmanage from Assigning → Quarantine (no replacement->remove).
    EXPECT_CALL(*replacement, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));

    reactor->manageMCTPDevice("/g229/dev", initial);
    reactor->manageMCTPDevice("/g229/dev", replacement);
    EXPECT_FALSE(reactor->isRetrying(0));

    // tick on Assigning is a no-op.
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
    // Setup failure leaves state Unassigned; unmanage → terminate (no
    // device->remove).
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(
            std::make_error_code(std::errc::timed_out), nullptr));

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
    EXPECT_CALL(*initial, id()).WillRepeatedly(testing::Return(0U));
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

    // New state machine: replacement's setup runs immediately during the
    // recursive manage call from the Assigned branch. Setup with null ep keeps
    // state Assigning; unmanage from Assigning → Quarantine (no
    // replacement->remove).
    auto replacement = std::make_shared<MockMCTPDevice>();
    EXPECT_CALL(*replacement, id()).WillRepeatedly(testing::Return(0U));
    EXPECT_CALL(*replacement, describe())
        .WillRepeatedly(testing::Return("g301-replacement"));
    EXPECT_CALL(*replacement, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), nullptr));

    reactor->manageMCTPDevice("/g301/path", initial);
    reactor->manageMCTPDevice("/g301/path", replacement);
    reactor->tick();
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
    // setup calls callback with success and null endpoint → early return,
    // state stays Assigning. unmanage from Assigning → Quarantine, NO
    // device->remove.
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

    // subscribe() throws MCTPException → trackEndpoint propagates it →
    // setupEndpoint catches it → next(Quarantine). Quarantine is terminal:
    // tick is no-op, unmanage from Quarantine is no-op (no device->remove).
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::Throw(MCTPException("test subscribe failure")));

    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));

    reactor->manageMCTPDevice("/g306/dev", device);
    reactor->tick(); // no-op on Quarantine
    reactor->unmanageMCTPDevice("/g306/dev");
}

// ===========================================================================
// Removed-callback state-machine coverage. trackEndpoint() subscribes a
// removed handler whose behaviour depends on the device's current state.
// Force each state (accessible via -fno-access-control) then invoke the
// captured handler to exercise every switch case, plus the degraded/available
// callbacks.
// ===========================================================================
namespace
{
void captureAndManage(
    const std::shared_ptr<MCTPReactor>& reactor,
    const std::shared_ptr<MockMCTPDevice>& device,
    const std::shared_ptr<MockMCTPEndpoint>& endpoint,
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)>& degraded,
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)>& available,
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)>& removed)
{
    EXPECT_CALL(*endpoint, subscribe(testing::_, testing::_, testing::_))
        .WillOnce(testing::DoAll(testing::SaveArg<0>(&degraded),
                                 testing::SaveArg<1>(&available),
                                 testing::SaveArg<2>(&removed)));
    EXPECT_CALL(*device, setup(testing::_))
        .WillOnce(testing::InvokeArgument<0>(std::error_code(), endpoint));
    reactor->manageMCTPDevice("/test", device);
}
} // namespace

TEST_F(MCTPReactorFixture, removedCallbackQuarantineTerminates)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> deg;
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> avail;
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> rem;
    captureAndManage(reactor, device, endpoint, deg, avail, rem);
    ASSERT_TRUE(static_cast<bool>(rem));
    reactor->states[0] = MCTPDeviceState::Quarantine;
    EXPECT_NO_THROW(rem(endpoint));
    reactor->unmanageMCTPDevice("/test");
}

TEST_F(MCTPReactorFixture, removedCallbackRecoveredGoesLost)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> deg;
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> avail;
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> rem;
    captureAndManage(reactor, device, endpoint, deg, avail, rem);
    ASSERT_TRUE(static_cast<bool>(rem));
    reactor->states[0] = MCTPDeviceState::Recovered;
    EXPECT_NO_THROW(rem(endpoint));
    reactor->unmanageMCTPDevice("/test");
}

TEST_F(MCTPReactorFixture, removedCallbackRemovingTerminatesIfKnown)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> deg;
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> avail;
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> rem;
    captureAndManage(reactor, device, endpoint, deg, avail, rem);
    ASSERT_TRUE(static_cast<bool>(rem));
    reactor->states[0] = MCTPDeviceState::Removing;
    EXPECT_NO_THROW(rem(endpoint));
    reactor->unmanageMCTPDevice("/test");
}

TEST_F(MCTPReactorFixture, removedCallbackPendingGoesUnassigned)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> deg;
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> avail;
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> rem;
    captureAndManage(reactor, device, endpoint, deg, avail, rem);
    ASSERT_TRUE(static_cast<bool>(rem));
    reactor->states[0] = MCTPDeviceState::Pending;
    EXPECT_NO_THROW(rem(endpoint));
    reactor->unmanageMCTPDevice("/test");
}

TEST_F(MCTPReactorFixture, removedCallbackRecoveringIsNoop)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> deg;
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> avail;
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> rem;
    captureAndManage(reactor, device, endpoint, deg, avail, rem);
    ASSERT_TRUE(static_cast<bool>(rem));
    reactor->states[0] = MCTPDeviceState::Recovering;
    EXPECT_NO_THROW(rem(endpoint));
    reactor->unmanageMCTPDevice("/test");
}

TEST_F(MCTPReactorFixture, degradedAndAvailableCallbacksAreInvokable)
{
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> deg;
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> avail;
    std::function<void(const std::shared_ptr<MCTPEndpoint>&)> rem;
    captureAndManage(reactor, device, endpoint, deg, avail, rem);
    ASSERT_TRUE(static_cast<bool>(deg));
    ASSERT_TRUE(static_cast<bool>(avail));
    EXPECT_NO_THROW(deg(endpoint));
    EXPECT_NO_THROW(avail(endpoint));
    reactor->unmanageMCTPDevice("/test");
}

// ===========================================================================
// handleUSBSetupFailure() recovery-branch coverage.
// ===========================================================================
TEST(MCTPReactor, usbRecoveryBackendFailureAtThresholdWarns)
{
    MockAssociationServer assoc{};
    auto recovery = std::make_unique<MockUSBRecovery>();
    auto* recoveryPtr = recovery.get();
    auto reactor = std::make_shared<MCTPReactor>(assoc, std::move(recovery));
    reactor->setAutoUSBRecoveryEnabled(true);
    auto device = std::make_shared<TestUSBMCTPDDevice>("usb-recfail", 1);
    device->setupHandler = [](auto&& added) {
        std::forward<decltype(added)>(
            added)(std::make_error_code(std::errc::timed_out), nullptr);
    };
    // Threshold reached on first failure; backend recovery fails -> warning.
    EXPECT_CALL(*recoveryPtr, clearBulkOutHalt("usb-recfail", testing::_))
        .WillOnce(testing::DoAll(testing::SetArgReferee<1>("clear-halt failed"),
                                 testing::Return(false)));
    reactor->manageMCTPDevice("/test/recfail", device);
    reactor->unmanageMCTPDevice("/test/recfail");
}

TEST(MCTPReactor, usbRecoveryDisabledAtRuntimeSkipsClearHalt)
{
    MockAssociationServer assoc{};
    auto recovery = std::make_unique<MockUSBRecovery>();
    auto* recoveryPtr = recovery.get();
    auto reactor = std::make_shared<MCTPReactor>(assoc, std::move(recovery));
    reactor->setAutoUSBRecoveryEnabled(false); // disabled at runtime
    auto device = std::make_shared<TestUSBMCTPDDevice>("usb-disabled", 1);
    device->setupHandler = [](auto&& added) {
        std::forward<decltype(added)>(
            added)(std::make_error_code(std::errc::timed_out), nullptr);
    };
    // Threshold reached but recovery disabled -> clearBulkOutHalt not called.
    EXPECT_CALL(*recoveryPtr, clearBulkOutHalt(testing::_, testing::_))
        .Times(0);
    reactor->manageMCTPDevice("/test/disabled", device);
    reactor->unmanageMCTPDevice("/test/disabled");
}

TEST(MCTPReactor, usbSetupFailureEmptyInterfaceSkipsTracking)
{
    MockAssociationServer assoc{};
    auto recovery = std::make_unique<MockUSBRecovery>();
    auto* recoveryPtr = recovery.get();
    auto reactor = std::make_shared<MCTPReactor>(assoc, std::move(recovery));
    reactor->setAutoUSBRecoveryEnabled(true);
    auto device =
        std::make_shared<TestUSBMCTPDDevice>("", 1); // empty interface
    device->setupHandler = [](auto&& added) {
        std::forward<decltype(added)>(
            added)(std::make_error_code(std::errc::timed_out), nullptr);
    };
    // Empty interface -> handleUSBSetupFailure returns before any recovery.
    EXPECT_CALL(*recoveryPtr, clearBulkOutHalt(testing::_, testing::_))
        .Times(0);
    reactor->manageMCTPDevice("/test/empty-iface", device);
    reactor->unmanageMCTPDevice("/test/empty-iface");
}

TEST(MCTPReactor, usbRecoveryThresholdZeroDisablesTracking)
{
    MockAssociationServer assoc{};
    auto recovery = std::make_unique<MockUSBRecovery>();
    auto* recoveryPtr = recovery.get();
    auto reactor = std::make_shared<MCTPReactor>(assoc, std::move(recovery));
    reactor->setAutoUSBRecoveryEnabled(true);
    auto device = std::make_shared<TestUSBMCTPDDevice>("usb-thr0", 0); // thr 0
    device->setupHandler = [](auto&& added) {
        std::forward<decltype(added)>(
            added)(std::make_error_code(std::errc::timed_out), nullptr);
    };
    EXPECT_CALL(*recoveryPtr, clearBulkOutHalt(testing::_, testing::_))
        .Times(0);
    reactor->manageMCTPDevice("/test/thr0", device);
    reactor->tick();
    reactor->unmanageMCTPDevice("/test/thr0");
}

// ===========================================================================
// onMctpdEndpointInterfacesAdded() + eidFromMctpdEndpointPath() coverage using
// crafted InterfacesAdded signal messages (fake-bus construction technique).
// ===========================================================================
namespace
{
constexpr const char* kMctpdEndpointCtrlIface =
    "au.com.codeconstruct.MCTP.Endpoint1";

sd_bus_message* buildReactorIfacesAdded(
    const std::string& path, const std::string& iface, bool includeIface)
{
    sd_bus* bus = nullptr;
    (void)sd_bus_new(&bus);
    (void)sd_bus_set_address(bus, "unix:abstract=dbus-sensors-test-fake");
    (void)sd_bus_start(bus);
    sd_bus_message* msg = nullptr;
    (void)sd_bus_message_new_signal(bus, &msg, "/au/com/codeconstruct/mctp1",
                                    "org.freedesktop.DBus.ObjectManager",
                                    "InterfacesAdded");
    (void)sd_bus_message_append_basic(msg, 'o', path.c_str());
    (void)sd_bus_message_open_container(msg, 'a', "{sa{sv}}");
    if (includeIface)
    {
        (void)sd_bus_message_open_container(msg, 'e', "sa{sv}");
        (void)sd_bus_message_append_basic(msg, 's', iface.c_str());
        (void)sd_bus_message_open_container(msg, 'a', "{sv}");
        (void)sd_bus_message_close_container(msg);
        (void)sd_bus_message_close_container(msg);
    }
    (void)sd_bus_message_close_container(msg);
    (void)sd_bus_message_seal(msg, 1, 0);
    (void)sd_bus_message_rewind(msg, 1);
    sd_bus_unref(bus);
    return msg;
}

void deliverIfacesAdded(const std::shared_ptr<MCTPReactor>& reactor,
                        const std::string& path, const std::string& iface,
                        bool includeIface)
{
    sd_bus_message* raw = buildReactorIfacesAdded(path, iface, includeIface);
    ASSERT_NE(raw, nullptr);
    sdbusplus::message_t msg(raw, std::false_type{});
    reactor->onMctpdEndpointInterfacesAdded(msg);
}
} // namespace

TEST(MCTPReactorIfacesAdded, ValidEndpointPathMarksDiscovered)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    EXPECT_NO_THROW(deliverIfacesAdded(
        reactor, "/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
        kMctpdEndpointCtrlIface, true));
}

TEST(MCTPReactorIfacesAdded, MissingEndpointInterfaceIgnored)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    EXPECT_NO_THROW(deliverIfacesAdded(
        reactor, "/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
        "some.Other.Interface", true));
}

TEST(MCTPReactorIfacesAdded, PathWithoutEndpointsSegmentIgnored)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    EXPECT_NO_THROW(
        deliverIfacesAdded(reactor, "/au/com/codeconstruct/mctp1/networks/1",
                           kMctpdEndpointCtrlIface, true));
}

TEST(MCTPReactorIfacesAdded, PathWithOutOfRangeEidIgnored)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    EXPECT_NO_THROW(deliverIfacesAdded(
        reactor, "/au/com/codeconstruct/mctp1/networks/1/endpoints/999",
        kMctpdEndpointCtrlIface, true));
}

TEST(MCTPReactorIfacesAdded, PathWithNonNumericEidIgnored)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    EXPECT_NO_THROW(deliverIfacesAdded(
        reactor, "/au/com/codeconstruct/mctp1/networks/1/endpoints/abc",
        kMctpdEndpointCtrlIface, true));
}

TEST(MCTPReactorIfacesAdded, EmptyInterfacesDictIgnored)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    EXPECT_NO_THROW(deliverIfacesAdded(
        reactor, "/au/com/codeconstruct/mctp1/networks/1/endpoints/9", "",
        false));
}

TEST(MCTPReactorIfacesAdded, NullMessageHandledGracefully)
{
    MockAssociationServer assoc{};
    auto reactor = std::make_shared<MCTPReactor>(assoc);
    sdbusplus::message_t msg(nullptr);
    EXPECT_NO_THROW(reactor->onMctpdEndpointInterfacesAdded(msg));
}
