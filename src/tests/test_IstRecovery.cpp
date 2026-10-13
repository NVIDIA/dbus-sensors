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
#include "IstRecovery.hpp"

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace
{

constexpr const char* istService = "com.nvidia.vera.ist";
constexpr const char* istPath = "/com/nvidia/vera/ist";
constexpr const char* istStateIface = "com.nvidia.vera.ist.State";
constexpr const char* istInProgressProperty = "IstInProgress";
constexpr const char* unrelatedProperty = "RunPhase";
constexpr const char* mapperService = "xyz.openbmc_project.ObjectMapper";
constexpr const char* mapperPath = "/xyz/openbmc_project/object_mapper";
constexpr const char* mapperInterface = "xyz.openbmc_project.ObjectMapper";

using GetObjectReply =
    std::vector<std::pair<std::string, std::vector<std::string>>>;

constexpr auto pumpTimeout = std::chrono::seconds(5);
constexpr auto pumpSlice = std::chrono::milliseconds(50);
constexpr auto settleTime = std::chrono::milliseconds(250);

/**
 * Drives DBusIstRecovery against a real session bus, which the unit-test
 * runner provides via DBUS_SESSION_BUS_ADDRESS. The IST service is published
 * on a second connection so the PropertiesChanged and NameOwnerChanged
 * signals the code matches on are genuine broadcasts from another owner
 * rather than messages the client sent to itself.
 *
 * The service appearing and disappearing is modelled by taking and releasing
 * the bus name, not by building up and tearing down the serving connection:
 * sdbusplus::asio::connection posts its read loop bound to a raw this, so a
 * destroyed connection leaves a dangling handler that the next pump would
 * run.
 */
class IstRecoveryTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        clientBus = connect();
        serverBus = connect();
        if (clientBus == nullptr || serverBus == nullptr)
        {
            GTEST_SKIP() << "no session bus: " << connectError;
        }

        server =
            std::make_unique<sdbusplus::asio::object_server>(serverBus, true);
        istIface = server->add_interface(istPath, istStateIface);
        istIface->register_property(istInProgressProperty, false);
        istIface->register_property(unrelatedProperty, std::string("idle"));
        istIface->initialize();

        // Answers from a fixed table rather than tracking the name, so the
        // tests can exercise the owner match against a service that has not
        // started yet.
        mapperIface = server->add_interface(mapperPath, mapperInterface);
        mapperIface->register_method(
            "GetObject", [this](const std::string& path,
                                const std::vector<std::string>& interfaces) {
                return getObject(path, interfaces);
            });
        mapperIface->initialize();
        serverBus->request_name(mapperService);
    }

    void TearDown() override
    {
        recovery.reset();
        withdrawIstService();
        drain();
    }

    void publishIstService()
    {
        serverBus->request_name(istService);
        serviceOwned = true;
    }

    void withdrawIstService()
    {
        if (!serviceOwned)
        {
            return;
        }
        serviceOwned = false;
        try
        {
            sdbusplus::message_t msg = serverBus->new_method_call(
                "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "ReleaseName");
            msg.append(std::string(istService));
            serverBus->call(msg);
        }
        catch (const std::exception& e)
        {
            ADD_FAILURE() << "could not release " << istService << ": "
                          << e.what();
        }
    }

    void hideServiceFromMapper()
    {
        mapperKnowsService = false;
    }

    void failMapperLookup()
    {
        mapperAnswers = false;
    }

    void setInProgress(bool inProgress)
    {
        istIface->set_property(istInProgressProperty, inProgress);
    }

    void setUnrelated(const std::string& phase)
    {
        istIface->set_property(unrelatedProperty, phase);
    }

    // PropertiesChanged carrying IstInProgress as a string. object_server
    // cannot publish a property under two types, so the signal is built by
    // hand.
    void emitIstInProgressAsString()
    {
        sdbusplus::message_t msg = serverBus->new_signal(
            istPath, "org.freedesktop.DBus.Properties", "PropertiesChanged");
        msg.append(std::string(istStateIface),
                   std::vector<
                       std::pair<std::string, std::variant<bool, std::string>>>{
                       {istInProgressProperty, std::string("yes")}},
                   std::vector<std::string>{});
        msg.signal_send();
    }

    // The match filters on the interface name alone, so a signal whose body
    // does not follow PropertiesChanged still reaches the handler, where the
    // whole decode fails rather than one property being skipped.
    void emitUndecodablePropertiesChanged()
    {
        sdbusplus::message_t msg = serverBus->new_signal(
            istPath, "org.freedesktop.DBus.Properties", "PropertiesChanged");
        msg.append(std::string(istStateIface), uint32_t{1});
        msg.signal_send();
    }

    bool istInProgress() const
    {
        return recovery->isIstInProgress();
    }

    // Runs the shared io_context until done() holds; false on timeout.
    bool pumpUntil(const std::function<bool()>& done)
    {
        const auto deadline = std::chrono::steady_clock::now() + pumpTimeout;
        while (!done())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            pumpOnce();
        }
        return true;
    }

    // Lets any pending signal or reply land when the expectation is that
    // nothing changes, so the assertion cannot pass merely by racing ahead.
    void pumpFor(std::chrono::milliseconds duration)
    {
        const auto deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline)
        {
            pumpOnce();
        }
    }

  private:
    GetObjectReply getObject(
        const std::string& path,
        const std::vector<std::string>& /*interfaces*/) const
    {
        if (!mapperAnswers)
        {
            throw std::runtime_error("path is not in the object tree");
        }
        if (!mapperKnowsService || path != istPath)
        {
            return {};
        }
        return {{istService, {istStateIface}}};
    }

    std::shared_ptr<sdbusplus::asio::connection> connect()
    {
        try
        {
            return std::make_shared<sdbusplus::asio::connection>(
                io, sdbusplus::bus::new_user());
        }
        catch (const std::exception& e)
        {
            connectError = e.what();
            return nullptr;
        }
    }

    void pumpOnce()
    {
        if (io.stopped())
        {
            io.restart();
        }
        io.run_one_for(pumpSlice);
    }

    void drain()
    {
        if (io.stopped())
        {
            io.restart();
        }
        io.poll();
    }

    // Declaration order is destruction order reversed: the io_context is
    // first so it outlives every connection holding an executor from it, and
    // the served interface outlives nothing that still needs it.
    boost::asio::io_context io;
    std::shared_ptr<sdbusplus::asio::connection> serverBus;
    std::unique_ptr<sdbusplus::asio::object_server> server;
    std::shared_ptr<sdbusplus::asio::dbus_interface> istIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> mapperIface;
    std::string connectError;
    bool serviceOwned = false;
    bool mapperKnowsService = true;
    bool mapperAnswers = true;

  protected:
    std::shared_ptr<sdbusplus::asio::connection> clientBus;
    std::unique_ptr<DBusIstRecovery> recovery;
};

