/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "MCTPBridgePoolDevice.hpp"
#include "MCTPEndpoint.hpp"
#include "Utils.hpp"
#include "async_test_helpers.hpp"

#include <systemd/sd-bus-protocol.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/message.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>

#include <gtest/gtest.h>

// gFakeSdBusFd, gMockSdBusCallSuccess, gSyncCallHandler and gTestSdBusInterface
// are declared in async_test_helpers.hpp and defined in sd_bus_wrappers.cpp.

namespace
{
// Configuration for a scripted resolveBridge()/setup() D-Bus conversation.
struct ResolveScript
{
    const char* bridgePath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/8";
    const char* emName = "bridge0"; // trailing segment of configured_by target
    uint8_t poolStart = 10;
    uint8_t poolEnd = 20;
    bool providePoolEnd = true;
    bool poolStartFails = false; // model Bridge1.PoolStart not yet populated
    bool subtreeThrows = false;  // model GetSubTree failure
    bool configuredByPathIsBare = false;
    bool introspectThrows = false;
    bool introspectHasEndpoint = true; // warm-boot: endpoint already present
    int poolGetCount = 0;              // internal: PoolStart then PoolEnd
};

// Build a GetSubTree reply "a{sa{sas}}" with a single bridge path exposing the
// Bridge1 interface.
int buildSubtreeReply(sd_bus_message* req, sd_bus_message** reply,
                      const char* path)
{
    sd_bus_message_new_method_return(req, reply);
    sd_bus_message* r = *reply;
    sd_bus_message_open_container(r, 'a', "{sa{sas}}");
    sd_bus_message_open_container(r, 'e', "sa{sas}");
    sd_bus_message_append_basic(r, 's', path);
    sd_bus_message_open_container(r, 'a', "{sas}");
    sd_bus_message_open_container(r, 'e', "sas");
    sd_bus_message_append_basic(r, 's', "au.com.codeconstruct.MCTP1");
    sd_bus_message_open_container(r, 'a', "s");
    sd_bus_message_append_basic(r, 's', "au.com.codeconstruct.MCTP.Bridge1");
    sd_bus_message_close_container(r);
    sd_bus_message_close_container(r);
    sd_bus_message_close_container(r);
    sd_bus_message_close_container(r);
    sd_bus_message_close_container(r);
    sd_bus_message_seal(r, 2, 0);
    return 0;
}

// Build a Properties.Get reply for configured_by "endpoints": variant<as> with
// a single EM object path whose trailing segment is emName.
int buildConfiguredByReply(sd_bus_message* req, sd_bus_message** reply,
                           const char* emName, bool pathIsBare)
{
    std::string emPath =
        pathIsBare ? std::string(emName)
                   : std::string("/xyz/openbmc_project/inventory/") + emName;
    sd_bus_message_new_method_return(req, reply);
    sd_bus_message* r = *reply;
    sd_bus_message_open_container(r, 'v', "as");
    sd_bus_message_open_container(r, 'a', "s");
    sd_bus_message_append_basic(r, 's', emPath.c_str());
    sd_bus_message_close_container(r);
    sd_bus_message_close_container(r);
    sd_bus_message_seal(r, 2, 0);
    return 0;
}

// Build a Properties.Get reply for a uint8_t property: variant<y>.
int buildByteVariantReply(sd_bus_message* req, sd_bus_message** reply,
                          uint8_t value)
{
    sd_bus_message_new_method_return(req, reply);
    sd_bus_message* r = *reply;
    sd_bus_message_open_container(r, 'v', "y");
    sd_bus_message_append_basic(r, 'y', &value);
    sd_bus_message_close_container(r);
    sd_bus_message_seal(r, 2, 0);
    return 0;
}

// Build an Introspect reply: an XML string, optionally advertising Endpoint1.
int buildIntrospectReply(sd_bus_message* req, sd_bus_message** reply,
                         bool hasEndpoint)
{
    const char* xml =
        hasEndpoint
            ? "<node><interface name=\"au.com.codeconstruct.MCTP.Endpoint1\"/></node>"
            : "<node><interface name=\"org.freedesktop.DBus.Peer\"/></node>";
    sd_bus_message_new_method_return(req, reply);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    sd_bus_message_append(*reply, "s", xml);
    sd_bus_message_seal(*reply, 2, 0);
    return 0;
}

// Assemble a gSyncCallHandler that scripts the full resolveBridge()/setup()
// conversation described by `s`. Dispatches by request member/path.
std::function<int(sd_bus_message*, sd_bus_message**)> makeResolveHandler(
    const std::shared_ptr<ResolveScript>& s)
{
    return [s](sd_bus_message* req, sd_bus_message** reply) -> int {
        // The real sd_bus_call (which seals the outgoing message) is replaced
        // by the wrap, so seal the request here before building a reply from
        // it. Sealing an already-sealed message is a harmless no-op error.
        (void)sd_bus_message_seal(req, 1, 0);
        const char* member = sd_bus_message_get_member(req);
        const char* path = sd_bus_message_get_path(req);
        if (member == nullptr)
        {
            return -EINVAL;
        }
        if (std::strcmp(member, "GetSubTree") == 0)
        {
            if (s->subtreeThrows)
            {
                return -ENOTSUP;
            }
            return buildSubtreeReply(req, reply, s->bridgePath);
        }
        if (std::strcmp(member, "Introspect") == 0)
        {
            if (s->introspectThrows)
            {
                return -ENOTSUP;
            }
            return buildIntrospectReply(req, reply, s->introspectHasEndpoint);
        }
        if (std::strcmp(member, "Get") == 0)
        {
            if (path != nullptr &&
                std::string(path).ends_with("/configured_by"))
            {
                return buildConfiguredByReply(req, reply, s->emName,
                                              s->configuredByPathIsBare);
            }
            // Bridge1 PoolStart (first) then PoolEnd (second).
            int idx = s->poolGetCount++;
            if (idx == 0)
            {
                if (s->poolStartFails)
                {
                    return -ENOTSUP;
                }
                return buildByteVariantReply(req, reply, s->poolStart);
            }
            if (!s->providePoolEnd)
            {
                return -ENOTSUP;
            }
            return buildByteVariantReply(req, reply, s->poolEnd);
        }
        return -ENOTSUP;
    };
}
} // namespace

