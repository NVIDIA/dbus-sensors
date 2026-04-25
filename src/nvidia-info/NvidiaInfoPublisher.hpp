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

// Publisher is the shared base class for NvidiaCpu / NvidiaDimm / NvidiaPcie
// / NvidiaTpm. Each of those types registers a fixed set of D-Bus
// interfaces on an object_server in its publish() method, and must remove
// them again when destroyed.
//
// Instead of storing one named std::shared_ptr<dbus_interface> per
// interface and hand-maintaining a destructor init-list, derived classes
// add() each interface and let the base destructor unregister them all in
// one loop.
//
// Ownership rules (unchanged from the original four copies of this code):
//   * server is a raw pointer cached on the first add(). Its lifetime is
//     managed by NvidiaInfo::objServer (a shared_ptr), which outlives
//     terminusInfos and therefore outlives every Publisher contained in
//     it.
//   * Publishers are owned by std::vector inside TerminusData, so they
//     must be move-constructible and move-assignable. Copying is
//     forbidden because the D-Bus interfaces are single-owner resources.
//   * On move, the source's ifaces vector is left empty and its server
//     pointer retains its value; the source's destructor therefore
//     iterates an empty list and does nothing. The destination takes
//     over the registered interfaces and will remove them on its own
//     destruction.
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
    // Add a new D-Bus interface at path, remember it for later
    // unregistration, and return a reference the caller can use to
    // register_property() on it. The first call on a given Publisher
    // instance also latches the object_server pointer.
    sdbusplus::asio::dbus_interface& add(const std::string& path,
                                         std::string_view interfaceName,
                                         sdbusplus::asio::object_server& objs)
    {
        server = &objs;
        auto iface =
            objs.add_interface(path, std::string(interfaceName));
        ifaces.push_back(std::move(iface));
        return *ifaces.back();
    }

    // Call initialize() on every interface added so far. Derived classes
    // call this once at the end of publish(); the original code split
    // initialize() calls per-path but the observable effect on D-Bus is
    // the same.
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
