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

#include "NvidiaInfo.hpp"

#include "NvidiaInfoPersistence.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace nvidia::info
{

namespace
{

// Sockets per processor module (meson option nvidia-info-sockets-per-module);
// defaults to 1 so test/schema sources compile without the -D flag. Drives
// the rank-within-module derivation for DIMM/PCIe/TPM numbering.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#ifndef NVIDIA_INFO_SOCKETS_PER_MODULE
#define NVIDIA_INFO_SOCKETS_PER_MODULE 1
#endif
constexpr int32_t socketsPerModule = NVIDIA_INFO_SOCKETS_PER_MODULE;

// Upper bound on the CPU socket index, matching the schema's Socket range.
constexpr uint32_t maxSocket = 255;

} // namespace

NvidiaInfo::NvidiaInfo(const std::shared_ptr<boost::asio::io_context>& io,
                       std::shared_ptr<sdbusplus::asio::connection> conn,
                       std::shared_ptr<sdbusplus::asio::object_server> obj,
                       std::string invPath, std::string persistedDirArg) :
    ioCtx(io), bus(std::move(conn)), objServer(std::move(obj)),
    inventoryPath(std::move(invPath)), persistedDir(std::move(persistedDirArg))
{
    lg2::info("NVIDIA Info inventory path: {I}", "I", inventoryPath);

    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(persistedDir), ec);
        if (ec)
        {
            lg2::error("Failed to create info directory {D}: {E}", "D",
                       persistedDir, "E", ec.message());
            throw std::system_error(ec, "Failed to create info directory " +
                                            persistedDir);
        }
    }

    setupMotherboardMatch();

    try
    {
        loadPersistedInfoFiles();
    }
    catch (const std::exception& e)
    {
        // Per-file rejections are handled in loadPersistedInfoFiles; an
        // error here is unexpected. Log it and continue startup.
        lg2::error("Unexpected error during persisted-info recovery, "
                   "continuing startup without it: {E}",
                   "E", e.what());
    }

    serviceIface =
        objServer->add_interface(nvidiaInfoObjPath, nvidiaInfoInterface);

    serviceIface->register_method(
        "CreateInfo",
        [this](int32_t processorModuleIndex, const std::string& jsonPayload) {
            createInfoFromJsonString(processorModuleIndex, jsonPayload);
        });

    serviceIface->initialize();

    lg2::info("NVIDIA Info service ready, D-Bus method registered at "
              "{P}/{I}",
              "P", nvidiaInfoObjPath, "I", nvidiaInfoInterface);

    // Probe in case Entity Manager already published the motherboard /
    // ProcessorModule paths; otherwise the InterfacesAdded path covers it.
    discoverPaths([this]() { attachAllAssociations(); });
}

NvidiaInfo::~NvidiaInfo()
{
    if (serviceIface)
    {
        objServer->remove_interface(serviceIface);
    }
}

void NvidiaInfo::setupMotherboardMatch()
{
    interfaceAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
        *bus,
        sdbusplus::bus::match::rules::interfacesAdded() +
            sdbusplus::bus::match::rules::argNpath(
                0, "/xyz/openbmc_project/inventory/"),
        [this](sdbusplus::message_t& msg) { onInterfacesAdded(msg); });
}

void NvidiaInfo::onInterfacesAdded(sdbusplus::message_t& msg)
{
    try
    {
        sdbusplus::object_path objPath;
        InterfacesAddedMap interfaces;
        msg.read(objPath, interfaces);

        if (interfaces.find(systemInterface) == interfaces.end())
        {
            return;
        }

        lg2::info("System interface appeared at {P}, scheduling "
                  "motherboard discovery after delay",
                  "P", std::string(objPath));
        scheduleMotherboardDiscovery();
    }
    catch (const std::exception& e)
    {
        lg2::error("Exception in interfacesAdded match callback: {E}", "E",
                   e.what());
    }
}

void NvidiaInfo::scheduleMotherboardDiscovery()
{
    // Timer kept alive by its own async_wait callback.
    auto delayTimer = std::make_shared<boost::asio::steady_timer>(*ioCtx);
    delayTimer->expires_after(std::chrono::seconds(2));
    delayTimer->async_wait(
        [this, delayTimer](const boost::system::error_code& ec) {
            if (ec)
            {
                return;
            }
            discoverPaths([this]() { attachAllAssociations(); });
        });
}