// ===========================================================================
// Static match() helpers — no connection required.
// ===========================================================================

TEST(BridgePoolMCTPDevice, matchInterfacesRelevant)
{
    std::set<std::string> interfaces{
        "xyz.openbmc_project.Configuration.MCTPBridgePoolDevice"};
    EXPECT_TRUE(BridgePoolMCTPDevice::match(interfaces));
}

TEST(BridgePoolMCTPDevice, matchInterfacesIrrelevant)
{
    std::set<std::string> interfaces{
        "xyz.openbmc_project.Configuration.SomeOtherType"};
    EXPECT_FALSE(BridgePoolMCTPDevice::match(interfaces));
}

TEST(BridgePoolMCTPDevice, matchInterfacesEmpty)
{
    EXPECT_FALSE(BridgePoolMCTPDevice::match(std::set<std::string>{}));
}

TEST(BridgePoolMCTPDevice, matchConfigRelevant)
{
    SensorData config{{"xyz.openbmc_project.Configuration.MCTPBridgePoolDevice",
                       {{"Type", std::string("MCTPBridgePoolDevice")},
                        {"Name", std::string("pool0")},
                        {"BridgeName", std::string("bridge0")},
                        {"PoolIndex", std::string("0")}}}};
    auto result = BridgePoolMCTPDevice::match(config);
    EXPECT_TRUE(result.has_value());
}

TEST(BridgePoolMCTPDevice, matchConfigIrrelevant)
{
    SensorData config{{"xyz.openbmc_project.Configuration.NVME1000", {}}};
    EXPECT_FALSE(BridgePoolMCTPDevice::match(config).has_value());
}

TEST(BridgePoolMCTPDevice, matchConfigEmpty)
{
    EXPECT_FALSE(BridgePoolMCTPDevice::match(SensorData{}).has_value());
}

// ===========================================================================
// from() factory — construction never needs a live bus (the connection is only
// stored), so a null connection is sufficient to exercise every branch.
// ===========================================================================

class BridgePoolFromTest : public ::testing::Test
{
  protected:
    std::shared_ptr<sdbusplus::asio::connection> conn = nullptr;
};

TEST_F(BridgePoolFromTest, fromMissingTypeThrows)
{
    SensorBaseConfigMap iface{{"Name", std::string("pool0")},
                              {"BridgeName", std::string("bridge0")},
                              {"PoolIndex", std::string("0")}};
    EXPECT_THROW(BridgePoolMCTPDevice::from(conn, iface),
                 std::invalid_argument);
}

TEST_F(BridgePoolFromTest, fromWrongTypeThrows)
{
    SensorBaseConfigMap iface{{"Type", std::string("SomeOtherType")},
                              {"Name", std::string("pool0")},
                              {"BridgeName", std::string("bridge0")},
                              {"PoolIndex", std::string("0")}};
    EXPECT_THROW(BridgePoolMCTPDevice::from(conn, iface),
                 std::invalid_argument);
}

TEST_F(BridgePoolFromTest, fromMissingNameThrows)
{
    SensorBaseConfigMap iface{{"Type", std::string("MCTPBridgePoolDevice")},
                              {"BridgeName", std::string("bridge0")},
                              {"PoolIndex", std::string("0")}};
    EXPECT_THROW(BridgePoolMCTPDevice::from(conn, iface),
                 std::invalid_argument);
}

TEST_F(BridgePoolFromTest, fromMissingBridgeNameThrows)
{
    SensorBaseConfigMap iface{{"Type", std::string("MCTPBridgePoolDevice")},
                              {"Name", std::string("pool0")},
                              {"PoolIndex", std::string("0")}};
    EXPECT_THROW(BridgePoolMCTPDevice::from(conn, iface),
                 std::invalid_argument);
}

