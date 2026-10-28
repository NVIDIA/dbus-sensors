// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA CORPORATION & AFFILIATES
#pragma once

// The parts of hmc_usb_monitor.cpp its unit tests call, and the daemon.

#include "Utils.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/icmp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>
#include <sdbusplus/asio/connection.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace hmc_usb_monitor
{

using Ping = std::function<boost::asio::awaitable<bool>()>;
using Sleep = std::function<boost::asio::awaitable<void>(std::chrono::seconds)>;

// The link at path (/xyz/openbmc_project/network/<name>) is online when
// phosphor-networkd shows it LinkUp with an IPv4 address under it.
bool isOnline(const ManagedObjectType& objects, const std::string& path);

// The HMC's RNDIS gadget (USB 0525:a4a2) as sysfs shows it: its network
// interface, its root hub (usbN) and the ehci-platform host controller
// above it.
struct HmcUsb
{
    std::string link;
    std::string rootHub;
    std::string controller;
};

// Nothing while the gadget is not there.
std::optional<HmcUsb> findHmcUsb(const std::filesystem::path& sysfs);

// Re-authorizes the HMC's RNDIS device under sysfs; if the HMC still does
// not answer, resets the root hub and rebinds its host controller.
boost::asio::awaitable<void> recoverUsbConnection(
    const std::filesystem::path& sysfs, const HmcUsb& usb, const Ping& ping,
    const Sleep& sleep);

class LinkState;

// The daemon: finds the gadget, then follows the link and recovers it. It
// outlives io.run(), so its callbacks capture this.
class Monitor
{
  public:
    Monitor(boost::asio::io_context& io,
            std::shared_ptr<sdbusplus::asio::connection> conn);
    ~Monitor();
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    void start();

  private:
    void retry(const boost::system::error_code& ec);
    void linkKnown();

    boost::asio::io_context& io;
    std::shared_ptr<sdbusplus::asio::connection> conn;
    // The pings and the sleeps; they never overlap.
    boost::asio::steady_timer timer;
    boost::asio::steady_timer findTimer;
    std::optional<boost::asio::ip::icmp::socket> socket;
    uint16_t sequence = 0;
    Ping ping;
    Sleep sleep;
    HmcUsb usb;
    std::unique_ptr<LinkState> link;
    bool waiting = false;
};

} // namespace hmc_usb_monitor
