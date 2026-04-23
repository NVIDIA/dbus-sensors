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

#include "NvidiaInfoTpm.hpp"

#include <phosphor-logging/lg2.hpp>

#include <string>

namespace nvidia
{
namespace info
{

void from_json(const Json& j, NvidiaTpm& t)
{
    t.manufacturer = j.value("Manufacturer", std::string());
    t.version = j.value("Version", std::string());
    t.majorSpecVersion = j.value("MajorSpecVersion", std::string());
}

void NvidiaTpm::validate()
{
    // All TPM fields are optional; nothing to validate — matches original
    // NvidiaInfoTpm behavior.
}

NvidiaTpm::~NvidiaTpm()
{
    if (server == nullptr)
    {
        return;
    }
    for (auto* iface : {&tpmIface, &itemIface, &assetIface, &versionIface})
    {
        if (*iface)
        {
            server->remove_interface(*iface);
        }
    }
}

void NvidiaTpm::publish(sdbusplus::asio::object_server& objServer,
                        const std::string& tpmPath)
{
    server = &objServer;

    std::string prettyName;
    std::string model;
    if (!majorSpecVersion.empty())
    {
        prettyName = "TPM " + majorSpecVersion;
        model = "TPM " + majorSpecVersion;
    }

    tpmIface = objServer.add_interface(
        tpmPath, "xyz.openbmc_project.Inventory.Item.TrustedComponent");
    itemIface =
        objServer.add_interface(tpmPath, "xyz.openbmc_project.Inventory.Item");
    assetIface = objServer.add_interface(
        tpmPath, "xyz.openbmc_project.Inventory.Decorator.Asset");
    versionIface = objServer.add_interface(
        tpmPath, "xyz.openbmc_project.Software.Version");

    tpmIface->register_property(
        "TrustedComponentType",
        std::string("xyz.openbmc_project.Inventory.Item.TrustedComponent."
                    "ComponentAttachType.Discrete"));

    itemIface->register_property("PrettyName", prettyName);
    itemIface->register_property("Present", true);

    assetIface->register_property("Manufacturer", manufacturer);
    assetIface->register_property("Model", model);

    versionIface->register_property("Version", version);
    versionIface->register_property(
        "Purpose",
        std::string(
            "xyz.openbmc_project.Software.Version.VersionPurpose.Other"));

    tpmIface->initialize();
    itemIface->initialize();
    assetIface->initialize();
    versionIface->initialize();

    lg2::info("Created TPM inventory object: {P}", "P", tpmPath);
}

} // namespace info
} // namespace nvidia