TEST_F(BridgePoolFromTest, fromMissingPoolIndexThrows)
{
    SensorBaseConfigMap iface{{"Type", std::string("MCTPBridgePoolDevice")},
                              {"Name", std::string("pool0")},
                              {"BridgeName", std::string("bridge0")}};
    EXPECT_THROW(BridgePoolMCTPDevice::from(conn, iface),
                 std::invalid_argument);
}

TEST_F(BridgePoolFromTest, fromBadPoolIndexThrows)
{
    SensorBaseConfigMap iface{{"Type", std::string("MCTPBridgePoolDevice")},
                              {"Name", std::string("pool0")},
                              {"BridgeName", std::string("bridge0")},
                              {"PoolIndex", std::string("notanumber")}};
    EXPECT_THROW(BridgePoolMCTPDevice::from(conn, iface),
                 std::invalid_argument);
}

TEST_F(BridgePoolFromTest, fromValidConfigCreatesDevice)
{
    SensorBaseConfigMap iface{{"Type", std::string("MCTPBridgePoolDevice")},
                              {"Name", std::string("pool0")},
                              {"BridgeName", std::string("bridge0")},
                              {"PoolIndex", std::string("3")}};
    auto dev = BridgePoolMCTPDevice::from(conn, iface);
    ASSERT_NE(dev, nullptr);
    // poolIndex 3, unresolved -> eid reported as -1 in describe.
    EXPECT_NE(dev->describe().find("bridge0"), std::string::npos);
}

TEST_F(BridgePoolFromTest, fromPoolIndexAsUint64CreatesDevice)
{
    SensorBaseConfigMap iface{{"Type", std::string("MCTPBridgePoolDevice")},
                              {"Name", std::string("pool0")},
                              {"BridgeName", std::string("bridge0")},
                              {"PoolIndex", uint64_t{5}}};
    auto dev = BridgePoolMCTPDevice::from(conn, iface);
    ASSERT_NE(dev, nullptr);
}

// ===========================================================================
// Instance methods — fake (unconnected) connection.  Synchronous calls made by
// setup()/resolveBridge() throw internally and are caught, driving the "defer"
// paths.  Private members/methods are reachable thanks to -fno-access-control.
// ===========================================================================

class BridgePoolFakeConnTest : public ::testing::Test
{
  protected:
    std::array<int, 2> fds{-1, -1};
    boost::asio::io_context io;
    std::shared_ptr<sdbusplus::asio::connection> conn;

    void SetUp() override
    {
        ASSERT_EQ(pipe(fds.data()), 0);
        gFakeSdBusFd = fds[0];
        conn = std::make_shared<sdbusplus::asio::connection>(io, nullptr);
    }

    void TearDown() override
    {
        io.restart();
        io.poll();
        conn.reset();
        io.stop();
        close(fds[0]);
        close(fds[1]);
        gFakeSdBusFd = -1;
        gMockSdBusCallSuccess = false;
        gSyncCallHandler = nullptr;
    }

    std::shared_ptr<BridgePoolMCTPDevice> makeDevice(uint8_t poolIndex = 0)
    {
        return std::make_shared<BridgePoolMCTPDevice>(conn, "pool0", "bridge0",
                                                      poolIndex);
    }
};

TEST_F(BridgePoolFakeConnTest, describeUnresolvedReportsNegativeEid)
{
    auto dev = makeDevice(2);
    const auto text = dev->describe();
    EXPECT_NE(text.find("BridgePool"), std::string::npos);
    EXPECT_NE(text.find("bridge0"), std::string::npos);
    EXPECT_NE(text.find("eid=-1"), std::string::npos);
}

TEST_F(BridgePoolFakeConnTest, describeResolvedReportsEid)
{
    auto dev = makeDevice(2);
    dev->networkId = 1;
    dev->bridgedEid = 12;
    EXPECT_NE(dev->describe().find("eid=12"), std::string::npos);
}

TEST_F(BridgePoolFakeConnTest, networkAndEidDefaultZeroWhenUnresolved)
{
    auto dev = makeDevice();
    EXPECT_EQ(dev->network(), 0);
    EXPECT_EQ(dev->eid(), 0);
}

TEST_F(BridgePoolFakeConnTest, networkAndEidReflectResolvedValues)
{
    auto dev = makeDevice();
    dev->networkId = 7;
    dev->bridgedEid = 42;
    EXPECT_EQ(dev->network(), 7);
    EXPECT_EQ(dev->eid(), 42);
}

TEST_F(BridgePoolFakeConnTest, getNameForEidMatchesResolvedEid)
{
    auto dev = makeDevice();
    dev->bridgedEid = 20;
    EXPECT_EQ(dev->getNameForEid(20).value_or(""), "pool0");
}

TEST_F(BridgePoolFakeConnTest, getNameForEidNoMatchReturnsNullopt)
{
    auto dev = makeDevice();
    dev->bridgedEid = 20;
    EXPECT_FALSE(dev->getNameForEid(21).has_value());
    // Unresolved bridgedEid -> always nullopt.
    auto dev2 = makeDevice();
    EXPECT_FALSE(dev2->getNameForEid(0).has_value());
}