void NvidiaInfo::attachAssociationsFor(const std::string& terminusName)
{
    auto it = terminusInfos.find(terminusName);
    if (it == terminusInfos.end())
    {
        return;
    }
    auto& entry = it->second;
    auto& terminus = entry.terminus;
    const auto moduleIdx = static_cast<uint64_t>(entry.moduleIndex);

    if (!motherboardPath.empty())
    {
        for (auto& dimm : terminus.dimms)
        {
            dimm.attach(motherboardPath);
        }
    }

    if (auto modIt = processorModulePaths.find(moduleIdx);
        modIt != processorModulePaths.end())
    {
        for (auto& slot : terminus.pcieSlots)
        {
            if (!slot.isPresent())
            {
                continue;
            }
            slot.attach(modIt->second);
        }
    }
}

void NvidiaInfo::attachAllAssociations()
{
    if (motherboardPath.empty() && processorModulePaths.empty())
    {
        return;
    }
    for (auto& [name, _] : terminusInfos)
    {
        attachAssociationsFor(name);
    }
}

void NvidiaInfo::triggerInventoryRefresh()
{
    static constexpr const char* triggerInterface =
        "xyz.openbmc_project.Control.Trigger";
    static constexpr std::string_view inventoryDataTag = "InventoryData";
    static constexpr const char* controlRoot = "/xyz/openbmc_project/control";

    using SubTreeType = std::vector<std::pair<
        std::string,
        std::vector<std::pair<std::string, std::vector<std::string>>>>>;

    lg2::info("Requesting startup inventory refresh from upstream producers");

    bus->async_method_call(
        [this](const boost::system::error_code& ec,
               const SubTreeType& subtree) {
            if (ec)
            {
                lg2::info(
                    "Inventory refresh: Control.Trigger subtree not ready: {E}",
                    "E", ec.message());
                return;
            }

            bool any = false;
            for (const auto& [path, services] : subtree)
            {
                if (path.find(inventoryDataTag) == std::string::npos ||
                    services.empty())
                {
                    continue;
                }
                const std::string& svc = services.front().first;
                any = true;

                lg2::info("Inventory refresh: Refresh=true -> {S} {P}", "S",
                          svc, "P", path);

                bus->async_method_call(
                    [path](const boost::system::error_code& setEc) {
                        if (setEc)
                        {
                            lg2::error("Inventory refresh: failed to set "
                                       "Refresh on {P}: {E}",
                                       "P", path, "E", setEc.message());
                            return;
                        }
                        lg2::info("Inventory refresh: triggered on {P}", "P",
                                  path);
                    },
                    svc, path, "org.freedesktop.DBus.Properties", "Set",
                    triggerInterface, "Refresh", std::variant<bool>(true));
            }

            if (!any)
            {
                lg2::info("Inventory refresh: no Control.Trigger "
                          "InventoryData paths found under {R}",
                          "R", controlRoot);
            }
        },
        mapperBusName, mapperPath, mapperInterface, "GetSubTree", controlRoot,
        0, std::vector<std::string>{triggerInterface});
}

void NvidiaInfo::discoverMotherboardPath(std::function<void()> callback)
{
    std::string searchPath = inventoryPath;
    bool requireExactMatch = false;

    if (inventoryPath != defaultInfoPath)
    {
        std::filesystem::path p(inventoryPath);
        searchPath = p.parent_path().string();
        requireExactMatch = true;
    }

    std::vector<std::string> desiredInterfaces{systemInterface};
    if (requireExactMatch)
    {
        desiredInterfaces.emplace_back(boardInterface);
    }

    bus->async_method_call(
        [this, requireExactMatch,
         cb = std::move(callback)](const boost::system::error_code& ec,
                                   const std::vector<std::string>& paths) {
            if (ec)
            {
                lg2::error("Failed to query system motherboard: {E}", "E",
                           ec.message());
            }
            else
            {
                for (const auto& p : paths)
                {
                    if (requireExactMatch && (p != inventoryPath))
                    {
                        continue;
                    }
                    motherboardPath = p;
                    break;
                }
                if (!motherboardPath.empty())
                {
                    lg2::info("Discovered motherboard path: {M}", "M",
                              motherboardPath);
                }
            }
            if (cb)
            {
                cb();
            }
        },
        mapperBusName, mapperPath, mapperInterface, "GetSubTreePaths",
        searchPath, 0, desiredInterfaces);
}

