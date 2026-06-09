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
#include <sdbusplus/exception.hpp>

#include <cerrno>
#include <string>
#include <vector>

namespace nvidia
{
namespace info
{

// Base for payload rejections. These are D-Bus exceptions, so they are
// thrown directly and serialized by sdbusplus' method dispatch without a
// translating catch; subclasses fix the error name. errno is EINVAL (the
// caller sent bad data). The detail string is carried as the description.
struct InfoError : sdbusplus::exception_t
{
    explicit InfoError(std::string detailArg) : detail(std::move(detailArg)) {}
    const char* description() const noexcept override
    {
        return detail.c_str();
    }
    const char* what() const noexcept override
    {
        return detail.c_str();
    }
    int get_errno() const noexcept override
    {
        return EINVAL;
    }
    std::string detail;
};

// JSON parse, schema, or structural payload problem.
struct SchemaViolation : InfoError
{
    using InfoError::InfoError;
    const char* name() const noexcept override
    {
        return "xyz.openbmc_project.NvidiaInfo.Error.SchemaViolation";
    }
};

// Payload inconsistent with the platform topology (socket/module/rank, or
// non-uniform component counts across a module's sockets).
struct InvalidConfiguration : InfoError
{
    using InfoError::InfoError;
    const char* name() const noexcept override
    {
        return "xyz.openbmc_project.NvidiaInfo.Error.InvalidConfiguration";
    }
};

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
// structure; must run before get<TerminusData>(). Throws SchemaViolation on
// any violation.
void validateAgainstSchema(const Json& doc);

// Runs each section's validate() (derivation only). Throws the first
// SchemaViolation it encounters, wrapped with section/index context.
void validate(TerminusData& t);

// Daemon-facing entry: parse rawJson, enforce the schema, derive, and run
// per-section validate(). Throws SchemaViolation on any malformed/invalid
// input (nlohmann parse/type errors are translated here, the one library
// boundary). Unexpected exceptions (e.g. bad_alloc) propagate.
TerminusData parseAndValidate(const std::string& rawJson);

} // namespace info
} // namespace nvidia
