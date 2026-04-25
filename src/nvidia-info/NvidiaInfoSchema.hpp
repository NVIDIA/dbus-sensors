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

#include "NvidiaInfoCpu.hpp"
#include "NvidiaInfoDimm.hpp"
#include "NvidiaInfoEnums.hpp"
#include "NvidiaInfoPcie.hpp"
#include "NvidiaInfoTpm.hpp"

#include <nlohmann/json.hpp>

#include <vector>

namespace nvidia
{
namespace info
{

// In-memory representation of a per-terminus Info JSON payload.
struct TerminusData
{
    std::vector<NvidiaCpu> cpus;
    std::vector<NvidiaDimm> dimms;
    std::vector<NvidiaPcie> pcieSlots;
    std::vector<NvidiaTpm> tpms;
};

void from_json(const Json& j, TerminusData& t);

// Validates doc against the embedded JSON schema. Authoritative guard for
// structure; must run before get<TerminusData>(). Throws
// std::invalid_argument on any violation.
void validateAgainstSchema(const Json& doc);

// Runs each section's validate() (derivation only). Throws the first
// std::invalid_argument it encounters, wrapped with section/index context.
void validate(TerminusData& t);

} // namespace info
} // namespace nvidia