TEST_F(BridgePoolFakeConnTest, idIsStableAndDependsOnFields)
{
    auto a = makeDevice(1);
    auto b = makeDevice(1);
    EXPECT_EQ(a->id(), b->id());
    auto c = makeDevice(2);
    EXPECT_NE(a->id(), c->id());
}

TEST_F(BridgePoolFakeConnTest, subscribeStoresRemovedCallback)
{
    auto dev = makeDevice();
    bool called = false;
    dev->subscribe(
        [](const std::shared_ptr<MCTPEndpoint>&) {},
        [](const std::shared_ptr<MCTPEndpoint>&) {},
        [&called](const std::shared_ptr<MCTPEndpoint>&) { called = true; });
    // remove() should invoke the stored removed callback.
    dev->remove();
    EXPECT_TRUE(called);
}

TEST_F(BridgePoolFakeConnTest, removeWithoutSubscribeDoesNotCrash)
{
    auto dev = makeDevice();
    EXPECT_NO_THROW(dev->remove());
}

TEST_F(BridgePoolFakeConnTest, removePropagatesSubscriberError)
{
    auto dev = makeDevice();
    dev->subscribe({}, {}, [](const std::shared_ptr<MCTPEndpoint>&) {
        throw std::runtime_error("subscriber failed");
    });

    EXPECT_THROW(dev->remove(), std::runtime_error);
}

TEST_F(BridgePoolFakeConnTest, deviceReturnsSelf)
{
    auto dev = makeDevice();
    EXPECT_EQ(dev->device().get(), dev.get());
}

TEST_F(BridgePoolFakeConnTest, predictedEndpointPathUsesResolvedValues)
{
    auto dev = makeDevice();
    dev->networkId = 3;
    dev->bridgedEid = 30;
    EXPECT_EQ(dev->predictedEndpointPath(),
              "/au/com/codeconstruct/mctp1/networks/3/endpoints/30");
}

TEST_F(BridgePoolFakeConnTest, notifyEndpointPresentFiresSetupCallbackOnce)
{
    auto dev = makeDevice();
    int calls = 0;
    dev->setupCallback = [&calls](const std::error_code& ec,
                                  const std::shared_ptr<MCTPEndpoint>& ep) {
        ++calls;
        EXPECT_FALSE(ec);
        EXPECT_NE(ep, nullptr);
    };
    dev->notifyEndpointPresent();
    EXPECT_EQ(calls, 1);
    // Callback was exchanged to null: a second call is a no-op.
    dev->notifyEndpointPresent();
    EXPECT_EQ(calls, 1);
}

TEST_F(BridgePoolFakeConnTest, notifyEndpointPresentWithoutCallbackIsNoop)
{
    auto dev = makeDevice();
    EXPECT_NO_THROW(dev->notifyEndpointPresent());
}

TEST_F(BridgePoolFakeConnTest,
       notifyEndpointPresentPropagatesCallbackErrorAndClearsCallback)
{
    auto dev = makeDevice();
    dev->setupCallback =
        [](const std::error_code&, const std::shared_ptr<MCTPEndpoint>&) {
            throw std::runtime_error("setup callback failed");
        };

    EXPECT_THROW(dev->notifyEndpointPresent(), std::runtime_error);
    EXPECT_FALSE(dev->setupCallback);
    EXPECT_NO_THROW(dev->notifyEndpointPresent());
}

TEST_F(BridgePoolFakeConnTest, armWatchesDoesNotCrashOnFakeConn)
{
    auto dev = makeDevice();
    dev->networkId = 1;
    dev->bridgedEid = 10;
    // match_t creation goes through libsdbusplus (not --wrap intercepted); it
    // may throw on the fake bus.  Either way the function body is entered.
    try
    {
        dev->armWatches();
    }
    catch (const std::exception& e)
    {
        GTEST_LOG_(INFO) << "tolerated exception: " << e.what();
    }
}

TEST_F(BridgePoolFakeConnTest, resolveBridgeDefersWhenSubtreeCallThrows)
{
    auto dev = makeDevice();
    // gMockSdBusCallSuccess=false -> sd_bus_call returns -ENOTSUP -> the
    // GetSubTree call throws -> resolveBridge catches and defers.
    EXPECT_FALSE(dev->resolveBridge());
    EXPECT_FALSE(dev->resolved);
}

TEST_F(BridgePoolFakeConnTest, resolveBridgeDefersWhenSubtreeReplyEmpty)
{
    auto dev = makeDevice();
    // gMockSdBusCallSuccess=true -> new_method_call succeeds and sd_bus_call
    // returns a null reply; reply.read(subtree) throws -> defer.
    gMockSdBusCallSuccess = true;
    EXPECT_FALSE(dev->resolveBridge());
}