void NvidiaInfo::discoverProcessorModulePaths(std::function<void()> callback)
{
    std::vector<std::string> ifaces = {processorModuleInterface};

    bus->async_method_call(
        [this,
         cb = std::move(callback)](const boost::system::error_code& ec,
                                   const std::vector<std::string>& paths) {
            if (ec)
            {
                lg2::error("Failed to query processor module paths: {E}", "E",
                           ec.message());
                if (cb)
                {
                    cb();
                }
                return;
            }

            for (const auto& path : paths)
            {
                const auto pos = path.rfind('_');
                if (pos == std::string::npos)
                {
                    continue;
                }
                const char* first = path.data() + pos + 1;
                const char* last = path.data() + path.size();
                uint64_t idx = 0;
                auto [ptr, ec2] = std::from_chars(first, last, idx);
                if (ec2 != std::errc{} || ptr != last)
                {
                    lg2::warning("Could not parse module index from path {P}",
                                 "P", path);
                    continue;
                }
                processorModulePaths[idx] = path;
                lg2::info("Discovered processor module {I} at path: {P}", "I",
                          idx, "P", path);
            }

            if (cb)
            {
                cb();
            }
        },
        mapperBusName, mapperPath, mapperInterface, "GetSubTreePaths",
        "/xyz/openbmc_project/inventory/", 0, ifaces);
}

void NvidiaInfo::discoverPaths(std::function<void()> callback)
{
    auto afterMotherboard = [this, cb = std::move(callback)]() mutable {
        if (processorModulePaths.empty())
        {
            discoverProcessorModulePaths(std::move(cb));
        }
        else if (cb)
        {
            cb();
        }
    };

    if (motherboardPath.empty())
    {
        discoverMotherboardPath(std::move(afterMotherboard));
    }
    else
    {
        afterMotherboard();
    }
}

void NvidiaInfo::createInfoFromJsonString(int32_t processorModuleIndex,
                                          const std::string& jsonStr)
{
    // Must match the single-digit range that recovery's filename regex
    // accepts.
    if (processorModuleIndex < 0 || processorModuleIndex > 9)
    {
        throw sdbusplus::exception::SdBusError(
            -EINVAL, "processor module index must be in the range 0..9");
    }

    lg2::info("CreateInfo called for processor module {I} (JSON length {L})",
              "I", processorModuleIndex, "L", jsonStr.size());

    try
    {
        processAndPublish(processorModuleIndex, jsonStr,
                          /*persistOnSuccess=*/true, "CreateInfo");
    }
    catch (const InfoError& e)
    {
        // Log the rejection, then rethrow so the caller still gets the error.
        lg2::error("CreateInfo rejected for module {I}: {N}: {E}", "I",
                   processorModuleIndex, "N", e.name(), "E", e.what());
        throw;
    }
}

std::optional<int32_t> NvidiaInfo::rankWithinModule(int32_t moduleIndex,
                                                    int32_t socket)
{
    const int32_t rank = socket - moduleIndex * socketsPerModule;
    if (rank < 0 || rank >= socketsPerModule)
    {
        return std::nullopt;
    }
    return rank;
}

void NvidiaInfo::assertUniformCounts(int32_t moduleIndex, int32_t socket,
                                     const TerminusData& td) const
{
    for (const auto& [name, entry] : terminusInfos)
    {
        // Only compare against other sockets of the same module; a socket
        // replacing itself is fine.
        if (entry.moduleIndex != moduleIndex || entry.socket == socket)
        {
            continue;
        }
        const auto& other = entry.terminus;
        if (other.dimms.size() != td.dimms.size() ||
            other.pcieSlots.size() != td.pcieSlots.size() ||
            other.tpms.size() != td.tpms.size())
        {
            throw InvalidConfiguration(std::format(
                "component counts (DIMM/PCIe/TPM = {}/{}/{}) differ from "
                "socket {} of the same module ({}/{}/{}); rank-based "
                "numbering requires a uniform count across a module's sockets",
                td.dimms.size(), td.pcieSlots.size(), td.tpms.size(),
                entry.socket, other.dimms.size(), other.pcieSlots.size(),
                other.tpms.size()));
        }
    }
}

