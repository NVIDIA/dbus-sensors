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

#include "NvidiaInfoSchema.hpp"

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nvidia::info
{

inline constexpr std::string_view defaultInfoPath =
    "/xyz/openbmc_project/inventory/system";

inline constexpr std::string_view persistedJsonDir = "/var/lib/nvidia-info";

inline constexpr std::string_view mapperBusName =
    "xyz.openbmc_project.ObjectMapper";
inline constexpr std::string_view mapperPath =
    "/xyz/openbmc_project/object_mapper";
inline constexpr std::string_view mapperInterface =
    "xyz.openbmc_project.ObjectMapper";
inline constexpr std::string_view systemInterface =
    "xyz.openbmc_project.Inventory.Item.System";
inline constexpr std::string_view boardInterface =
    "xyz.openbmc_project.Inventory.Item.Board";
inline constexpr std::string_view processorModuleInterface =
    "xyz.openbmc_project.Inventory.Item.ProcessorModule";

// D-Bus service constants for the CreateInfo method
inline constexpr std::string_view nvidiaInfoService =
    "xyz.openbmc_project.NvidiaInfo";
inline constexpr std::string_view nvidiaInfoObjPath =
    "/xyz/openbmc_project/NvidiaInfo";
inline constexpr std::string_view nvidiaInfoInterface =
    "xyz.openbmc_project.NvidiaInfo";

struct TerminusInfo
{
    std::string rawJson;
    TerminusData terminus;
};

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
    std::string motherboardPath;
    std::map<uint64_t, std::string> processorModulePaths;
    std::map<std::string, TerminusInfo> terminusInfos;
    std::unique_ptr<sdbusplus::bus::match_t> interfaceAddedMatch;

    // D-Bus interface that exposes CreateInfo method
    std::shared_ptr<sdbusplus::asio::dbus_interface> serviceIface;

    static std::string extractTerminusName(const std::string& filePath);
    static uint64_t parseModuleIndex(std::string_view terminusName);

    void createInfoFromFile(const std::string& filePath);
    void createInfoFromJsonString(int32_t processorModuleIndex,
                                  const std::string& jsonStr);

    void clearTerminusInfo(const std::string& terminusName);
    void updateTerminusInfo(const std::string& terminusName, TerminusData td,
                            std::string rawJson);

    void loadPersistedInfoFiles();

    void setupMotherboardMatch();
    void discoverMotherboardPath(std::function<void()> callback);
    void discoverProcessorModulePaths(std::function<void()> callback);
    void discoverPaths(std::function<void()> callback);
};

} // namespace nvidia::info