TEST_F(BridgePoolFakeConnTest, setupDefersAndReportsHostUnreachable)
{
    auto dev = makeDevice();
    std::error_code captured;
    bool invoked = false;
    dev->setup([&](const std::error_code& ec,
                   const std::shared_ptr<MCTPEndpoint>& ep) {
        invoked = true;
        captured = ec;
        EXPECT_EQ(ep, nullptr);
    });
    EXPECT_TRUE(invoked);
    EXPECT_EQ(captured, std::make_error_code(std::errc::host_unreachable));
}

TEST_F(BridgePoolFakeConnTest, setupWithoutCallbackDefersSafely)
{
    auto dev = makeDevice();
    std::function<void(const std::error_code&,
                       const std::shared_ptr<MCTPEndpoint>&)>
        callback;

    EXPECT_NO_THROW(dev->setup(std::move(callback)));
    EXPECT_FALSE(dev->resolved);
}

TEST_F(BridgePoolFakeConnTest, onEndpointAddedNullMsgThrows)
{
    auto dev = makeDevice();
    auto msg = sdbusplus::message_t(nullptr);
    EXPECT_ANY_THROW(dev->onEndpointAdded(msg));
}

TEST_F(BridgePoolFakeConnTest, onEndpointRemovedNullMsgThrows)
{
    auto dev = makeDevice();
    auto msg = sdbusplus::message_t(nullptr);
    EXPECT_ANY_THROW(dev->onEndpointRemoved(msg));
}

// ===========================================================================
// resolveBridge()/setup() success-path coverage. gSyncCallHandler scripts the
// GetSubTree + configured_by + PoolStart/PoolEnd + Introspect conversation so
// the full resolution loop, bounds checks, and warm-boot publish run.
// ===========================================================================

TEST_F(BridgePoolFakeConnTest, resolveBridgeFullSuccessComputesEid)
{
    auto s = std::make_shared<ResolveScript>();
    s->poolStart = 10;
    s->poolEnd = 20;
    gSyncCallHandler = makeResolveHandler(s);

    auto dev = makeDevice(2); // poolIndex 2 -> bridgedEid = 12
    EXPECT_TRUE(dev->resolveBridge());
    EXPECT_TRUE(dev->resolved);
    ASSERT_TRUE(dev->networkId.has_value());
    EXPECT_EQ(dev->networkId.value_or(-1), 1);
    ASSERT_TRUE(dev->bridgedEid.has_value());
    EXPECT_EQ(static_cast<int>(dev->bridgedEid.value_or(0)), 12);
}

TEST_F(BridgePoolFakeConnTest, resolveBridgeWithoutPoolEndDefers)
{
    auto s = std::make_shared<ResolveScript>();
    // PoolStart and PoolEnd are published together by mctpd; an absent PoolEnd
    // means resolution must defer so the bounds check is never skipped.
    s->providePoolEnd = false;
    gSyncCallHandler = makeResolveHandler(s);

    auto dev = makeDevice(1);
    EXPECT_FALSE(dev->resolveBridge());
    EXPECT_FALSE(dev->resolved);
}

TEST_F(BridgePoolFakeConnTest, resolveBridgePoolIndexOutOfRangeDefers)
{
    auto s = std::make_shared<ResolveScript>();
    s->poolStart = 10;
    s->poolEnd = 12; // range size 2
    gSyncCallHandler = makeResolveHandler(s);

    auto dev = makeDevice(9); // poolIndex 9 > (12-10) -> out of range
    EXPECT_FALSE(dev->resolveBridge());
    EXPECT_FALSE(dev->resolved);
}

TEST_F(BridgePoolFakeConnTest, resolveBridgeReversedPoolBoundsDefers)
{
    auto s = std::make_shared<ResolveScript>();
    s->poolStart = 20;
    s->poolEnd = 10;
    gSyncCallHandler = makeResolveHandler(s);

    auto dev = makeDevice();
    EXPECT_FALSE(dev->resolveBridge());
    EXPECT_FALSE(dev->resolved);
}

// configured_by target reported as a bare EM name (no inventory path prefix)
// still matches the bridge name, exercising the bare-path branch of resolve.
TEST_F(BridgePoolFakeConnTest, resolveBridgeBareConfiguredByNameMatches)
{
    auto s = std::make_shared<ResolveScript>();
    s->configuredByPathIsBare = true;
    gSyncCallHandler = makeResolveHandler(s);

    auto dev = makeDevice(2);
    EXPECT_TRUE(dev->resolveBridge());
    EXPECT_TRUE(dev->resolved);
    EXPECT_EQ(dev->bridgedEid.value_or(0), 12);
}

TEST_F(BridgePoolFakeConnTest, resolveBridgeConfiguredByMismatchDefers)
{
    auto s = std::make_shared<ResolveScript>();
    s->emName = "some-other-bridge"; // does not match "bridge0"
    gSyncCallHandler = makeResolveHandler(s);

    auto dev = makeDevice();
    EXPECT_FALSE(dev->resolveBridge());
}

