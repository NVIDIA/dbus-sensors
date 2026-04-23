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
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

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

void NvidiaInfo::createInfoFromFile(const std::string& filePath)
{
    lg2::info("CreateInfoFromFile called (file={F})", "F", filePath);
}

void NvidiaInfo::createInfoFromJsonString(int32_t processorModuleIndex,
                                          const std::string& jsonStr)
{
    lg2::info("CreateInfo called (JSON length={L}, processorModule={M})", "L",
              jsonStr.size(), "M", processorModuleIndex);
}

} // namespace nvidia::info