// The flag starts false, so reaching true proves the lookup and the seeding
// read that follows it both completed.
TEST_F(IstRecoveryTest, SeedsCachedFlagFromRunningService)
{
    publishIstService();
    setInProgress(true);
    recovery = std::make_unique<DBusIstRecovery>(clientBus);

    EXPECT_TRUE(pumpUntil([this] { return istInProgress(); }));
}

// False is where the flag starts, so waiting on it reaching false would return
// without pumping and leave the lookup and the read behind it unfinished.
// Settling is what makes this an assertion about the failure path.
TEST_F(IstRecoveryTest, ClearsCachedFlagWhenServiceIsAbsent)
{
    recovery = std::make_unique<DBusIstRecovery>(clientBus);

    pumpFor(settleTime);

    EXPECT_FALSE(istInProgress());
}

TEST_F(IstRecoveryTest, TracksPropertyChangesInBothDirections)
{
    publishIstService();
    recovery = std::make_unique<DBusIstRecovery>(clientBus);
    pumpFor(settleTime);
    ASSERT_FALSE(istInProgress());

    setInProgress(true);
    EXPECT_TRUE(pumpUntil([this] { return istInProgress(); }));

    setInProgress(false);
    EXPECT_TRUE(pumpUntil([this] { return !istInProgress(); }));
}

TEST_F(IstRecoveryTest, IgnoresChangesToOtherPropertiesOnTheInterface)
{
    publishIstService();
    recovery = std::make_unique<DBusIstRecovery>(clientBus);
    pumpFor(settleTime);
    ASSERT_FALSE(istInProgress());

    setUnrelated("running");
    pumpFor(settleTime);

    EXPECT_FALSE(istInProgress());
}

// Losing the name must clear the flag: a run cannot still be in progress once
// the service that was running it is gone.
TEST_F(IstRecoveryTest, ClearsCachedFlagWhenServiceStops)
{
    publishIstService();
    setInProgress(true);
    recovery = std::make_unique<DBusIstRecovery>(clientBus);
    ASSERT_TRUE(pumpUntil([this] { return istInProgress(); }));

    withdrawIstService();

    EXPECT_TRUE(pumpUntil([this] { return !istInProgress(); }));
}

// A late-starting IST service must be re-read, otherwise mctpreactor would
// latch false and clear-halt a CPU that is mid-run.
TEST_F(IstRecoveryTest, RereadsPropertyWhenServiceStarts)
{
    // Set before the client is listening: a PropertiesChanged seen by the
    // state match would update the flag without the owner match reading it,
    // which is the path this test exists to cover.
    setInProgress(true);
    recovery = std::make_unique<DBusIstRecovery>(clientBus);

    // Settling is what installs the owner match, so the name being taken below
    // arrives as a change rather than as the state it started in.
    pumpFor(settleTime);
    ASSERT_FALSE(istInProgress());

    publishIstService();

    EXPECT_TRUE(pumpUntil([this] { return istInProgress(); }));
}

