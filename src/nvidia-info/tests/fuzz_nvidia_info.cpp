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

// LibFuzzer entry point for the per-terminus Info JSON pipeline.  Mirrors
// libcper's fuzz_cper_buf_to_ir.c shape: feed raw bytes to the schema gate
// and assert that any input the schema accepts can also be derived and
// validated without throwing.  Sanitizers (ASan + LeakSan) catch UB and
// leaks; the explicit assert catches schema/derivation drift.

#include "NvidiaInfoSchema.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    nvidia::info::Json doc;
    try
    {
        doc = nvidia::info::Json::parse(
            std::string_view(reinterpret_cast<const char*>(data), size));
    }
    catch (const nlohmann::json::exception&)
    {
        // Not parseable JSON; libFuzzer will mutate further.  Not an error.
        return 0;
    }

    try
    {
        nvidia::info::validateAgainstSchema(doc);
    }
    catch (const std::invalid_argument&)
    {
        // Schema rejected the document.  Expected path for most inputs.
        return 0;
    }

    // Invariant: if the schema admits a document, derivation and per-section
    // validate() must both succeed.  A throw here is a real bug -- either
    // the schema is too permissive or the section validators are too strict.
    nvidia::info::TerminusData t;
    try
    {
        nvidia::info::from_json(doc, t);
        nvidia::info::validate(t);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "post-schema failure: %s\n", e.what());
        assert(false && "schema-admitted input failed derivation/validate");
    }

    return 0;
}
