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

#include "NvidiaInfoEnums.hpp"
#include "NvidiaInfoPublisher.hpp"

#include <sdbusplus/asio/object_server.hpp>

#include <string>

namespace nvidia
{
namespace info
{

// NvidiaTpm registers the 4 D-Bus interfaces that represent a single
// Trusted Platform Module at
// .../chassis/motherboard/<terminusName>_tpm<i>: Item.TrustedComponent,
// Item, Decorator.Asset, and Software.Version.
//
// Default-constructed by the vector<NvidiaTpm> inside TerminusData,
// populated via from_json, checked by validate(), and only then
// registered on D-Bus by publish(). The Publisher base removes every
// interface on destruction.
class NvidiaTpm : public Publisher
{
  public:
    // Every TPM field is optional in the original NvidiaInfoTpm, so this
    // has no invariants to enforce. Kept declared so validateEach can
    // invoke it uniformly with the other sections.
    void validate();

    // Registers the 4 interfaces on objServer. Must be called after
    // validate(). The Publisher base retains objServer for destruction.
    void publish(sdbusplus::asio::object_server& objServer,
                 const std::string& tpmPath);

    // JSON-populated fields. from_json fills these; all are optional and
    // default to empty strings. PrettyName and Model are derived from
    // majorSpecVersion inside publish().
    std::string manufacturer;     // "Manufacturer" (optional)
    std::string version;          // "Version" (optional)
    std::string majorSpecVersion; // "MajorSpecVersion" (optional)
};

void from_json(const Json& j, NvidiaTpm& t);

} // namespace info
} // namespace nvidia
