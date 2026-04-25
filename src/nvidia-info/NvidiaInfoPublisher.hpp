/*
 * Copyright (c) 2026 NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <sdbusplus/asio/object_server.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nvidia
{
namespace info
{

// Shared base for NvidiaCpu / NvidiaDimm / NvidiaPcie / NvidiaTpm. Each
// derived type add()s its D-Bus interfaces in publish(); the base
// destructor unregisters whatever is still owned.
//
// Move-only because dbus_interface is a single-owner resource. The
// `server` raw pointer is safe: it points at NvidiaInfo::objServer, which
// outlives every Publisher. After a move, the moved-from object's
// ifaces vector is empty so its destructor is a no-op.
class Publisher
{
  public:
    Publisher() = default;
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;
    Publisher(Publisher&&) noexcept = default;
    Publisher& operator=(Publisher&&) noexcept = default;

    ~Publisher()
    {
        if (server == nullptr)
        {
            return;
        }
        for (auto& iface : ifaces)
        {
            if (iface)
            {
                server->remove_interface(iface);
            }
        }
    }

  protected:
    // Register an interface at path, remember it for unregistration, and
    // return a reference for register_property().
    sdbusplus::asio::dbus_interface& add(const std::string& path,
                                         std::string_view interfaceName,
                                         sdbusplus::asio::object_server& objs)
    {
        server = &objs;
        auto iface = objs.add_interface(path, std::string(interfaceName));
        ifaces.push_back(std::move(iface));
        return *ifaces.back();
    }

    // Handle to the most recently add()ed interface, or nullptr.
    std::shared_ptr<sdbusplus::asio::dbus_interface> lastIface() const
    {
        return ifaces.empty() ? nullptr : ifaces.back();
    }

    // initialize() every interface added so far.
    void initializeAll()
    {
        for (auto& iface : ifaces)
        {
            iface->initialize();
        }
    }

  private:
    sdbusplus::asio::object_server* server{nullptr};
    std::vector<std::shared_ptr<sdbusplus::asio::dbus_interface>> ifaces;
};

} // namespace info
} // namespace nvidia