// A value of the wrong type must not be read as "no run in progress": that
// would clear the flag and allow a clear-halt mid-run.
TEST_F(IstRecoveryTest, IgnoresNonBooleanPropertyValue)
{
    publishIstService();
    setInProgress(true);
    recovery = std::make_unique<DBusIstRecovery>(clientBus);
    ASSERT_TRUE(pumpUntil([this] { return istInProgress(); }));

    emitIstInProgressAsString();
    pumpFor(settleTime);

    EXPECT_TRUE(istInProgress());
}

// Models a service that announced a run and then stopped answering: it never
// takes the name, which is what the refresh read fails against.
TEST_F(IstRecoveryTest, RefreshReleasesStaleInProgressFlag)
{
    recovery = std::make_unique<DBusIstRecovery>(clientBus);
    pumpFor(settleTime);
    ASSERT_FALSE(istInProgress());

    setInProgress(true);
    ASSERT_TRUE(pumpUntil([this] { return istInProgress(); }));

    recovery->refreshState();

    EXPECT_TRUE(pumpUntil([this] { return !istInProgress(); }));
}

// The decision point refreshes right after it suppresses, so a refresh that
// cleared unconditionally would release the suppression before the next
// threshold and clear-halt the CPU mid-run.
TEST_F(IstRecoveryTest, RefreshKeepsFlagWhileServiceStillReportsARun)
{
    publishIstService();
    setInProgress(true);
    recovery = std::make_unique<DBusIstRecovery>(clientBus);
    ASSERT_TRUE(pumpUntil([this] { return istInProgress(); }));

    recovery->refreshState();
    pumpFor(settleTime);

    EXPECT_TRUE(istInProgress());
}

// An unresolved owner is indistinguishable from no run in progress, so the
// refresh has to clear rather than let an earlier signal stand.
TEST_F(IstRecoveryTest, RefreshClearsFlagWhenOwnerCannotBeResolved)
{
    hideServiceFromMapper();
    recovery = std::make_unique<DBusIstRecovery>(clientBus);
    pumpFor(settleTime);
    ASSERT_FALSE(istInProgress());

    setInProgress(true);
    ASSERT_TRUE(pumpUntil([this] { return istInProgress(); }));

    recovery->refreshState();

    EXPECT_TRUE(pumpUntil([this] { return !istInProgress(); }));
}

// A failed decode loses the new value, so the flag has to be rebuilt from a
// read instead of being left at whatever the last readable signal said. With
// no owner resolved there is no owner match either, so the refresh behind the
// failed decode is the only thing that can release the flag here.
TEST_F(IstRecoveryTest, RereadsPropertyWhenChangeCannotBeDecoded)
{
    failMapperLookup();
    recovery = std::make_unique<DBusIstRecovery>(clientBus);
    pumpFor(settleTime);
    ASSERT_FALSE(istInProgress());

    setInProgress(true);
    ASSERT_TRUE(pumpUntil([this] { return istInProgress(); }));

    emitUndecodablePropertiesChanged();

    EXPECT_TRUE(pumpUntil([this] { return !istInProgress(); }));
}

// A path the mapper has never seen comes back as an error rather than as an
// empty list, so this is the arm a boot ahead of the IST service takes.
TEST_F(IstRecoveryTest, RefreshClearsFlagWhenMapperLookupFails)
{
    failMapperLookup();
    recovery = std::make_unique<DBusIstRecovery>(clientBus);
    pumpFor(settleTime);
    ASSERT_FALSE(istInProgress());

    setInProgress(true);
    ASSERT_TRUE(pumpUntil([this] { return istInProgress(); }));

    recovery->refreshState();

    EXPECT_TRUE(pumpUntil([this] { return !istInProgress(); }));
}

// The lookup cannot be cancelled, so its reply lands on a destroyed object.
// Under asan these two fail loudly if the lifetime token stops being checked.
TEST_F(IstRecoveryTest, DropsLookupReplyThatArrivesAfterDestruction)
{
    publishIstService();
    setInProgress(true);
    recovery = std::make_unique<DBusIstRecovery>(clientBus);

    recovery.reset();

    pumpFor(settleTime);
}

TEST_F(IstRecoveryTest, DropsReadReplyThatArrivesAfterDestruction)
{
    publishIstService();
    setInProgress(true);
    recovery = std::make_unique<DBusIstRecovery>(clientBus);
    pumpFor(settleTime);

    recovery->refreshState();
    recovery.reset();

    pumpFor(settleTime);
}

} // namespace