void NvidiaInfo::processAndPublish(int32_t moduleIndex, std::string rawJson,
                                   bool persistOnSuccess,
                                   std::string_view context)
{
    // Validation throws SchemaViolation/InvalidConfiguration (D-Bus
    // exceptions) directly; no translating catch here.
    TerminusData td = parseAndValidate(rawJson);

    // Identity is the single Processor's socket; the model is one socket per
    // payload, so reject anything else rather than keying off the first.
    if (td.cpus.size() != 1)
    {
        throw SchemaViolation(std::format(
            "expected exactly one Processor entry, got {}", td.cpus.size()));
    }
    // Re-check the schema's 0..255 Socket range before narrowing so an
    // out-of-range value can't wrap to a negative socket.
    const uint32_t rawSocket = td.cpus.front().socketNum;
    if (rawSocket > maxSocket)
    {
        throw SchemaViolation(std::format("socket {} exceeds the maximum of {}",
                                          rawSocket, maxSocket));
    }
    const int32_t socket = static_cast<int32_t>(rawSocket);

    const auto maybeRank = rankWithinModule(moduleIndex, socket);
    if (!maybeRank)
    {
        throw InvalidConfiguration(std::format(
            "socket {} is inconsistent with module {} for a platform with "
            "{} socket(s) per module",
            socket, moduleIndex, socketsPerModule));
    }
    const int32_t rank = *maybeRank;
    assertUniformCounts(moduleIndex, socket, td);

    const std::string terminusName =
        std::format("ProcessorModule_{}_Socket_{}", moduleIndex, socket);

    try
    {
        updateTerminusInfo(terminusName, moduleIndex, socket, rank,
                           std::move(td));
    }
    catch (const std::exception& e)
    {
        // Roll back the partial publish and rethrow.
        lg2::error("{C}: exception publishing terminus {T}: {E}", "C", context,
                   "T", terminusName, "E", e.what());
        clearTerminusInfo(terminusName);
        throw;
    }

    const persistence::PersistedId id{moduleIndex, socket};
    if (persistOnSuccess &&
        !persistence::persistInfoJson(persistedDir, id, rawJson))
    {
        // Roll back the publish so [D-Bus state] still matches
        // [on-disk state] (the previous good file, or none). Caller can
        // retry; this avoids thrashing the entire process for a
        // potentially transient filesystem error.
        lg2::error("{C}: failed to persist info JSON for terminus {T}; "
                   "rolling back publish to preserve round-trip guarantee",
                   "C", context, "T", terminusName);
        clearTerminusInfo(terminusName);
        throw sdbusplus::exception::SdBusError(
            -EIO, std::format("{} failed to persist", context).c_str());
    }
}

void NvidiaInfo::clearTerminusInfo(const std::string& terminusName)
{
    terminusInfos.erase(terminusName);
}

