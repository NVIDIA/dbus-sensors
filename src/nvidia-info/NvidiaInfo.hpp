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

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace nvidia::info
{

inline constexpr std::string_view defaultInfoPath =
    "/xyz/openbmc_project/inventory/system";

// D-Bus service constants for the CreateInfo method
inline constexpr std::string_view nvidiaInfoService =
    "xyz.openbmc_project.NvidiaInfo";
inline constexpr std::string_view nvidiaInfoObjPath =
    "/xyz/openbmc_project/NvidiaInfo";
inline constexpr std::string_view nvidiaInfoInterface =
    "xyz.openbmc_project.NvidiaInfo";

class NvidiaInfo
{
  public:
    NvidiaInfo(const NvidiaInfo&) = delete;
    NvidiaInfo& operator=(const NvidiaInfo&) = delete;
    NvidiaInfo(NvidiaInfo&&) = default;
    NvidiaInfo& operator=(NvidiaInfo&&) = default;
    ~NvidiaInfo();

    NvidiaInfo(const std::shared_ptr<boost::asio::io_context>& io,
               std::shared_ptr<sdbusplus::asio::connection> conn,
               std::shared_ptr<sdbusplus::asio::object_server> obj,
               std::string invPath);

  private:
    std::shared_ptr<boost::asio::io_context> ioCtx;
    std::shared_ptr<sdbusplus::asio::connection> bus;
    std::shared_ptr<sdbusplus::asio::object_server> objServer;

    std::string inventoryPath;

    // D-Bus interface that exposes CreateInfo method
    std::shared_ptr<sdbusplus::asio::dbus_interface> serviceIface;

    void createInfoFromFile(const std::string& filePath);
    void createInfoFromJsonString(int32_t processorModuleIndex,
                                  const std::string& jsonStr);
};

} // namespace nvidia::info
