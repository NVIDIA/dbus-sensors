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

bool persistInfoJson(int32_t processorModuleIndex, const std::string& jsonStr)
{
    namespace fs = std::filesystem;

    const fs::path dir{std::string(persistedJsonDir)};
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
    {
        lg2::error("Failed to create info directory {D}: {E}", "D",
                   persistedJsonDir, "E", ec.message());
        return false;
    }

    const fs::path finalPath =
        dir / std::format("ProcessorModule_{}_Info.json", processorModuleIndex);
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
    const fs::path path = fs::path(std::string(persistedJsonDir)) /
        std::format("ProcessorModule_{}_Info.json", processorModuleIndex);
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

std::string NvidiaInfo::extractTerminusName(const std::string& filePath)
{
    namespace fs = std::filesystem;
    std::string filename = fs::path(filePath).stem().string();
    if (filename.ends_with("_Info"))
    {
        return filename.substr(0, filename.rfind('_'));
    }
    return filename;
}

uint64_t NvidiaInfo::parseModuleIndex(std::string_view terminusName)
{
    uint64_t moduleIndex = 0;
    std::string_view sv(terminusName);
    const auto pos = sv.rfind('_');
    if (pos != std::string_view::npos)
    {
        sv.remove_prefix(pos + 1);
        auto [ptr, ec] =
            std::from_chars(sv.data(), sv.data() + sv.size(), moduleIndex);
        if (ec != std::errc{} || ptr != sv.data() + sv.size())
        {
            lg2::warning(
                "Failed to parse module index from terminus '{NAME}', using 0",
                "NAME", std::string(terminusName));
            moduleIndex = 0;
        }
    }
    return moduleIndex;
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
                                                    name, std::move(td),
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
                auto pos = path.rfind('_');
                if (pos != std::string::npos)
                {
                    try
                    {
                        uint64_t idx = std::stoull(path.substr(pos + 1));
                        processorModulePaths[idx] = path;
                        lg2::info(
                            "Discovered processor module {I} at path: {P}", "I",
                            idx, "P", path);
                    }
                    catch (const std::exception& ex)
                    {
                        lg2::warning(
                            "Could not parse module index from path {P}: {E}",
                            "P", path, "E", ex.what());
                    }
                }
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
    lg2::info("CreateInfoFromFile called (file={F})", "F", filePath);

    std::string rawJson;
    try
    {
        std::ifstream in(filePath);
        if (!in.good())
        {
            throw std::runtime_error(
                std::format("failed to open {}", filePath));
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        rawJson = ss.str();
    }
    catch (const std::exception& e)
    {
        lg2::error("createInfoFromFile: {E}", "E", e.what());
        throw sdbusplus::exception::SdBusError(
            -EINVAL,
            std::format("CreateInfoFromFile read failed: {}", e.what())
                .c_str());
    }

    const std::string terminusName = extractTerminusName(filePath);

    TerminusData td;
    try
    {
        td = Json::parse(rawJson).get<TerminusData>();
        validate(td);
    }
    catch (const std::exception& e)
    {
        lg2::critical("CreateInfoFromFile rejected for terminus {T}: {E}", "T",
                      terminusName, "E", e.what());
        clearTerminusInfo(terminusName);
        removePersistedFile(
            static_cast<int32_t>(parseModuleIndex(terminusName)));
        throw sdbusplus::exception::SdBusError(
            -EINVAL,
            std::format("CreateInfoFromFile rejected: {}", e.what()).c_str());
    }

    // Wrap td in a shared_ptr: the discoverPaths callback is type-erased
    // through std::function, which requires its target to be copyable.
    // TerminusData now contains non-copyable NvidiaCpu objects, so we
    // indirect through a shared_ptr and move out of it on invocation.
    auto tdPtr = std::make_shared<TerminusData>(std::move(td));
    auto rawPtr = std::make_shared<std::string>(std::move(rawJson));
    discoverPaths([this, terminusName, tdPtr, rawPtr]() mutable {
        try
        {
            updateTerminusInfo(terminusName, std::move(*tdPtr),
                               std::move(*rawPtr));
        }
        catch (const std::exception& e)
        {
            lg2::error(
                "CreateInfoFromFile: exception publishing terminus {T}: {E}",
                "T", terminusName, "E", e.what());
        }
    });
}

void NvidiaInfo::createInfoFromJsonString(int32_t processorModuleIndex,
                                          const std::string& jsonStr)
{
    if (processorModuleIndex != 0 && processorModuleIndex != 1)
    {
        throw sdbusplus::exception::SdBusError(
            -EINVAL, "processor module index must be 0 or 1");
    }
    const std::string terminusName =
        std::format("ProcessorModule_{}", processorModuleIndex);

    lg2::info("CreateInfo called (JSON length={L}, processorModule={M})", "L",
              jsonStr.size(), "M", processorModuleIndex);

    TerminusData td;
    try
    {
        td = Json::parse(jsonStr).get<TerminusData>();
        validate(td);
    }
    catch (const std::exception& e)
    {
        lg2::critical("CreateInfo rejected for terminus {T}: {E}", "T",
                      terminusName, "E", e.what());
        clearTerminusInfo(terminusName);
        removePersistedFile(processorModuleIndex);
        throw sdbusplus::exception::SdBusError(
            -EINVAL,
            std::format("CreateInfo rejected: {}", e.what()).c_str());
    }

    auto tdPtr = std::make_shared<TerminusData>(std::move(td));
    auto rawPtr = std::make_shared<std::string>(jsonStr);
    discoverPaths([this, terminusName, processorModuleIndex, tdPtr,
                   rawPtr]() mutable {
        try
        {
            updateTerminusInfo(terminusName, std::move(*tdPtr),
                               std::string(*rawPtr));
        }
        catch (const std::exception& e)
        {
            lg2::error("CreateInfo: exception publishing terminus {T}: {E}",
                       "T", terminusName, "E", e.what());
            return;
        }
        if (!persistInfoJson(processorModuleIndex, *rawPtr))
        {
            lg2::critical(
                "Failed to persist info JSON for terminus {T}; exiting "
                "to preserve round-trip guarantee",
                "T", terminusName);
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
                                    TerminusData td, std::string rawJson)
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
    auto& stored = entry.terminus;

    const uint64_t moduleIndex = parseModuleIndex(terminusName);
    const auto cpuCount = static_cast<uint64_t>(stored.cpus.size());

    for (std::size_t i = 0; i < stored.cpus.size(); ++i)
    {
        const uint64_t cpuIndex =
            moduleIndex * cpuCount + static_cast<uint64_t>(i);

        const std::string cpuPath =
            std::format("{}/cpu/CPU_{}", inventoryPath, cpuIndex);
        const std::string componentPath =
            std::format("{}/component/HGX_CPU_{}", inventoryPath, cpuIndex);
        const std::string boardPath = std::format(
            "{}/board/HGX_ProcessorModule_{}", inventoryPath, moduleIndex);

        stored.cpus[i].publish(*objServer, cpuPath, componentPath, boardPath,
                               cpuIndex);
        lg2::info("Published CPU {I} at {P}", "I", cpuIndex, "P", cpuPath);
    }

    for (std::size_t i = 0; i < stored.dimms.size(); ++i)
    {
        const std::string dimmPath =
            std::format("{}/dimm/ProcessorModule_{}_Memory_{}", inventoryPath,
                        moduleIndex, i);
        stored.dimms[i].publish(*objServer, dimmPath, motherboardPath);
        lg2::info("Created DIMM {I} at {P}", "I", i, "P", dimmPath);
    }

    if (!stored.pcieSlots.empty())
    {
        auto it = processorModulePaths.find(moduleIndex);
        if (it == processorModulePaths.end())
        {
            lg2::error("No processor module path found for module index {I} "
                       "(terminus {T}) — skipping PCIe slot publish",
                       "I", moduleIndex, "T", terminusName);
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
                                            moduleIndex);
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
    namespace fs = std::filesystem;
    static constexpr std::string_view fileSuffix = "_Info.json";

    std::error_code ec;
    const fs::path infoDir{std::string(persistedJsonDir)};
    const bool dirExists = fs::exists(infoDir, ec);
    if (ec)
    {
        lg2::error(
            "Info directory {D} could not be checked: error code {C}, {M}", "D",
            persistedJsonDir, "C", ec.value(), "M", ec.message());
        return;
    }
    if (!dirExists)
    {
        lg2::info("Info directory {D} does not exist, skipping recovery", "D",
                  persistedJsonDir);
        return;
    }

    try
    {
        for (const auto& entry : fs::directory_iterator(infoDir))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            const std::string filename = entry.path().filename().string();
            if (!filename.ends_with(fileSuffix))
            {
                continue;
            }

            lg2::info("Recovery: loading existing info file: {F}", "F",
                      entry.path().string());
            try
            {
                createInfoFromFile(entry.path().string());
            }
            catch (const std::exception& e)
            {
                lg2::error(
                    "Recovery: exception while loading info file {F}, "
                    "skipped: {E}",
                    "F", entry.path().string(), "E", e.what());
            }
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to scan info directory {D}: {E}", "D",
                   persistedJsonDir, "E", e.what());
    }
}

} // namespace nvidia::info
