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
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
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

inline constexpr std::string_view persistedFilenamePrefix = "ProcessorModule_";
inline constexpr std::string_view persistedFilenameSuffix = "_Info.json";

std::filesystem::path persistedPathFor(int32_t processorModuleIndex)
{
    return std::filesystem::path(std::string(persistedJsonDir)) /
           std::format("{}{}{}", persistedFilenamePrefix,
                       processorModuleIndex, persistedFilenameSuffix);
}

// Strict inverse of persistedPathFor(): returns the module index encoded in
// a filename of exactly the form "ProcessorModule_<digit>_Info.json", where
// <digit> is a single character in [0-9]. Any other name returns nullopt
// and must be ignored by callers. Restricting to one digit makes the
// canonical-form check trivial (no leading-zero aliasing is possible) and
// caps the supported module indices at 0..9.
std::optional<int32_t> moduleIndexFromPersistedFilename(
    std::string_view filename)
{
    if (!filename.starts_with(persistedFilenamePrefix) ||
        !filename.ends_with(persistedFilenameSuffix))
    {
        return std::nullopt;
    }
    const std::string_view digits = filename.substr(
        persistedFilenamePrefix.size(),
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
    fs::create_directories(fs::path(std::string(persistedJsonDir)), ec);
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
        fs::create_directories(fs::path(std::string(persistedJsonDir)), ec);
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

    const std::string serviceObjPath(nvidiaInfoObjPath);
    const std::string serviceInterfaceName(nvidiaInfoInterface);
    serviceIface =
        objServer->add_interface(serviceObjPath, serviceInterfaceName.c_str());

    serviceIface->register_method(
        "CreateInfoFromFile",
        [this](const std::string& filePath) { createInfoFromFile(filePath); });

    serviceIface->register_method(
        "CreateInfo",
        [this](int32_t processorModuleIndex, const std::string& jsonPayload) {
            createInfoFromJsonString(processorModuleIndex, jsonPayload);
        });

    serviceIface->initialize();

    lg2::info("NVIDIA Info service ready, D-Bus method registered at "
              "{P}/{I}",
              "P", nvidiaInfoObjPath, "I", nvidiaInfoInterface);
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
        [this](sdbusplus::message_t& msg) {
            try
            {
                sdbusplus::message::object_path objPath;
                std::map<std::string,
                         std::map<std::string,
                                  std::variant<bool, uint8_t, int16_t, uint16_t,
                                               int32_t, uint32_t, int64_t,
                                               uint64_t, double, std::string,
                                               std::vector<uint8_t>>>>
                    interfaces;
                msg.read(objPath, interfaces);

                for (const auto& [intf, properties] : interfaces)
                {
                    if (intf == systemInterface)
                    {
                        lg2::info(
                            "System interface appeared at {P}, scheduling "
                            "motherboard discovery after delay",
                            "P", std::string(objPath));
                        auto delayTimer =
                            std::make_shared<boost::asio::steady_timer>(*ioCtx);
                        delayTimer->expires_after(std::chrono::seconds(2));
                        delayTimer->async_wait(
                            [this,
                             delayTimer](const boost::system::error_code& ec) {
                                if (ec)
                                {
                                    return;
                                }
                                discoverPaths([this]() {
                                    try
                                    {
                                        if (terminusInfos.empty())
                                        {
                                            return;
                                        }
                                        if (motherboardPath.empty() &&
                                            processorModulePaths.empty())
                                        {
                                            return;
                                        }
                                        for (auto& [name, inv] : terminusInfos)
                                        {
                                            if (inv.rawJson.empty())
                                            {
                                                continue;
                                            }
                                            try
                                            {
                                                TerminusData td =
                                                    Json::parse(inv.rawJson)
                                                        .get<TerminusData>();
                                                validate(td);
                                                updateTerminusInfo(
                                                    name, inv.moduleIndex,
                                                    std::move(td),
                                                    std::string(inv.rawJson));
                                            }
                                            catch (const std::exception& e)
                                            {
                                                lg2::error(
                                                    "Deferred replay failed "
                                                    "for terminus {T}: {E}",
                                                    "T", name, "E", e.what());
                                            }
                                        }
                                    }
                                    catch (const std::exception& e)
                                    {
                                        lg2::error("Exception in deferred "
                                                   "motherboard discovery: {E}",
                                                   "E", e.what());
                                    }
                                });
                            });
                        break;
                    }
                }
            }
            catch (const std::exception& e)
            {
                lg2::error("Exception in interfacesAdded match callback: {E}",
                           "E", e.what());
            }
        });
}