void NvidiaInfo::updateTerminusInfo(const std::string& terminusName,
                                    int32_t moduleIndex, int32_t socket,
                                    int32_t rankArg, TerminusData td)
{
    lg2::info("Updating NVIDIA inventory for terminus {T}", "T", terminusName);

    // Drop any old publishers (and their D-Bus interfaces) before
    // re-registering.
    clearTerminusInfo(terminusName);

    auto& entry = terminusInfos[terminusName];
    entry.terminus = std::move(td);
    entry.moduleIndex = moduleIndex;
    entry.socket = socket;
    auto& stored = entry.terminus;

    const auto moduleIdx = static_cast<uint64_t>(moduleIndex);

    // rank is the per-module offset multiplier; counts are uniform across the
    // module's sockets (assertUniformCounts), so each socket's block tiles
    // after the previous rank's.
    const auto rank = static_cast<uint64_t>(rankArg);
    const auto dimmStride = static_cast<uint64_t>(stored.dimms.size());
    const auto pcieStride = static_cast<uint64_t>(stored.pcieSlots.size());
    const auto tpmStride = static_cast<uint64_t>(stored.tpms.size());

    for (std::size_t i = 0; i < stored.cpus.size(); ++i)
    {
        // CPU path is flat (no module segment), so the index must be
        // globally unique: use the socket number.
        const uint64_t cpuIndex =
            static_cast<uint64_t>(stored.cpus[i].socketNum);

        const std::string cpuPath =
            std::format("{}/cpu/CPU_{}", inventoryPath, cpuIndex);
        const std::string componentPath =
            std::format("{}/component/HGX_CPU_{}", inventoryPath, cpuIndex);
        const std::string boardPath = std::format(
            "{}/board/HGX_ProcessorModule_{}", inventoryPath, moduleIdx);

        stored.cpus[i].publish(*objServer, cpuPath, componentPath, boardPath,
                               cpuIndex);
    }

    for (std::size_t i = 0; i < stored.dimms.size(); ++i)
    {
        const uint64_t memIndex = rank * dimmStride + static_cast<uint64_t>(i);
        const std::string dimmPath =
            std::format("{}/dimm/ProcessorModule_{}_Memory_{}", inventoryPath,
                        moduleIdx, memIndex);
        stored.dimms[i].publish(*objServer, dimmPath);
    }

    for (std::size_t i = 0; i < stored.pcieSlots.size(); ++i)
    {
        if (!stored.pcieSlots[i].isPresent())
        {
            continue;
        }
        const uint64_t slotIndex = rank * pcieStride + static_cast<uint64_t>(i);
        const std::string pciePath =
            std::format("{}/board/HGX_ProcessorModule_{}/pcieslot{}",
                        inventoryPath, moduleIdx, slotIndex);
        stored.pcieSlots[i].publish(*objServer, pciePath, moduleIdx);
    }

    for (std::size_t i = 0; i < stored.tpms.size(); ++i)
    {
        const uint64_t tpmIndex = rank * tpmStride + static_cast<uint64_t>(i);
        std::string tpmPath =
            std::format("{}/board/HGX_ProcessorModule_{}/tpm{}", inventoryPath,
                        moduleIdx, tpmIndex);
        stored.tpms[i].publish(*objServer, tpmPath);
    }

    // Best-effort attach with whatever paths are already known; later
    // discovery patches the rest.
    attachAssociationsFor(terminusName);

    lg2::info("NVIDIA inventory update complete for terminus {T}", "T",
              terminusName);
}

void NvidiaInfo::loadPersistedInfoFiles()
{
    // Only files matching persistedPathFor()'s shape are accepted; others
    // are ignored.
    namespace fs = std::filesystem;

    std::error_code ec;
    const fs::path infoDir{persistedDir};
    if (!fs::exists(infoDir, ec))
    {
        if (ec)
        {
            lg2::error("Info directory {D} could not be checked: {E}", "D",
                       persistedDir, "E", ec.message());
        }
        return;
    }

    fs::directory_iterator dirIt(infoDir, ec);
    if (ec)
    {
        lg2::error("Failed to open info directory {D}: {E}", "D", persistedDir,
                   "E", ec.message());
        return;
    }

    for (const auto& entry : dirIt)
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        const std::string filename = entry.path().filename().string();
        const auto persistedId = persistence::persistedIdFromFilename(filename);
        if (!persistedId)
        {
            continue;
        }

        const fs::path& path = entry.path();
        std::string rawJson;
        try
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                lg2::warning(
                    "Recovery: cannot open persisted file {F}, skipping", "F",
                    path.string());
                continue;
            }
            rawJson.assign(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>());
        }
        catch (const std::exception& e)
        {
            lg2::warning("Recovery: read error for {F}: {E}, skipping", "F",
                         path.string(), "E", e.what());
            continue;
        }

        lg2::info("Recovery: loading persisted info for module {I} socket {S} "
                  "from {F}",
                  "I", persistedId->processorModuleIndex, "S",
                  persistedId->socket, "F", path.string());
        try
        {
            // Already on disk, so no re-persist; drop the file ourselves if
            // it no longer validates.
            processAndPublish(persistedId->processorModuleIndex,
                              std::move(rawJson),
                              /*persistOnSuccess=*/false, "Recovery");
        }
        catch (const InfoError& e)
        {
            // Only an expected payload rejection drops the file; anything
            // unexpected (e.g. bad_alloc) propagates.
            lg2::warning("Recovery: persisted file {F} rejected: {E}, "
                         "removing",
                         "F", path.string(), "E", e.what());
            persistence::removePersistedFile(persistedDir, *persistedId);
        }
    }
}

} // namespace nvidia::info
