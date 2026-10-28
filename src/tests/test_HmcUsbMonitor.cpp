// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA CORPORATION & AFFILIATES

#include "Utils.hpp"
#include "hmc_usb_monitor.hpp"

#include <unistd.h>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp> // NOLINT(misc-include-cleaner)
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace
{

namespace fs = std::filesystem;
using std::chrono::seconds;

std::string readFile(const fs::path& path)
{
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

void writeFile(const fs::path& path, const std::string& value)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << value;
}

TEST(HmcUsbMonitor, OnlineIsLinkUpWithAnIPv4Address)
{
    const std::string link = "/xyz/openbmc_project/network/hmcusb0";
    const std::string eth = "xyz.openbmc_project.Network.EthernetInterface";
    const std::string ip = "xyz.openbmc_project.Network.IP";
    const std::string v4 = "xyz.openbmc_project.Network.IP.Protocol.IPv4";
    const std::string v6 = "xyz.openbmc_project.Network.IP.Protocol.IPv6";
    ManagedObjectType objects;
    EXPECT_FALSE(hmc_usb_monitor::isOnline(objects, link));

    objects[sdbusplus::object_path(link)][eth]["LinkUp"] = true;
    EXPECT_FALSE(hmc_usb_monitor::isOnline(objects, link));

    objects[sdbusplus::object_path(link + "/_3fe80_3a_3a1_2f64")][ip]["Type"] =
        v6;
    objects[sdbusplus::object_path(link + "0/_3172_2e31_2e13_2e1_2f24")][ip]
           ["Type"] = v4;
    EXPECT_FALSE(hmc_usb_monitor::isOnline(objects, link));

    objects[sdbusplus::object_path(link + "/_3172_2e31_2e13_2e241_2f24")][ip]
           ["Type"] = v4;
    EXPECT_TRUE(hmc_usb_monitor::isOnline(objects, link));

    objects[sdbusplus::object_path(link)][eth]["LinkUp"] = false;
    EXPECT_FALSE(hmc_usb_monitor::isOnline(objects, link));
}

// The AST2600 layout: hmcusb0 is the RNDIS gadget on usb1 under
// 1e6a3000.usb; eth0 is not a USB device.
TEST(HmcUsbMonitor, FindsTheGadgetItsHubAndController)
{
    const fs::path root =
        fs::temp_directory_path() /
        ("hmc-usb-monitor-find-" + std::to_string(::getpid()));
    fs::remove_all(root);
    EXPECT_FALSE(hmc_usb_monitor::findHmcUsb(root).has_value());

    const fs::path ctl = root / "devices/platform/ahb/1e6a3000.usb";
    const fs::path gadget = ctl / "usb1/1-1";
    writeFile(gadget / "idVendor", "0525");
    writeFile(gadget / "idProduct", "a4a2");
    fs::create_directories(gadget / "1-1:1.0");
    fs::create_directories(root / "bus/platform/drivers/ehci-platform");
    fs::create_directory_symlink(root / "bus/platform/drivers/ehci-platform",
                                 ctl / "driver");
    fs::create_directories(root / "class/net/eth0");
    fs::create_directories(root / "class/net/hmcusb0");
    fs::create_directory_symlink(gadget / "1-1:1.0",
                                 root / "class/net/hmcusb0/device");

    const auto usb =
        hmc_usb_monitor::findHmcUsb(root).value_or(hmc_usb_monitor::HmcUsb{});
    EXPECT_EQ(usb.link, "hmcusb0");
    EXPECT_EQ(usb.rootHub, "usb1");
    EXPECT_EQ(usb.controller, "1e6a3000.usb");
    fs::remove_all(root);
}

// recoverUsbConnection on a fake sysfs, with the HMC answering from the
// given ping on; each sleep records the sysfs state it saw.
class Recovery : public testing::Test
{
  protected:
    void SetUp() override
    {
        root = fs::temp_directory_path() /
               ("hmc-usb-monitor-" + std::to_string(::getpid()) + "-" +
                testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(root);
        writeFile(devices() / "1-1/idVendor", "0525");
        writeFile(devices() / "1-1/idProduct", "a4a2");
        writeFile(devices() / "1-1/authorized", "1");
        writeFile(devices() / "1-2/idVendor", "1d6b");
        writeFile(devices() / "1-2/idProduct", "0002");
        writeFile(devices() / "1-2/authorized", "1");
        writeFile(devices() / "usb1/authorized", "1");
        writeFile(driver() / "unbind", "");
        writeFile(driver() / "bind", "");
    }

    void TearDown() override
    {
        fs::remove_all(root);
    }

    fs::path devices() const
    {
        return root / "bus/usb/devices";
    }

    fs::path driver() const
    {
        return root / "bus/platform/drivers/ehci-platform";
    }

    boost::asio::awaitable<bool> ping()
    {
        co_return ++pings >= answerFrom;
    }

    boost::asio::awaitable<void> sleep(seconds d)
    {
        steps.push_back(std::to_string(d.count()) +
                        "s rndis=" + readFile(devices() / "1-1/authorized") +
                        " usb1=" + readFile(devices() / "usb1/authorized") +
                        " unbind=" + readFile(driver() / "unbind") +
                        " bind=" + readFile(driver() / "bind"));
        co_return;
    }

    std::string recover()
    {
        const hmc_usb_monitor::Ping p = std::bind_front(&Recovery::ping, this);
        const hmc_usb_monitor::Sleep s =
            std::bind_front(&Recovery::sleep, this);
        testing::internal::CaptureStderr();
        boost::asio::io_context io;
        // NOLINTNEXTLINE(misc-include-cleaner)
        boost::asio::co_spawn(
            io, hmc_usb_monitor::recoverUsbConnection(root, usb, p, s),
            boost::asio::detached);
        io.run();
        return testing::internal::GetCapturedStderr();
    }

    fs::path root;
    // Lives as long as the recovery: it holds a reference.
    const hmc_usb_monitor::HmcUsb usb{"hmcusb0", "usb1", "1e6a3000.usb"};
    int answerFrom = 1;
    int pings = 0;
    std::vector<std::string> steps;
};

TEST_F(Recovery, ReauthorizeIsEnough)
{
    EXPECT_EQ(recover(), "Resetting USB device: 1-1\n"
                         "BMC/HMC USB Connection restored\n");
    EXPECT_EQ(steps,
              std::vector<std::string>{"2s rndis=0 usb1=1 unbind= bind="});
    EXPECT_EQ(readFile(devices() / "1-1/authorized"), "1");
    EXPECT_EQ(readFile(devices() / "1-2/authorized"), "1");
    EXPECT_EQ(pings, 2);
}

TEST_F(Recovery, ControllerResetBringsTheHmcBack)
{
    answerFrom = 19;
    EXPECT_EQ(recover(), "Resetting USB device: 1-1\n"
                         "HMC not responding to ping\n"
                         "Resetting USB1\n"
                         "BMC/HMC USB Connection restored\n");
    std::vector<std::string> want{"2s rndis=0 usb1=1 unbind= bind="};
    want.insert(want.end(), 17, "5s rndis=1 usb1=1 unbind= bind=");
    want.emplace_back("2s rndis=1 usb1=0 unbind= bind=");
    want.emplace_back("2s rndis=1 usb1=0 unbind=1e6a3000.usb bind=");
    want.emplace_back(
        "5s rndis=1 usb1=0 unbind=1e6a3000.usb bind=1e6a3000.usb");
    EXPECT_EQ(steps, want);
    EXPECT_EQ(pings, 19);
}

TEST_F(Recovery, HmcNeverAnswers)
{
    answerFrom = 1000;
    EXPECT_EQ(recover(), "Resetting USB device: 1-1\n"
                         "HMC not responding to ping\n"
                         "Resetting USB1\n"
                         "HMC not responding to ping\n"
                         "Failed to restore BMC/HMC USB Connection\n");
    EXPECT_EQ(pings, 36);
    EXPECT_EQ(steps.size(), 1U + 17 + 3 + 17);
}

} // namespace