TEST_F(BridgePoolFakeConnTest, resolveBridgePoolStartMissingDefers)
{
    auto s = std::make_shared<ResolveScript>();
    s->poolStartFails = true; // Bridge1.PoolStart not yet populated
    gSyncCallHandler = makeResolveHandler(s);

    auto dev = makeDevice();
    EXPECT_FALSE(dev->resolveBridge());
}

TEST_F(BridgePoolFakeConnTest, resolveBridgeUnparseableNetworkDefers)
{
    auto s = std::make_shared<ResolveScript>();
    // Path lacks the /networks/<N>/endpoints/ structure.
    s->bridgePath = "/au/com/codeconstruct/mctp1/badpath";
    gSyncCallHandler = makeResolveHandler(s);

    auto dev = makeDevice();
    EXPECT_FALSE(dev->resolveBridge());
}

TEST_F(BridgePoolFakeConnTest, resolveBridgeMissingEndpointsSegmentDefers)
{
    auto s = std::make_shared<ResolveScript>();
    // Keep this a valid D-Bus object path so the scripted calls reach the
    // network parser.
    s->bridgePath = "/au/com/codeconstruct/mctp1/networks/1/bridges/8";
    gSyncCallHandler = makeResolveHandler(s);

    auto dev = makeDevice();
    EXPECT_FALSE(dev->resolveBridge());
    EXPECT_EQ(s->poolGetCount, 2);
}

TEST_F(BridgePoolFakeConnTest, resolveBridgeNonNumericNetworkDefers)
{
    auto s = std::make_shared<ResolveScript>();
    s->bridgePath =
        "/au/com/codeconstruct/mctp1/networks/not-a-number/endpoints/8";
    gSyncCallHandler = makeResolveHandler(s);

    auto dev = makeDevice();
    EXPECT_FALSE(dev->resolveBridge());
}

TEST_F(BridgePoolFakeConnTest, resolveBridgePartiallyNumericNetworkDefers)
{
    auto s = std::make_shared<ResolveScript>();
    s->bridgePath = "/au/com/codeconstruct/mctp1/networks/1x/endpoints/8";
    gSyncCallHandler = makeResolveHandler(s);

    auto dev = makeDevice();
    EXPECT_FALSE(dev->resolveBridge());
}

// setup() drives resolveBridge() then armWatches() (match creation) then the
// Introspect probe. Match creation goes through sdbusplus' SdBusImpl virtual
// dispatch, which --wrap cannot reach, so use a TestSdBusInterface-backed
// connection (whose sd_bus_add_match succeeds and whose sd_bus_call delegates
// to gSyncCallHandler) rather than the plain fake connection.
class BridgePoolAsyncFixture : public ::testing::Test
{
  protected:
    std::array<int, 2> fds{-1, -1};
    boost::asio::io_context io;
    std::shared_ptr<sdbusplus::asio::connection> conn;

    void SetUp() override
    {
        ASSERT_EQ(pipe(fds.data()), 0);
        gFakeSdBusFd = fds[0];
        conn = std::make_shared<sdbusplus::asio::connection>(
            io, sdbusplus::bus_t(nullptr, &gTestSdBusInterface));
        gMockSdBusCallSuccess = true;
    }

    void TearDown() override
    {
        io.restart();
        io.poll();
        conn.reset();
        io.stop();
        close(fds[0]);
        close(fds[1]);
        gFakeSdBusFd = -1;
        gMockSdBusCallSuccess = false;
        gSyncCallHandler = nullptr;
    }

    std::shared_ptr<BridgePoolMCTPDevice> makeDevice(uint8_t poolIndex = 0)
    {
        return std::make_shared<BridgePoolMCTPDevice>(conn, "pool0", "bridge0",
                                                      poolIndex);
    }
};

TEST_F(BridgePoolAsyncFixture, setupSuccessEndpointPresentPublishes)
{
    auto s = std::make_shared<ResolveScript>();
    s->introspectHasEndpoint = true; // warm boot: endpoint already exists
    gSyncCallHandler = makeResolveHandler(s);

    auto dev = makeDevice(2);
    bool invoked = false;
    std::error_code captured{std::make_error_code(std::errc::io_error)};
    std::shared_ptr<MCTPEndpoint> gotEp;
    dev->setup([&](const std::error_code& ec,
                   const std::shared_ptr<MCTPEndpoint>& ep) {
        invoked = true;
        captured = ec;
        gotEp = ep;
    });
    EXPECT_TRUE(invoked);
    EXPECT_FALSE(captured);
    EXPECT_EQ(gotEp.get(), dev.get());
}

TEST_F(BridgePoolAsyncFixture, setupSuccessEndpointAbsentWaits)
{
    auto s = std::make_shared<ResolveScript>();
    s->introspectHasEndpoint = false; // endpoint not present yet -> wait
    gSyncCallHandler = makeResolveHandler(s);

    auto dev = makeDevice(2);
    bool invoked = false;
    dev->setup([&](const std::error_code&,
                   const std::shared_ptr<MCTPEndpoint>&) { invoked = true; });
    // Resolution succeeded but endpoint absent: callback deferred to the watch.
    EXPECT_FALSE(invoked);
    EXPECT_TRUE(dev->resolved);
}

