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

#include <fcntl.h>
#include <unistd.h>

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
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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

inline constexpr std::string_view persistedFilenamePrefix = "ProcessorModule_";
inline constexpr std::string_view persistedFilenameSuffix = "_Info.json";

std::filesystem::path persistedPathFor(int32_t processorModuleIndex)
{
    return std::filesystem::path(persistedJsonDir) /
           std::format("{}{}{}", persistedFilenamePrefix, processorModuleIndex,
                       persistedFilenameSuffix);
}

// Inverse of persistedPathFor(); only single-digit indices accepted, which
// avoids leading-zero aliasing and caps modules at 0..9.
std::optional<int32_t> moduleIndexFromPersistedFilename(
    std::string_view filename)
{
    if (!filename.starts_with(persistedFilenamePrefix) ||
        !filename.ends_with(persistedFilenameSuffix))
    {
        return std::nullopt;
    }
    const std::string_view digits =
        filename.substr(persistedFilenamePrefix.size(),
                        filename.size() - persistedFilenamePrefix.size() -
                            persistedFilenameSuffix.size());
    if (digits.size() != 1 || digits[0] < '0' || digits[0] > '9')
    {
        return std::nullopt;
    }
    return digits[0] - '0';
}

bool persistInfoJson(int32_t processorModuleIndex, const std::string& jsonStr)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::create_directories(fs::path(persistedJsonDir), ec);
    if (ec)
    {
        lg2::error("Failed to create info directory {D}: {E}", "D",
                   persistedJsonDir, "E", ec.message());
        return false;
    }

    const fs::path finalPath = persistedPathFor(processorModuleIndex);
    const fs::path tempPath = std::format("{}.tmp", finalPath.string());

    try
    {
        std::ofstream out(tempPath.string(),
                          std::ios::binary | std::ios::out | std::ios::trunc);
        if (!out.good())
        {
            lg2::error("Failed to open temp info file {F}", "F",
                       tempPath.string());
            return false;
        }
        out << jsonStr;
        out.flush();
        if (!out.good())
        {
            lg2::error("Failed to write temp info file {F}", "F",
                       tempPath.string());
            fs::remove(tempPath, ec);
            return false;
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Exception while writing temp info file {F}: {E}", "F",
                   tempPath.string(), "E", e.what());
        ec.clear();
        fs::remove(tempPath, ec);
        return false;
    }

    // ofstream::flush() only reaches the kernel buffer; reopen read-only and
    // fsync so the data is on disk before the rename. Best-effort: failures
    // are logged but do not abort the rename.
    if (int fd = ::open(tempPath.c_str(), O_RDONLY | O_CLOEXEC); fd < 0)
    {
        lg2::warning("Failed to open temp info file {F} for fsync: {E}", "F",
                     tempPath.string(), "E", std::strerror(errno));
    }
    else
    {
        if (::fsync(fd) < 0)
        {
            lg2::warning("Failed to fsync temp info file {F}: {E}", "F",
                         tempPath.string(), "E", std::strerror(errno));
        }
        ::close(fd);
    }

    fs::rename(tempPath, finalPath, ec);
    if (ec)
    {
        lg2::error("Failed to rename {T} to {F}: {E}", "T", tempPath.string(),
                   "F", finalPath.string(), "E", ec.message());
        fs::remove(tempPath, ec);
        return false;
    }

    lg2::info("Persisted info JSON to {F}", "F", finalPath.string());
    return true;
}

void removePersistedFile(int32_t processorModuleIndex)
{
    namespace fs = std::filesystem;
    const fs::path path = persistedPathFor(processorModuleIndex);
    std::error_code ec;
    fs::remove(path, ec);
    if (ec)
    {
        lg2::warning("Failed to remove stale persisted file {F}: {E}", "F",
                     path.string(), "E", ec.message());
    }
    else
    {
        lg2::info("Removed stale persisted file: {F}", "F", path.string());
    }
}

} // namespace

NvidiaInfo::NvidiaInfo(const std::shared_ptr<boost::asio::io_context>& io,
                       std::shared_ptr<sdbusplus::asio::connection> conn,
                       std::shared_ptr<sdbusplus::asio::object_server> obj,
                       std::string invPath) :
    ioCtx(io), bus(std::move(conn)), objServer(std::move(obj)),
    inventoryPath(std::move(invPath))
{
    lg2::info("NVIDIA Info inventory path: {I}", "I", inventoryPath);

    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(persistedJsonDir), ec);
        if (ec)
        {
            lg2::error("Failed to create info directory {D}: {E}", "D",
                       persistedJsonDir, "E", ec.message());
        }
    }

    setupMotherboardMatch();

    try
    {
        loadPersistedInfoFiles();
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to recover persisted info files on startup: {E}",
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
        sdbusplus::message::object_path objPath;
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

    processAndPublish(std::format("ProcessorModule_{}", processorModuleIndex),
                      jsonStr, processorModuleIndex,
                      /*persistOnSuccess=*/true, "CreateInfo");
}

