// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
#pragma once

#include "LpuPowerModeDevice.hpp"
#include "LpuPowerProfiles.hpp"

#include <sdbusplus/asio/object_server.hpp>

#include <memory>

namespace lpu::power
{
class Service
{
  public:
    explicit Service(sdbusplus::asio::object_server& server);
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    Service(Service&&) = delete;
    Service& operator=(Service&&) = delete;

    void updateProfileCatalog(const ManagedObjectType& objects);

  private:
    sdbusplus::asio::object_server& server;
    DeviceTransport transport;
    Controller controller{transport};
    std::optional<ProfileCatalog> catalog;
    std::shared_ptr<sdbusplus::asio::dbus_interface> modeInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> associationInterface;
};
} // namespace lpu::power
