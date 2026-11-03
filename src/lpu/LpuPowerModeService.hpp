// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
#pragma once

#include "LpuPowerModeDevice.hpp"
#include "LpuPowerModeStore.hpp"
#include "LpuPowerProfiles.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>

#include <memory>
#include <string>
#include <vector>

namespace lpu::power
{
class Service
{
  public:
    Service(boost::asio::io_context& io,
            const std::shared_ptr<sdbusplus::asio::connection>& connection,
            sdbusplus::asio::object_server& server);
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    Service(Service&&) = delete;
    Service& operator=(Service&&) = delete;

    void updateProfileCatalog(const ManagedObjectType& objects);

  private:
    void setPowerMode(boost::asio::yield_context yield,
                      const std::string& mode);
    void restoreAll();
    void restoreIfOnline(size_t slot, const boost::system::error_code& ec,
                         const std::string& state);
    void restoreSlot(size_t slot, size_t attemptsLeft);

    boost::asio::io_context& io;
    std::shared_ptr<sdbusplus::asio::connection> connection;
    sdbusplus::asio::object_server& server;
    DeviceTransport transport;
    Controller controller{transport};
    ModeStore store;
    std::optional<ProfileCatalog> catalog;
    bool settingMode = false;
    std::vector<boost::asio::steady_timer> retryTimers;
    std::shared_ptr<sdbusplus::asio::dbus_interface> modeInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> associationInterface;
    std::unique_ptr<sdbusplus::bus::match_t> lpuStateMatch;
    std::unique_ptr<sdbusplus::bus::match_t> pwrSeqOwnerMatch;
};
} // namespace lpu::power