TEST_F(BridgePoolAsyncFixture, setupIntrospectFailureWaitsForWatch)
{
    auto s = std::make_shared<ResolveScript>();
    s->introspectThrows = true;
    gSyncCallHandler = makeResolveHandler(s);

    auto dev = makeDevice(2);
    bool invoked = false;
    dev->setup([&](const std::error_code&,
                   const std::shared_ptr<MCTPEndpoint>&) { invoked = true; });

    EXPECT_FALSE(invoked);
    EXPECT_TRUE(dev->resolved);
    EXPECT_TRUE(dev->setupCallback);
}

// ===========================================================================
// onEndpointAdded()/onEndpointRemoved() branch coverage with real signal
// messages, plus the armWatches() match-callback lambdas. Uses the same fake-
// bus signal-construction technique proven in test_MCTPCustomDevices.
// ===========================================================================
namespace
{
constexpr const char* kEndpointCtrlIface =
    "au.com.codeconstruct.MCTP.Endpoint1";

sd_bus_message* buildIfacesAdded(const std::string& path,
                                 const std::string& iface, bool includeIface)
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

sd_bus_message* buildIfacesRemoved(const std::string& path,
                                   const std::string& iface, bool includeIface)
{
    sd_bus* bus = nullptr;
    (void)sd_bus_new(&bus);
    (void)sd_bus_set_address(bus, "unix:abstract=dbus-sensors-test-fake");
    (void)sd_bus_start(bus);
    sd_bus_message* msg = nullptr;
    (void)sd_bus_message_new_signal(bus, &msg, "/au/com/codeconstruct/mctp1",
                                    "org.freedesktop.DBus.ObjectManager",
                                    "InterfacesRemoved");
    (void)sd_bus_message_append_basic(msg, 'o', path.c_str());
    (void)sd_bus_message_open_container(msg, 'a', "s");
    if (includeIface)
    {
        (void)sd_bus_message_append_basic(msg, 's', iface.c_str());
    }
    (void)sd_bus_message_close_container(msg);
    (void)sd_bus_message_seal(msg, 1, 0);
    (void)sd_bus_message_rewind(msg, 1);
    sd_bus_unref(bus);
    return msg;
}
} // namespace

TEST_F(BridgePoolFakeConnTest, onEndpointAddedMatchingPathPublishes)
{
    auto dev = makeDevice(2);
    dev->networkId = 1;
    dev->bridgedEid = 12;
    int calls = 0;
    dev->setupCallback =
        [&calls](const std::error_code&, const std::shared_ptr<MCTPEndpoint>&) {
            ++calls;
        };
    sd_bus_message* raw = buildIfacesAdded(dev->predictedEndpointPath(),
                                           kEndpointCtrlIface, true);
    ASSERT_NE(raw, nullptr);
    sdbusplus::message_t msg(raw, std::false_type{});
    dev->onEndpointAdded(msg);
    EXPECT_EQ(calls, 1);
}

TEST_F(BridgePoolFakeConnTest, onEndpointAddedWrongPathIgnored)
{
    auto dev = makeDevice(2);
    dev->networkId = 1;
    dev->bridgedEid = 12;
    int calls = 0;
    dev->setupCallback =
        [&calls](const std::error_code&, const std::shared_ptr<MCTPEndpoint>&) {
            ++calls;
        };
    sd_bus_message* raw =
        buildIfacesAdded("/au/com/codeconstruct/mctp1/networks/1/endpoints/99",
                         kEndpointCtrlIface, true);
    ASSERT_NE(raw, nullptr);
    sdbusplus::message_t msg(raw, std::false_type{});
    dev->onEndpointAdded(msg);
    EXPECT_EQ(calls, 0);
}

TEST_F(BridgePoolFakeConnTest, onEndpointAddedMissingInterfaceIgnored)
{
    auto dev = makeDevice(2);
    dev->networkId = 1;
    dev->bridgedEid = 12;
    int calls = 0;
    dev->setupCallback =
        [&calls](const std::error_code&, const std::shared_ptr<MCTPEndpoint>&) {
            ++calls;
        };
    sd_bus_message* raw = buildIfacesAdded(dev->predictedEndpointPath(),
                                           "some.Other.Iface", true);
    ASSERT_NE(raw, nullptr);
    sdbusplus::message_t msg(raw, std::false_type{});
    dev->onEndpointAdded(msg);
    EXPECT_EQ(calls, 0);
}

TEST_F(BridgePoolFakeConnTest, onEndpointRemovedMatchingPathNotifies)
{
    auto dev = makeDevice(2);
    dev->networkId = 1;
    dev->bridgedEid = 12;
    int removed = 0;
    dev->subscribe({}, {}, [&removed](const std::shared_ptr<MCTPEndpoint>&) {
        ++removed;
    });
    sd_bus_message* raw = buildIfacesRemoved(dev->predictedEndpointPath(),
                                             kEndpointCtrlIface, true);
    ASSERT_NE(raw, nullptr);
    sdbusplus::message_t msg(raw, std::false_type{});
    dev->onEndpointRemoved(msg);
    EXPECT_EQ(removed, 1);
}

