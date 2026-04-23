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
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nvidia::info
{

NvidiaInfo::NvidiaInfo(const std::shared_ptr<boost::asio::io_context>& io,
                       std::shared_ptr<sdbusplus::asio::connection> conn,
                       std::shared_ptr<sdbusplus::asio::object_server> obj,
                       std::string invPath) :
    ioCtx(io), bus(std::move(conn)), objServer(std::move(obj)),
    inventoryPath(std::move(invPath))
{
    lg2::info("NVIDIA Info inventory path: {I}", "I", inventoryPath);

    setupMotherboardMatch();

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
                                discoverPaths([]() {});
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
    discoverPaths([]() {});
}

void NvidiaInfo::createInfoFromJsonString(int32_t processorModuleIndex,
                                          const std::string& jsonStr)
{
    lg2::info("CreateInfo called (JSON length={L}, processorModule={M})", "L",
              jsonStr.size(), "M", processorModuleIndex);
    discoverPaths([]() {});
}

} // namespace nvidia::info