void NvidiaInfo::processAndPublish(
    std::string terminusName, std::string rawJson, int32_t moduleIndex,
    bool persistOnSuccess, std::string_view context)
{
    TerminusData td;
    try
    {
        Json doc = Json::parse(rawJson);
        // Schema enforces structure; validate() does derivation only.
        validateAgainstSchema(doc);
        td = doc.get<TerminusData>();
        validate(td);
    }
    catch (const std::exception& e)
    {
        lg2::critical("{C} rejected for terminus {T}: {E}", "C", context, "T",
                      terminusName, "E", e.what());
        clearTerminusInfo(terminusName);
        removePersistedFile(moduleIndex);
        throw sdbusplus::exception::SdBusError(
            -EINVAL, std::format("{} rejected: {}", context, e.what()).c_str());
    }

    try
    {
        updateTerminusInfo(terminusName, moduleIndex, std::move(td));
    }
    catch (const std::exception& e)
    {
        lg2::error("{C}: exception publishing terminus {T}: {E}", "C", context,
                   "T", terminusName, "E", e.what());
        // Tear down any partial publishers from the failed update so we
        // don't leave half-registered interfaces on D-Bus. The persisted
        // file is intentionally left intact: it still reflects the last
        // known-good state, and recovery on the next boot can retry.
        clearTerminusInfo(terminusName);
        throw sdbusplus::exception::SdBusError(
            -EIO,
            std::format("{} failed to publish: {}", context, e.what()).c_str());
    }

    if (persistOnSuccess && !persistInfoJson(moduleIndex, rawJson))
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
                                    int32_t moduleIndex, TerminusData td)
{
    lg2::info("Updating NVIDIA inventory for terminus {T}", "T", terminusName);

    // Drop any old publishers (and their D-Bus interfaces) before
    // re-registering.
    clearTerminusInfo(terminusName);

    auto& entry = terminusInfos[terminusName];
    entry.terminus = std::move(td);
    entry.moduleIndex = moduleIndex;
    auto& stored = entry.terminus;

    const auto moduleIdx = static_cast<uint64_t>(moduleIndex);
    const auto cpuCount = static_cast<uint64_t>(stored.cpus.size());

    for (std::size_t i = 0; i < stored.cpus.size(); ++i)
    {
        const uint64_t cpuIndex =
            moduleIdx * cpuCount + static_cast<uint64_t>(i);

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
        const std::string dimmPath =
            std::format("{}/dimm/ProcessorModule_{}_Memory_{}", inventoryPath,
                        moduleIdx, i);
        stored.dimms[i].publish(*objServer, dimmPath);
    }

    for (std::size_t i = 0; i < stored.pcieSlots.size(); ++i)
    {
        if (!stored.pcieSlots[i].isPresent())
        {
            continue;
        }
        const std::string pciePath =
            std::format("{}/board/HGX_ProcessorModule_{}/pcieslot{}",
                        inventoryPath, moduleIdx, i);
        stored.pcieSlots[i].publish(*objServer, pciePath, moduleIdx);
    }

    for (std::size_t i = 0; i < stored.tpms.size(); ++i)
    {
        std::string tpmPath = std::format("{}/chassis/motherboard/{}_tpm{}",
                                          inventoryPath, terminusName, i);
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
    const fs::path infoDir{persistedJsonDir};
    if (!fs::exists(infoDir, ec))
    {
        if (ec)
        {
            lg2::error("Info directory {D} could not be checked: {E}", "D",
                       persistedJsonDir, "E", ec.message());
        }
        return;
    }

    fs::directory_iterator dirIt(infoDir, ec);
    if (ec)
    {
        lg2::error("Failed to open info directory {D}: {E}", "D",
                   persistedJsonDir, "E", ec.message());
        return;
    }

    for (const auto& entry : dirIt)
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        const std::string filename = entry.path().filename().string();
        const auto moduleIndex = moduleIndexFromPersistedFilename(filename);
        if (!moduleIndex)
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

        lg2::info("Recovery: loading persisted info for module {I} from {F}",
                  "I", *moduleIndex, "F", path.string());
        try
        {
            // Already on disk; processAndPublish still removes it on failure.
            processAndPublish(std::format("ProcessorModule_{}", *moduleIndex),
                              std::move(rawJson), *moduleIndex,
                              /*persistOnSuccess=*/false, "Recovery");
        }
        catch (const std::exception& e)
        {
            lg2::warning("Recovery: persisted file {F} rejected: {E}", "F",
                         path.string(), "E", e.what());
        }
    }
}

} // namespace nvidia::info
