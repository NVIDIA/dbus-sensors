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
    // Nothing to do. Required-field presence is enforced by the JSON
    // schema (validated up-front in processAndPublish). Kept as a no-op so
    // the generic validateEach<>() walker still has a uniform shape across
    // sections.
}

void NvidiaTpm::publish(sdbusplus::asio::object_server& objServer,
                        const std::string& tpmPath)
{
    std::string prettyName;
    std::string model;
    if (!majorSpecVersion.empty())
    {
        prettyName = "TPM " + majorSpecVersion;
        model = "TPM " + majorSpecVersion;
    }

    auto& tpm =
        add(tpmPath, "xyz.openbmc_project.Inventory.Item.TrustedComponent",
            objServer);
    tpm.register_property(
        "TrustedComponentType",
        std::string("xyz.openbmc_project.Inventory.Item.TrustedComponent."
                    "ComponentAttachType.Discrete"));

    auto& item = add(tpmPath, "xyz.openbmc_project.Inventory.Item", objServer);
    item.register_property("PrettyName", prettyName);
    item.register_property("Present", true);

    auto& asset = add(tpmPath, "xyz.openbmc_project.Inventory.Decorator.Asset",
                      objServer);
    asset.register_property("Manufacturer", manufacturer);
    asset.register_property("Model", model);

    auto& ver = add(tpmPath, "xyz.openbmc_project.Software.Version", objServer);
    ver.register_property("Version", version);
    ver.register_property(
        "Purpose",
        std::string(
            "xyz.openbmc_project.Software.Version.VersionPurpose.Other"));

    initializeAll();

    lg2::info("Published TPM at {P}", "P", tpmPath);
}

} // namespace info
} // namespace nvidia
