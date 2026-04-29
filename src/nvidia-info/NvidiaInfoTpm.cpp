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
    j.at("Manufacturer").get_to(t.manufacturer);
    j.at("Version").get_to(t.version);
    j.at("MajorSpecVersion").get_to(t.majorSpecVersion);
}

void NvidiaTpm::validate()
{
    // Schema covers required fields; no-op kept for validateEach<> symmetry.
}

void NvidiaTpm::publish(sdbusplus::asio::object_server& objServer,
                        const std::string& tpmPath)
{
    const std::string prettyName = "TPM " + majorSpecVersion;
    const std::string model = prettyName;

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