void NvidiaInfo::triggerInventoryRefresh()
{
    static constexpr std::string_view triggerInterface =
        "xyz.openbmc_project.Control.Trigger";
    static constexpr std::string_view inventoryDataTag = "InventoryData";
    static constexpr std::string_view controlRoot =
        "/xyz/openbmc_project/control";

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
                    std::string(triggerInterface), std::string("Refresh"),
                    std::variant<bool>(true));
            }

            if (!any)
            {
                lg2::info("Inventory refresh: no Control.Trigger "
                          "InventoryData paths found under {R}",
                          "R", std::string(controlRoot));
            }
        },
        std::string(mapperBusName), std::string(mapperPath),
        std::string(mapperInterface), "GetSubTree",
        std::string(controlRoot), 0,
        std::vector<std::string>{std::string(triggerInterface)});
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

    std::vector<std::string> desiredInterfaces{std::string(systemInterface)};
    if (requireExactMatch)
    {
        desiredInterfaces.emplace_back(std::string(boardInterface));
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
        std::string(mapperBusName), std::string(mapperPath),
        std::string(mapperInterface), "GetSubTreePaths", searchPath, 0,
        desiredInterfaces);
}

void NvidiaInfo::discoverProcessorModulePaths(std::function<void()> callback)
{
    std::vector<std::string> ifaces = {std::string(processorModuleInterface)};

    bus->async_method_call(
        [this,
         cb = std::move(callback)](const boost::system::error_code& ec,
                                   const std::vector<std::string>& paths) {
            if (ec)
            {
                lg2::error(
                    "GetSubTreePaths (ProcessorModule) discovery failed: {E}",
                    "E", ec.message());
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
        std::string(mapperBusName), std::string(mapperPath),
        std::string(mapperInterface), "GetSubTreePaths", std::string("/"), 0,
        ifaces);
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

void NvidiaInfo::createInfoFromFile(const std::string& filePath)
{
    // Best-effort: file-based ingestion is used both for the D-Bus
    // CreateInfoFromFile method and for scanning the persistence directory
    // at startup. A broken file in the directory must never fail the D-Bus
    // call or abort startup recovery — log and skip instead.
    lg2::info("CreateInfoFromFile called (file={F})", "F", filePath);

    const std::string filename =
        std::filesystem::path(filePath).filename().string();
    const auto moduleIndex = moduleIndexFromPersistedFilename(filename);
    if (!moduleIndex)
    {
        lg2::warning("Skipping info file {F}: filename {N} is not of the form "
                     "ProcessorModule_<N>_Info.json",
                     "F", filePath, "N", filename);
        return;
    }

    std::string rawJson;
    try
    {
        std::ifstream in(filePath, std::ios::binary);
        if (!in)
        {
            lg2::warning("Skipping unreadable info file {F}", "F", filePath);
            return;
        }
        rawJson.assign(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
    }
    catch (const std::exception& e)
    {
        lg2::warning("Skipping info file {F} (read error): {E}", "F", filePath,
                     "E", e.what());
        return;
    }

    try
    {
        processAndPublish(std::format("ProcessorModule_{}", *moduleIndex),
                          std::move(rawJson), *moduleIndex,
                          /*persistOnSuccess=*/false, "CreateInfoFromFile");
    }
    catch (const std::exception& e)
    {
        // processAndPublish already logged the parse/validate failure and
        // cleared any stale terminus state. Swallow so a bad file in the
        // persistence directory doesn't fail the caller.
        lg2::warning("Skipping info file {F}: {E}", "F", filePath, "E",
                     e.what());
    }
}

void NvidiaInfo::createInfoFromJsonString(int32_t processorModuleIndex,
                                          const std::string& jsonStr)
{
    // Must match the range accepted by moduleIndexFromPersistedFilename().
    // If we ever persisted an index outside this range, startup recovery
    // would silently refuse to load it back.
    if (processorModuleIndex < 0 || processorModuleIndex > 9)
    {
        throw sdbusplus::exception::SdBusError(
            -EINVAL, "processor module index must be in the range 0..9");
    }

    lg2::info("CreateInfo called (JSON length={L}, processorModule={M})", "L",
              jsonStr.size(), "M", processorModuleIndex);

    processAndPublish(std::format("ProcessorModule_{}", processorModuleIndex),
                      jsonStr, processorModuleIndex,
                      /*persistOnSuccess=*/true, "CreateInfo");
}

void NvidiaInfo::processAndPublish(std::string terminusName,
                                   std::string rawJson, int32_t moduleIndex,
                                   bool persistOnSuccess,
                                   std::string_view context)
{
    TerminusData td;
    try
    {
        td = Json::parse(rawJson).get<TerminusData>();
        validate(td);
    }
    catch (const std::exception& e)
    {
        lg2::critical("{C} rejected for terminus {T}: {E}", "C", context, "T",
                      terminusName, "E", e.what());
        clearTerminusInfo(terminusName);
        removePersistedFile(moduleIndex);
        throw sdbusplus::exception::SdBusError(
            -EINVAL,
            std::format("{} rejected: {}", context, e.what()).c_str());
    }

    // Wrap td/rawJson in shared_ptrs: the discoverPaths callback is type-erased
    // through std::function, which requires its target to be copyable.
    // TerminusData contains non-copyable NvidiaCpu objects, so we indirect
    // through shared_ptrs and move out of them on invocation.
    auto tdPtr = std::make_shared<TerminusData>(std::move(td));
    auto rawPtr = std::make_shared<std::string>(std::move(rawJson));
    discoverPaths([this, name = std::move(terminusName), tdPtr, rawPtr,
                   moduleIndex, persistOnSuccess,
                   contextStr = std::string(context)]() mutable {
        try
        {
            // Move rawJson when we don't need to retain it for persistence.
            updateTerminusInfo(
                name, moduleIndex, std::move(*tdPtr),
                persistOnSuccess ? *rawPtr : std::move(*rawPtr));
        }
        catch (const std::exception& e)
        {
            lg2::error("{C}: exception publishing terminus {T}: {E}", "C",
                       contextStr, "T", name, "E", e.what());
            return;
        }
        if (persistOnSuccess && !persistInfoJson(moduleIndex, *rawPtr))
        {
            lg2::critical(
                "Failed to persist info JSON for terminus {T}; exiting "
                "to preserve round-trip guarantee",
                "T", name);
            std::exit(EXIT_FAILURE);
        }
    });
}

void NvidiaInfo::clearTerminusInfo(const std::string& terminusName)
{
    // Preserve rawJson so the deferred replay loop can re-parse the last
    // known good payload once motherboard/ProcessorModule paths appear.
    terminusInfos[terminusName].terminus = TerminusData{};
}

void NvidiaInfo::updateTerminusInfo(const std::string& terminusName,
                                    int32_t moduleIndex, TerminusData td,
                                    std::string rawJson)
{
    lg2::info(
        "Updating NVIDIA inventory for terminus={T} (motherboard={M}, "
        "processor_modules={N})",
        "T", terminusName, "M",
        motherboardPath.empty() ? "(not found)" : motherboardPath, "N",
        processorModulePaths.size());

    // Destruct old publisher objects first (removes their D-Bus interfaces)
    // before replacing with the new ones. Handles first-call and
    // replay-call cases uniformly.
    clearTerminusInfo(terminusName);

    auto& entry = terminusInfos[terminusName];
    entry.terminus = std::move(td);
    entry.rawJson = std::move(rawJson);
    entry.moduleIndex = moduleIndex;
    auto& stored = entry.terminus;

    // Callees downstream (maps, publish signatures, path arithmetic) are
    // all uint64_t; widen here once since the caller-facing type is int32_t.
    const auto moduleIdx = static_cast<uint64_t>(moduleIndex);
    const auto cpuCount = static_cast<uint64_t>(stored.cpus.size());

    for (std::size_t i = 0; i < stored.cpus.size(); ++i)
    {
        const uint64_t cpuIndex = moduleIdx * cpuCount +
                                  static_cast<uint64_t>(i);

        const std::string cpuPath =
            std::format("{}/cpu/CPU_{}", inventoryPath, cpuIndex);
        const std::string componentPath =
            std::format("{}/component/HGX_CPU_{}", inventoryPath, cpuIndex);
        const std::string boardPath = std::format(
            "{}/board/HGX_ProcessorModule_{}", inventoryPath, moduleIdx);

        stored.cpus[i].publish(*objServer, cpuPath, componentPath, boardPath,
                               cpuIndex);
        lg2::info("Published CPU {I} at {P}", "I", cpuIndex, "P", cpuPath);
    }

    for (std::size_t i = 0; i < stored.dimms.size(); ++i)
    {
        const std::string dimmPath =
            std::format("{}/dimm/ProcessorModule_{}_Memory_{}", inventoryPath,
                        moduleIdx, i);
        stored.dimms[i].publish(*objServer, dimmPath, motherboardPath);
        lg2::info("Created DIMM {I} at {P}", "I", i, "P", dimmPath);
    }

    if (!stored.pcieSlots.empty())
    {
        auto it = processorModulePaths.find(moduleIdx);
        if (it == processorModulePaths.end())
        {
            lg2::error("No processor module path found for module index {I} "
                       "(terminus {T}) — skipping PCIe slot publish",
                       "I", moduleIdx, "T", terminusName);
        }
        else
        {
            const std::string& modulePath = it->second;
            for (std::size_t i = 0; i < stored.pcieSlots.size(); ++i)
            {
                if (!stored.pcieSlots[i].isPresent())
                {
                    continue;
                }
                std::string pciePath = std::format(
                    "{}/{}_pcieslot{}", modulePath, terminusName, i);
                stored.pcieSlots[i].publish(*objServer, pciePath, modulePath,
                                            moduleIdx);
                lg2::info("Created PCIe slot inventory object: {P}", "P",
                          pciePath);
            }
        }
    }

    for (std::size_t i = 0; i < stored.tpms.size(); ++i)
    {
        std::string tpmPath =
            std::format("{}/chassis/motherboard/{}_tpm{}", inventoryPath,
                        terminusName, i);
        stored.tpms[i].publish(*objServer, tpmPath);
        lg2::info("Created TPM inventory object: {P}", "P", tpmPath);
    }

    lg2::info("NVIDIA inventory update complete for terminus={T}", "T",
              terminusName);
}

void NvidiaInfo::loadPersistedInfoFiles()
{
    // Recovery scans the persistence directory, but accepts *only* filenames
    // that match the exact shape we produce in persistedPathFor():
    // "ProcessorModule_<non-negative digits>_Info.json". Anything else is
    // silently ignored, since no well-formed write of ours could have
    // produced it. This supports an arbitrary number of module indices
    // without a hardcoded list.
    namespace fs = std::filesystem;

    std::error_code ec;
    const fs::path infoDir{std::string(persistedJsonDir)};
    if (!fs::exists(infoDir, ec))
    {
        if (ec)
        {
            lg2::error("Info directory {D} could not be checked: {M}", "D",
                       persistedJsonDir, "M", ec.message());
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
            // persistOnSuccess=false: the content is already on disk; no need
            // to rewrite it. processAndPublish will still remove the bad file
            // on parse/validate failure, which is the right behavior for our
            // own persisted state.
            processAndPublish(
                std::format("ProcessorModule_{}", *moduleIndex),
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