TEST_F(BridgePoolFakeConnTest,
       onEndpointRemovedMatchingPathWithoutSubscriberIsSafe)
{
    auto dev = makeDevice(2);
    dev->networkId = 1;
    dev->bridgedEid = 12;
    sd_bus_message* raw = buildIfacesRemoved(dev->predictedEndpointPath(),
                                             kEndpointCtrlIface, true);
    ASSERT_NE(raw, nullptr);
    sdbusplus::message_t msg(raw, std::false_type{});

    EXPECT_NO_THROW(dev->onEndpointRemoved(msg));
}

TEST_F(BridgePoolFakeConnTest, onEndpointRemovedWrongPathIgnored)
{
    auto dev = makeDevice(2);
    dev->networkId = 1;
    dev->bridgedEid = 12;
    int removed = 0;
    dev->subscribe({}, {}, [&removed](const std::shared_ptr<MCTPEndpoint>&) {
        ++removed;
    });
    sd_bus_message* raw = buildIfacesRemoved(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/77",
        kEndpointCtrlIface, true);
    ASSERT_NE(raw, nullptr);
    sdbusplus::message_t msg(raw, std::false_type{});
    dev->onEndpointRemoved(msg);
    EXPECT_EQ(removed, 0);
}

TEST_F(BridgePoolFakeConnTest, onEndpointRemovedMissingInterfaceIgnored)
{
    auto dev = makeDevice(2);
    dev->networkId = 1;
    dev->bridgedEid = 12;
    int removed = 0;
    dev->subscribe({}, {}, [&removed](const std::shared_ptr<MCTPEndpoint>&) {
        ++removed;
    });
    sd_bus_message* raw =
        buildIfacesRemoved(dev->predictedEndpointPath(), "other.Iface", true);
    ASSERT_NE(raw, nullptr);
    sdbusplus::message_t msg(raw, std::false_type{});
    dev->onEndpointRemoved(msg);
    EXPECT_EQ(removed, 0);
}

// armWatches() registers Added/Removed match callbacks; drive them via the
// stored match _callback with a matching message to cover the lambda bodies.
TEST_F(BridgePoolAsyncFixture, armWatchesCallbacksInvokeHandlers)
{
    auto dev = makeDevice(2);
    dev->networkId = 1;
    dev->bridgedEid = 12;
    int added = 0;
    int removed = 0;
    dev->setupCallback =
        [&added](const std::error_code&, const std::shared_ptr<MCTPEndpoint>&) {
            ++added;
        };
    dev->subscribe({}, {}, [&removed](const std::shared_ptr<MCTPEndpoint>&) {
        ++removed;
    });
    dev->armWatches();
    ASSERT_TRUE(dev->endpointAddedMatch);
    ASSERT_TRUE(dev->endpointRemovedMatch);
    {
        sd_bus_message* raw = buildIfacesAdded(dev->predictedEndpointPath(),
                                               kEndpointCtrlIface, true);
        ASSERT_NE(raw, nullptr);
        sdbusplus::message_t msg(raw, std::false_type{});
        (*dev->endpointAddedMatch->_callback)(msg);
    }
    {
        sd_bus_message* raw = buildIfacesRemoved(dev->predictedEndpointPath(),
                                                 kEndpointCtrlIface, true);
        ASSERT_NE(raw, nullptr);
        sdbusplus::message_t msg(raw, std::false_type{});
        (*dev->endpointRemovedMatch->_callback)(msg);
    }
    EXPECT_EQ(added, 1);
    EXPECT_EQ(removed, 1);
}

TEST_F(BridgePoolAsyncFixture, armWatchesCallbacksIgnoreExpiredDevice)
{
    auto dev = makeDevice(2);
    dev->networkId = 1;
    dev->bridgedEid = 12;
    const std::string endpointPath = dev->predictedEndpointPath();
    dev->armWatches();
    ASSERT_TRUE(dev->endpointAddedMatch);
    ASSERT_TRUE(dev->endpointRemovedMatch);

    auto addedMatch = std::move(dev->endpointAddedMatch);
    auto removedMatch = std::move(dev->endpointRemovedMatch);
    std::weak_ptr<BridgePoolMCTPDevice> weakDevice = dev;
    dev.reset();
    ASSERT_TRUE(weakDevice.expired());

    {
        sd_bus_message* raw =
            buildIfacesAdded(endpointPath, kEndpointCtrlIface, true);
        ASSERT_NE(raw, nullptr);
        sdbusplus::message_t msg(raw, std::false_type{});
        EXPECT_NO_THROW((*addedMatch->_callback)(msg));
    }
    {
        sd_bus_message* raw =
            buildIfacesRemoved(endpointPath, kEndpointCtrlIface, true);
        ASSERT_NE(raw, nullptr);
        sdbusplus::message_t msg(raw, std::false_type{});
        EXPECT_NO_THROW((*removedMatch->_callback)(msg));
    }
}
