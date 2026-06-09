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
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace nvidia::info
{

inline constexpr const char* defaultInfoPath =
    "/xyz/openbmc_project/inventory/system";

inline constexpr const char* persistedJsonDir = "/var/lib/nvidia-info";

inline constexpr const char* mapperBusName = "xyz.openbmc_project.ObjectMapper";
inline constexpr const char* mapperPath = "/xyz/openbmc_project/object_mapper";
inline constexpr const char* mapperInterface =
    "xyz.openbmc_project.ObjectMapper";
inline constexpr const char* systemInterface =
    "xyz.openbmc_project.Inventory.Item.System";
inline constexpr const char* boardInterface =
    "xyz.openbmc_project.Inventory.Item.Board";
inline constexpr const char* processorModuleInterface =
    "xyz.openbmc_project.Inventory.Item.ProcessorModule";

// D-Bus service constants for the CreateInfo method
inline constexpr const char* nvidiaInfoService =
    "xyz.openbmc_project.NvidiaInfo";
inline constexpr const char* nvidiaInfoObjPath =
    "/xyz/openbmc_project/NvidiaInfo";
inline constexpr const char* nvidiaInfoInterface =
    "xyz.openbmc_project.NvidiaInfo";

struct TerminusInfo
{
    TerminusData terminus;
    // Module index used to look up the processor-module path for associations.
    int32_t moduleIndex{0};
    // CPU socket; (moduleIndex, socket) is the terminus identity.
    int32_t socket{0};
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
               std::string invPath,
               std::string persistedDir = persistedJsonDir);

    // Sets Refresh=true on every Control.Trigger InventoryData object
    // under /xyz/openbmc_project/control.
    void triggerInventoryRefresh();

    // Public so tests can drive the publish path without going through the
    // CreateInfo D-Bus method dispatch (which would require running an
    // io_context concurrently). On the wire, this is exactly the body of
    // the CreateInfo method registered in the constructor; same throwing
    // contract.
    void createInfoFromJsonString(int32_t processorModuleIndex,
                                  const std::string& jsonStr);

  private:
    std::shared_ptr<boost::asio::io_context> ioCtx;
    std::shared_ptr<sdbusplus::asio::connection> bus;
    std::shared_ptr<sdbusplus::asio::object_server> objServer;

    std::string inventoryPath;
    std::string persistedDir;
    std::string motherboardPath;
    std::map<uint64_t, std::string> processorModulePaths;
    std::map<std::string, TerminusInfo> terminusInfos;
    std::unique_ptr<sdbusplus::bus::match_t> interfaceAddedMatch;

    // D-Bus interface that exposes CreateInfo method
    std::shared_ptr<sdbusplus::asio::dbus_interface> serviceIface;

    // Parse, validate, and publish rawJson for module; the socket is read
    // from the payload's Processor entry. Throws SdBusError on failure
    // (callers handle any persisted-file cleanup); if persistOnSuccess, a
    // persist failure rolls back the publish.
    void processAndPublish(int32_t moduleIndex, std::string rawJson,
                           bool persistOnSuccess, std::string_view context);

    // socket - moduleIndex * socketsPerModule: the per-module offset for
    // DIMM/PCIe/TPM numbering. nullopt if out of range for the platform
    // (rank outside [0, socketsPerModule)); validated in processAndPublish.
    static std::optional<int32_t> rankWithinModule(int32_t moduleIndex,
                                                   int32_t socket);

    // Throws if another published socket in the same module has a different
    // DIMM/PCIe/TPM count; rank-based numbering needs a uniform count or the
    // object paths would overlap.
    void assertUniformCounts(int32_t moduleIndex, int32_t socket,
                             const TerminusData& td) const;

    void clearTerminusInfo(const std::string& terminusName);
    // rank is the validated rankWithinModule() value, computed by the caller.
    void updateTerminusInfo(const std::string& terminusName,
                            int32_t moduleIndex, int32_t socket, int32_t rank,
                            TerminusData td);

    // Patch Association.Definitions on this terminus's DIMM and PCIe
    // slot objects using whatever discovered paths are known. Idempotent.
    void attachAssociationsFor(const std::string& terminusName);

    // Run attachAssociationsFor on every known terminus.
    void attachAllAssociations();

    void loadPersistedInfoFiles();

    // Payload of the ObjectManager InterfacesAdded signal.
    using InterfacesAddedMap = std::map<
        std::string,
        std::map<std::string,
                 std::variant<bool, uint8_t, int16_t, uint16_t, int32_t,
                              uint32_t, int64_t, uint64_t, double, std::string,
                              std::vector<uint8_t>>>>;

    void setupMotherboardMatch();
    void onInterfacesAdded(sdbusplus::message_t& msg);
    void scheduleMotherboardDiscovery();
    void discoverMotherboardPath(std::function<void()> callback);
    void discoverProcessorModulePaths(std::function<void()> callback);
    void discoverPaths(std::function<void()> callback);
};

} // namespace nvidia::info
