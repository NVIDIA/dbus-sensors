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

#include <nlohmann/json.hpp>

#include <vector>

namespace nvidia
{
namespace info
{

// A parsed and (after validate()) validated in-memory representation of a
// per-terminus Info JSON payload. Later commits will grow this with
// vector<NvidiaTpm>.
struct TerminusData
{
    std::vector<NvidiaCpu> cpus;
    std::vector<NvidiaDimm> dimms;
    std::vector<NvidiaPcie> pcieSlots;
};

void from_json(const Json& j, TerminusData& t);

// Calls .validate() on each element of each section. Propagates the first
// std::invalid_argument encountered, wrapped with section/index context.
void validate(TerminusData& t);

} // namespace info
} // namespace nvidia
