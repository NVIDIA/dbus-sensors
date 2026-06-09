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

// Unit tests for the nvidia-info module. Three layers:
//   1. Pure schema/derivation/validate -- no D-Bus.
//   2. Pure persistence helpers -- file I/O against a tmpdir, no D-Bus.
//   3. Live D-Bus tests against the real system bus, exercising the
//      NvidiaInfo lifecycle (CreateInfo body, recovery on construction).
//      These auto-skip on hosts without a system bus, so the same binary
//      runs both as a host-side `meson test` and on a BMC.

#include "NvidiaInfo.hpp"
#include "NvidiaInfoCpu.hpp"
#include "NvidiaInfoDimm.hpp"
#include "NvidiaInfoEnums.hpp"
#include "NvidiaInfoPcie.hpp"
#include "NvidiaInfoPersistence.hpp"
#include "NvidiaInfoSchema.hpp"
#include "NvidiaInfoTpm.hpp"

#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/exception.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

namespace nvi = nvidia::info;
namespace pst = nvidia::info::persistence;
using Json = nvi::Json;

namespace
{

// Returns a freshly-built schema-valid TerminusData JSON object covering
// every required field for one of each section. Tests mutate copies of
// this to exercise specific reject paths.
Json validBase()
{
    return Json{
        {"Processor",
         Json::array({Json{{"Socket", 1},
                           {"Family", "ARMv9"},
                           {"Manufacturer", "NVIDIA"},
                           {"Id", "0x036b0410"},
                           {"Version", "Vera B01"},
                           {"MaxSpeedInMhz", 4000},
                           {"SerialNumber", "SN-CPU-1"},
                           {"CoreCount", 88},
                           {"ThreadCount", 176},
                           {"Model", "Vera"},
                           {"ModelRevision", "B01"},
                           {"SKU", "VERA-000"}}})},
        {"Memory",
         Json::array({Json{
             {"MemoryTotalWidth", 144},
             {"MemoryDataWidth", 128},
             {"MemorySizeKB", 26843545},
             {"FormFactor", "SOCAMM"},
             {"MemoryDeviceLocator", "LP5x_16"},
             {"MemoryType", "LPDDR5-SDRAM"},
             {"MaxMemorySpeedInMHz", 7500},
             {"Manufacturer", "SAMSUNG"},
             {"SerialNumber", "SN-MEM-1"},
             {"SKU", "MEM-LP5X"},
             {"PartNumber", "PN-MEM-1"},
             {"MemoryConfiguredSpeedInMhz", 7500},
             {"Model", "LPDDR5-7500"},
             {"ECC", true},
             {"MemoryMedia", "DRAM"},
             {"ProcessorModuleIndex", 1}}})},
        {"PCIeSlots",
         Json::array({Json{{"Present", true},
                           {"Generation", 5},
                           {"HotPluggable", true},
                           {"SlotType", "M_2"},
                           {"MaxLinkSpeed", 5},
                           {"MaxLinkWidth", 16},
                           {"ProcessorModuleIndex", 1},
                           {"SegmentControllerIndex", 0},
                           {"PortType", "Bi-directional"},
                           {"PortProtocol", "CXL"},
                           {"RootPort", 0},
                           {"LocationCode", "UPHY0:0-15"},
                           {"Lanes", 16}}})},
        {"TPM", Json::array({Json{{"Manufacturer", "NTC"},
                                  {"MajorSpecVersion", "2.0"},
                                  {"Version", "7.2.3.0"}}})},
    };
}

// validBase() with the Processor socket overridden. On the test build
// (sockets-per-module defaults to 1) a payload is only valid for module M
// when its socket is M, so live tests pair createInfoFromJsonString(M, ...)
// with socket M.
Json validBaseWithSocket(int32_t socket)
{
    Json j = validBase();
    j["Processor"][0]["Socket"] = socket;
    return j;
}

// validBaseWithSocket() with two of each non-Processor section, to exercise
// the per-array DIMM/PCIe/TPM publish loops. One CPU per payload, so
// Processor stays single.
Json multiComponentPayload(int32_t socket)
{
    Json j = validBaseWithSocket(socket);
    {
        Json mem2 = j["Memory"][0];
        mem2["MemoryDeviceLocator"] = "LP5x_17";
        mem2["SerialNumber"] = "SN-MEM-2";
        j["Memory"].push_back(std::move(mem2));
    }
    {
        Json slot2 = j["PCIeSlots"][0];
        slot2["LocationCode"] = "UPHY0:16-31";
        j["PCIeSlots"].push_back(std::move(slot2));
    }
    {
        Json tpm2 = j["TPM"][0];
        j["TPM"].push_back(std::move(tpm2));
    }
    return j;
}

// Drives schema -> derive -> per-section validate end-to-end. Throws on
// rejection just like processAndPublish would.
void parseAndDerive(const Json& doc)
{
    nvi::validateAgainstSchema(doc);
    nvi::TerminusData td;
    nvi::from_json(doc, td);
    nvi::validate(td);
}

// Returns a schema-valid TerminusData JSON with two of every section,
// including two Processors. Used only by the pure schema/derivation test:
// the schema permits multiple Processors, even though the live publish
// model is one CPU (one socket) per payload.
Json validBaseMulti()
{
    Json j = validBase();
    {
        Json cpu2 = j["Processor"][0];
        cpu2["Socket"] = 2;
        cpu2["SerialNumber"] = "SN-CPU-2";
        j["Processor"].push_back(std::move(cpu2));
    }
    {
        Json mem2 = j["Memory"][0];
        mem2["MemoryDeviceLocator"] = "LP5x_17";
        mem2["SerialNumber"] = "SN-MEM-2";
        j["Memory"].push_back(std::move(mem2));
    }
    {
        Json slot2 = j["PCIeSlots"][0];
        slot2["LocationCode"] = "UPHY0:16-31";
        j["PCIeSlots"].push_back(std::move(slot2));
    }
    {
        // Schema doesn't require TPM fields to differ between entries;
        // a straight duplicate exercises the per-index publish path.
        Json tpm2 = j["TPM"][0];
        j["TPM"].push_back(std::move(tpm2));
    }
    return j;
}

// Creates a unique tmpdir under $TMPDIR and returns its path. Callers
// own cleanup.
std::filesystem::path makeTmpDir(const char* prefix)
{
    namespace fs = std::filesystem;
    fs::path base =
        fs::temp_directory_path() / std::format("{}-XXXXXX", prefix);
    std::array<char, 256> buf{};
    std::strncpy(buf.data(), base.string().c_str(), buf.size() - 1);
    char* d = ::mkdtemp(buf.data());
    if (d == nullptr)
    {
        throw std::runtime_error("mkdtemp failed");
    }
    return fs::path{d};
}

// Reads a small file's content into memory. Used to round-trip
// persisted JSON through the filesystem.
std::string slurp(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

} // namespace

// ============================================================================
// Schema gate (validateAgainstSchema + validate(TerminusData&))
// ============================================================================

TEST(SchemaGate, AcceptsValidBase)
{
    EXPECT_NO_THROW(parseAndDerive(validBase()));
}

TEST(SchemaGate, AcceptsMultiInstancePerSection)
{
    // Schema permits arrays of any length per section. Pure equivalent
    // of NvidiaInfoLiveTest.AcceptsMultipleInstancesPerSection -- proves
    // that multi-instance JSON parses, derives, and validates without
    // any D-Bus involvement.
    const Json doc = validBaseMulti();
    ASSERT_EQ(doc["Processor"].size(), 2U);
    ASSERT_EQ(doc["Memory"].size(), 2U);
    ASSERT_EQ(doc["PCIeSlots"].size(), 2U);
    ASSERT_EQ(doc["TPM"].size(), 2U);
    EXPECT_NO_THROW(parseAndDerive(doc));
}

TEST(SchemaGate, RejectsMissingTopLevel)
{
    for (const char* key : {"Processor", "Memory", "PCIeSlots"})
    {
        Json j = validBase();
        j.erase(key);
        EXPECT_THROW(nvi::validateAgainstSchema(j), nvi::SchemaViolation)
            << "schema must require top-level " << key;
    }
}

TEST(SchemaGate, AcceptsMissingTpm)
{
    // TPM is optional: the SatMC/MB2 no longer sends it. A payload without
    // the TPM key must pass schema validation, parse, and derive cleanly
    // (leaving tpms empty) rather than throwing out_of_range in from_json.
    Json j = validBase();
    j.erase("TPM");
    EXPECT_NO_THROW(parseAndDerive(j));

    nvi::validateAgainstSchema(j);
    nvi::TerminusData td;
    nvi::from_json(j, td);
    EXPECT_TRUE(td.tpms.empty());
}

TEST(SchemaGate, RejectsMissingPerSectionRequired)
{
    struct Case
    {
        const char* section;
        const char* field;
    };
    // Mirrors the `required` arrays in schema.json. Every entry must
    // cause validateAgainstSchema() to throw. New required fields in the
    // schema must be added here so the schema gate stays the rejection
    // point (and downstream from_json `j.at()` calls don't quietly mask
    // a relaxed schema).
    static constexpr std::array kCases{
        // Processor: 12 required fields.
        Case{"Processor", "Socket"},
        Case{"Processor", "Family"},
        Case{"Processor", "Manufacturer"},
        Case{"Processor", "Id"},
        Case{"Processor", "Version"},
        Case{"Processor", "MaxSpeedInMhz"},
        Case{"Processor", "SerialNumber"},
        Case{"Processor", "CoreCount"},
        Case{"Processor", "ThreadCount"},
        Case{"Processor", "Model"},
        Case{"Processor", "ModelRevision"},
        Case{"Processor", "SKU"},
        // Memory: 16 required fields.
        Case{"Memory", "MemoryTotalWidth"},
        Case{"Memory", "MemoryDataWidth"},
        Case{"Memory", "MemorySizeKB"},
        Case{"Memory", "FormFactor"},
        Case{"Memory", "MemoryDeviceLocator"},
        Case{"Memory", "MemoryType"},
        Case{"Memory", "MaxMemorySpeedInMHz"},
        Case{"Memory", "Manufacturer"},
        Case{"Memory", "SerialNumber"},
        Case{"Memory", "SKU"},
        Case{"Memory", "PartNumber"},
        Case{"Memory", "MemoryConfiguredSpeedInMhz"},
        Case{"Memory", "Model"},
        Case{"Memory", "ECC"},
        Case{"Memory", "MemoryMedia"},
        Case{"Memory", "ProcessorModuleIndex"},
        // PCIeSlots: 13 required fields.
        Case{"PCIeSlots", "Present"},
        Case{"PCIeSlots", "Generation"},
        Case{"PCIeSlots", "HotPluggable"},
        Case{"PCIeSlots", "SlotType"},
        Case{"PCIeSlots", "MaxLinkSpeed"},
        Case{"PCIeSlots", "MaxLinkWidth"},
        Case{"PCIeSlots", "Lanes"},
        Case{"PCIeSlots", "ProcessorModuleIndex"},
        Case{"PCIeSlots", "SegmentControllerIndex"},
        Case{"PCIeSlots", "PortType"},
        Case{"PCIeSlots", "PortProtocol"},
        Case{"PCIeSlots", "RootPort"},
        Case{"PCIeSlots", "LocationCode"},
        // TPM: 3 required fields.
        Case{"TPM", "Manufacturer"},
        Case{"TPM", "MajorSpecVersion"},
        Case{"TPM", "Version"},
    };
    for (const auto& c : kCases)
    {
        Json j = validBase();
        j[c.section][0].erase(c.field);
        EXPECT_THROW(nvi::validateAgainstSchema(j), nvi::SchemaViolation)
            << c.section << "." << c.field << " must be required by the schema";
    }
}

TEST(SchemaGate, RejectsOutOfRangeIntegers)
{
    {
        Json j = validBase();
        j["Processor"][0]["Socket"] = 256;
        EXPECT_THROW(nvi::validateAgainstSchema(j), nvi::SchemaViolation);
    }
    {
        Json j = validBase();
        j["Processor"][0]["CoreCount"] = 0; // minimum 1
        EXPECT_THROW(nvi::validateAgainstSchema(j), nvi::SchemaViolation);
    }
    {
        Json j = validBase();
        j["PCIeSlots"][0]["Generation"] = 7; // 0-6
        EXPECT_THROW(nvi::validateAgainstSchema(j), nvi::SchemaViolation);
    }
    {
        Json j = validBase();
        j["PCIeSlots"][0]["Lanes"] = 65; // 0-64
        EXPECT_THROW(nvi::validateAgainstSchema(j), nvi::SchemaViolation);
    }
    {
        Json j = validBase();
        j["PCIeSlots"][0]["MaxLinkSpeed"] = 7;
        EXPECT_THROW(nvi::validateAgainstSchema(j), nvi::SchemaViolation);
    }
    {
        Json j = validBase();
        j["Memory"][0]["ProcessorModuleIndex"] = 10;
        EXPECT_THROW(nvi::validateAgainstSchema(j), nvi::SchemaViolation);
    }
}

TEST(SchemaGate, RejectsBadEnumStrings)
{
    {
        Json j = validBase();
        j["Memory"][0]["FormFactor"] = "BOGUS";
        EXPECT_THROW(nvi::validateAgainstSchema(j), nvi::SchemaViolation);
    }
    {
        Json j = validBase();
        j["Memory"][0]["MemoryType"] = "DDR5x";
        EXPECT_THROW(nvi::validateAgainstSchema(j), nvi::SchemaViolation);
    }
    {
        Json j = validBase();
        j["Memory"][0]["MemoryMedia"] = "GDDR";
        EXPECT_THROW(nvi::validateAgainstSchema(j), nvi::SchemaViolation);
    }
}

TEST(SchemaGate, ProcessorIdRegex)
{
    auto withId = [](const char* id) {
        Json j = validBase();
        j["Processor"][0]["Id"] = id;
        return j;
    };

    // Accept: bare hex, 0x prefix, 0X prefix, 1 digit, 16 digits.
    EXPECT_NO_THROW(nvi::validateAgainstSchema(withId("a")));
    EXPECT_NO_THROW(nvi::validateAgainstSchema(withId("036b0410")));
    EXPECT_NO_THROW(nvi::validateAgainstSchema(withId("0xDEADBEEFCAFEBABE")));
    EXPECT_NO_THROW(nvi::validateAgainstSchema(withId("0XDeadBeefCafeBabe")));

    // Reject: empty, 17 digits, non-hex, embedded space.
    EXPECT_THROW(nvi::validateAgainstSchema(withId("")), nvi::SchemaViolation);
    EXPECT_THROW(nvi::validateAgainstSchema(withId("0xDEADBEEFCAFEBABE0")),
                 nvi::SchemaViolation);
    EXPECT_THROW(nvi::validateAgainstSchema(withId("0xZZ")),
                 nvi::SchemaViolation);
    EXPECT_THROW(nvi::validateAgainstSchema(withId(" 0x1")),
                 nvi::SchemaViolation);
}

TEST(SchemaGate, ValidateTagsSectionAndIndex)
{
    // Schema doesn't constrain idStr beyond the regex, but NvidiaCpu::validate
    // re-parses it. We bypass the schema (which would reject anyway) by
    // crafting a TerminusData directly so we can observe the
    // "Processor[<i>]: ..." prefix that validateEach<>() applies.
    //
    // cpus[0..1] need valid idStr values: NvidiaCpu::validate() rejects
    // the default-constructed empty string, so without pre-filling the
    // earlier slots the throw fires at index 0 and we never reach
    // index 2.
    nvi::TerminusData td;
    td.cpus.resize(3);
    td.cpus[0].idStr = "0x1";
    td.cpus[1].idStr = "0x2";
    td.cpus[2].idStr = "not-hex";

    try
    {
        nvi::validate(td);
        FAIL() << "validate() must throw on bad idStr";
    }
    catch (const nvi::SchemaViolation& e)
    {
        const std::string what = e.what();
        EXPECT_NE(what.find("Processor[2]:"), std::string::npos)
            << "missing section/index tag in: " << what;
    }
}

// ============================================================================
// CPU (NvidiaCpu::validate hex parser)
// ============================================================================

TEST(NvidiaCpu, HexParsesAllSpellings)
{
    struct Case
    {
        const char* in;
        uint64_t expected;
    };
    static constexpr std::array kCases{
        Case{"0", 0x0ULL},
        Case{"a", 0xaULL},
        Case{"0x1", 0x1ULL},
        Case{"0X1", 0x1ULL},
        Case{"0x036b0410", 0x036b0410ULL},
        Case{"036b0410", 0x036b0410ULL},
        Case{"0xDEADBEEFCAFEBABE", 0xDEADBEEFCAFEBABEULL},
        Case{"0xdeadbeefcafebabe", 0xDEADBEEFCAFEBABEULL},
    };
    for (const auto& c : kCases)
    {
        nvi::NvidiaCpu cpu;
        cpu.idStr = c.in;
        ASSERT_NO_THROW(cpu.validate()) << c.in;
        EXPECT_EQ(cpu.idValue, c.expected) << c.in;
    }
}

TEST(NvidiaCpu, HexRejectsGarbage)
{
    for (const char* bad : {"", "0x", "0xZZ", "0x1g", "1 ", " 1", "0x1\n"})
    {
        nvi::NvidiaCpu cpu;
        cpu.idStr = bad;
        EXPECT_THROW(cpu.validate(), std::invalid_argument)
            << "input: '" << bad << "'";
    }
}

TEST(NvidiaCpu, FromJsonBindsAllRequiredFields)
{
    Json j = validBase()["Processor"][0];
    nvi::NvidiaCpu cpu;
    nvi::from_json(j, cpu);
    EXPECT_EQ(cpu.socketNum, 1U);
    EXPECT_EQ(cpu.family, "ARMv9");
    EXPECT_EQ(cpu.idStr, "0x036b0410");
    EXPECT_EQ(cpu.coreCount, 88);
    EXPECT_EQ(cpu.threadCount, 176);
    EXPECT_EQ(cpu.maxSpeedInMhz, 4000U);
    EXPECT_EQ(cpu.manufacturer, "NVIDIA");
    EXPECT_EQ(cpu.model, "Vera");
    EXPECT_EQ(cpu.modelRevision, "B01");
    EXPECT_EQ(cpu.serialNumber, "SN-CPU-1");
    EXPECT_EQ(cpu.version, "Vera B01");
    EXPECT_EQ(cpu.sku, "VERA-000");
}

// ============================================================================
// DIMM
// ============================================================================

TEST(NvidiaDimm, FromJsonHonorsOptionals)
{
    // Strip every optional field that has a j.value(...) default and
    // confirm it falls back without throwing.
    Json j = validBase()["Memory"][0];
    j.erase("MemorySizeKB");
    j.erase("MemoryDataWidth");
    j.erase("MemoryTotalWidth");
    j.erase("MaxMemorySpeedInMHz");
    j.erase("MemoryConfiguredSpeedInMhz");
    j.erase("ECC");
    j.erase("Model");
    j.erase("PartNumber");
    j.erase("SerialNumber");
    j.erase("SKU");

    nvi::NvidiaDimm d;
    nvi::from_json(j, d);
    EXPECT_EQ(d.sizeKB, 0U);
    EXPECT_EQ(d.dataWidth, 0);
    EXPECT_EQ(d.totalWidth, 0);
    EXPECT_EQ(d.maxSpeed, 0);
    EXPECT_EQ(d.configSpeed, 0);
    EXPECT_FALSE(d.ecc);
    EXPECT_TRUE(d.model.empty());
    EXPECT_TRUE(d.partNumber.empty());
    EXPECT_TRUE(d.serialNumber.empty());
    EXPECT_TRUE(d.sku.empty());
    // Required field round-trip.
    EXPECT_EQ(d.locator, "LP5x_16");
    EXPECT_EQ(d.memoryType, nvi::MemoryType::LPDDR5_SDRAM);
    EXPECT_EQ(d.memoryMedia, nvi::MemoryMedia::DRAM);
    EXPECT_EQ(d.formFactor, nvi::FormFactor::SOCAMM);
}

TEST(NvidiaDimm, FromJsonRequiresLocator)
{
    Json j = validBase()["Memory"][0];
    j.erase("MemoryDeviceLocator");
    nvi::NvidiaDimm d;
    EXPECT_THROW(nvi::from_json(j, d), Json::out_of_range);
}

TEST(NvidiaDimm, FromJsonBindsAllFields)
{
    // Companion to FromJsonHonorsOptionals (which exercises the
    // all-stripped path). Here every field of validBase() Memory[0] is
    // populated; assert every member NvidiaDimm carries lands correctly.
    Json j = validBase()["Memory"][0];
    nvi::NvidiaDimm d;
    nvi::from_json(j, d);
    EXPECT_EQ(d.sizeKB, 26843545U);
    EXPECT_EQ(d.dataWidth, 128);
    EXPECT_EQ(d.totalWidth, 144);
    EXPECT_EQ(d.locator, "LP5x_16");
    EXPECT_EQ(d.maxSpeed, 7500);
    EXPECT_EQ(d.configSpeed, 7500);
    EXPECT_EQ(d.memoryType, nvi::MemoryType::LPDDR5_SDRAM);
    EXPECT_EQ(d.formFactor, nvi::FormFactor::SOCAMM);
    EXPECT_TRUE(d.ecc);
    EXPECT_EQ(d.manufacturer, "SAMSUNG");
    EXPECT_EQ(d.model, "LPDDR5-7500");
    EXPECT_EQ(d.partNumber, "PN-MEM-1");
    EXPECT_EQ(d.serialNumber, "SN-MEM-1");
    EXPECT_EQ(d.sku, "MEM-LP5X");
    EXPECT_EQ(d.memoryMedia, nvi::MemoryMedia::DRAM);
}

// ============================================================================
// PCIe
// ============================================================================

TEST(NvidiaPcie, SlotTypeAcceptsBothSpellings)
{
    auto deriveSlotType = [](const char* s) {
        Json j = validBase()["PCIeSlots"][0];
        j["SlotType"] = s;
        nvi::NvidiaPcie p;
        nvi::from_json(j, p);
        return p.slotType;
    };

    EXPECT_EQ(deriveSlotType("M_2"), nvi::SlotType::M2);
    EXPECT_EQ(deriveSlotType("M2"), nvi::SlotType::M2);
    EXPECT_EQ(deriveSlotType("U_2"), nvi::SlotType::U2);
    EXPECT_EQ(deriveSlotType("U2"), nvi::SlotType::U2);
    EXPECT_EQ(deriveSlotType("OEM"), nvi::SlotType::OEM);
    // Unknown strings fall back to OEM via the SERIALIZE_ENUM default.
    EXPECT_EQ(deriveSlotType("ThisDoesNotExist"), nvi::SlotType::OEM);
}

TEST(NvidiaPcie, NotPresentRoundTrips)
{
    Json j = validBase()["PCIeSlots"][0];
    j["Present"] = false;
    nvi::NvidiaPcie p;
    nvi::from_json(j, p);
    EXPECT_FALSE(p.isPresent());
}

TEST(NvidiaPcie, OptionalFieldsDefaultToZero)
{
    Json j = validBase()["PCIeSlots"][0];
    j.erase("HotPluggable");
    j.erase("SegmentControllerIndex");
    j.erase("PortType");
    j.erase("PortProtocol");
    j.erase("RootPort");
    nvi::NvidiaPcie p;
    nvi::from_json(j, p);
    EXPECT_FALSE(p.hotPluggable);
    EXPECT_EQ(p.segmentControllerIndex, 0U);
    EXPECT_EQ(p.rootPort, 0U);
    EXPECT_TRUE(p.portType.empty());
    EXPECT_TRUE(p.portProtocol.empty());
}

TEST(NvidiaPcie, FromJsonBindsAllFields)
{
    // Asserts every member NvidiaPcie::from_json binds (per
    // NvidiaInfoPcie.cpp). ProcessorModuleIndex is intentionally
    // omitted: schema requires it but the struct does not carry it; the
    // module index used at publish time comes from the outer terminus.
    Json j = validBase()["PCIeSlots"][0];
    nvi::NvidiaPcie p;
    nvi::from_json(j, p);
    EXPECT_EQ(p.slotType, nvi::SlotType::M2);
    EXPECT_EQ(p.locationCode, "UPHY0:0-15");
    EXPECT_EQ(p.generation, 5U);
    EXPECT_EQ(p.lanes, 16U);
    EXPECT_EQ(p.maxLinkSpeed, 5U);
    EXPECT_EQ(p.maxLinkWidth, 16U);
    EXPECT_TRUE(p.present);
    EXPECT_TRUE(p.hotPluggable);
    EXPECT_EQ(p.segmentControllerIndex, 0U);
    EXPECT_EQ(p.portType, "Bi-directional");
    EXPECT_EQ(p.portProtocol, "CXL");
    EXPECT_EQ(p.rootPort, 0U);
}

// ============================================================================
// TPM
// ============================================================================

TEST(NvidiaTpm, FromJsonThrowsOnMissingRequiredField)
{
    // Schema enforces "required"; from_json mirrors that with j.at()
    // so any caller that bypasses schema validation (e.g. a future
    // recovery path that forgets to revalidate) fails loudly instead
    // of silently producing a half-populated object.
    for (const char* field : {"Manufacturer", "Version", "MajorSpecVersion"})
    {
        Json j = validBase()["TPM"][0];
        j.erase(field);
        nvi::NvidiaTpm t;
        EXPECT_THROW(nvi::from_json(j, t), Json::out_of_range)
            << "missing " << field << " must throw";
    }
}

TEST(NvidiaTpm, FromJsonBindsAllFields)
{
    Json j = validBase()["TPM"][0];
    nvi::NvidiaTpm t;
    nvi::from_json(j, t);
    EXPECT_EQ(t.manufacturer, "NTC");
    EXPECT_EQ(t.majorSpecVersion, "2.0");
    EXPECT_EQ(t.version, "7.2.3.0");
}

// ============================================================================
// Enum-name mappings (formFactorName/memoryTypeName/memoryMediaTechName/
// slotTypeName)
// ============================================================================

TEST(EnumNames, FormFactor)
{
    // Every enumerator in NvidiaInfoEnums.hpp:FormFactor must round-trip
    // through formFactorName(). Adding a new enumerator without updating
    // the switch in NvidiaInfoSchema.cpp will fall through to "Unknown"
    // and be caught here.
    struct Case
    {
        nvi::FormFactor in;
        const char* expected;
    };
    static constexpr std::array kCases{
        Case{nvi::FormFactor::RDIMM, "RDIMM"},
        Case{nvi::FormFactor::UDIMM, "UDIMM"},
        Case{nvi::FormFactor::SO_DIMM, "SO_DIMM"},
        Case{nvi::FormFactor::LRDIMM, "LRDIMM"},
        Case{nvi::FormFactor::Mini_RDIMM, "Mini_RDIMM"},
        Case{nvi::FormFactor::Mini_UDIMM, "Mini_UDIMM"},
        Case{nvi::FormFactor::SO_RDIMM_72b, "SO_RDIMM_72b"},
        Case{nvi::FormFactor::SO_UDIMM_72b, "SO_UDIMM_72b"},
        Case{nvi::FormFactor::SO_DIMM_16b, "SO_DIMM_16b"},
        Case{nvi::FormFactor::SO_DIMM_32b, "SO_DIMM_32b"},
        Case{nvi::FormFactor::Die, "Die"},
        Case{nvi::FormFactor::SOCAMM, "SOCAMM"},
        Case{nvi::FormFactor::Unknown, "Unknown"},
    };
    for (const auto& c : kCases)
    {
        EXPECT_STREQ(nvi::formFactorName(c.in), c.expected);
    }
}

TEST(EnumNames, MemoryType)
{
    // Every enumerator in NvidiaInfoEnums.hpp:MemoryType. Strings here
    // match the C++ identifier (with underscores), not the JSON wire
    // form (which uses dashes for some types like "LPDDR5-SDRAM"); the
    // wire form is the deserialize-side concern.
    struct Case
    {
        nvi::MemoryType in;
        const char* expected;
    };
    static constexpr std::array kCases{
        Case{nvi::MemoryType::DDR, "DDR"},
        Case{nvi::MemoryType::DDR2, "DDR2"},
        Case{nvi::MemoryType::DDR3, "DDR3"},
        Case{nvi::MemoryType::DDR4, "DDR4"},
        Case{nvi::MemoryType::DDR4E_SDRAM, "DDR4E_SDRAM"},
        Case{nvi::MemoryType::DDR5, "DDR5"},
        Case{nvi::MemoryType::LPDDR5_SDRAM, "LPDDR5_SDRAM"},
        Case{nvi::MemoryType::LPDDR4_SDRAM, "LPDDR4_SDRAM"},
        Case{nvi::MemoryType::LPDDR3_SDRAM, "LPDDR3_SDRAM"},
        Case{nvi::MemoryType::DDR2_SDRAM_FB_DIMM, "DDR2_SDRAM_FB_DIMM"},
        Case{nvi::MemoryType::DDR2_SDRAM_FB_DIMM_PROBE,
             "DDR2_SDRAM_FB_DIMM_PROBE"},
        Case{nvi::MemoryType::DDR_SGRAM, "DDR_SGRAM"},
        Case{nvi::MemoryType::ROM, "ROM"},
        Case{nvi::MemoryType::SDRAM, "SDRAM"},
        Case{nvi::MemoryType::EDO, "EDO"},
        Case{nvi::MemoryType::FastPageMode, "FastPageMode"},
        Case{nvi::MemoryType::PipelinedNibble, "PipelinedNibble"},
        Case{nvi::MemoryType::Logical, "Logical"},
        Case{nvi::MemoryType::HBM, "HBM"},
        Case{nvi::MemoryType::HBM2, "HBM2"},
        Case{nvi::MemoryType::HBM3, "HBM3"},
        Case{nvi::MemoryType::Unknown, "Unknown"},
    };
    for (const auto& c : kCases)
    {
        EXPECT_STREQ(nvi::memoryTypeName(c.in), c.expected);
    }
}

TEST(EnumNames, MemoryMediaIsIdentity)
{
    // Schema-accepted MemoryMedia values publish as themselves on D-Bus.
    EXPECT_STREQ(nvi::memoryMediaTechName(nvi::MemoryMedia::DRAM), "DRAM");
    EXPECT_STREQ(nvi::memoryMediaTechName(nvi::MemoryMedia::NAND), "NAND");
    EXPECT_STREQ(nvi::memoryMediaTechName(nvi::MemoryMedia::Intel3DXPoint),
                 "Intel3DXPoint");
    EXPECT_STREQ(nvi::memoryMediaTechName(nvi::MemoryMedia::Unknown),
                 "Unknown");
}

TEST(EnumNames, SlotTypeAlwaysUnderscored)
{
    // Even though JSON accepts both "M_2" and "M2", the DBus suffix is
    // always the underscored spelling.
    EXPECT_STREQ(nvi::slotTypeName(nvi::SlotType::M2), "M_2");
    EXPECT_STREQ(nvi::slotTypeName(nvi::SlotType::U2), "U_2");
    EXPECT_STREQ(nvi::slotTypeName(nvi::SlotType::FullLength), "FullLength");
    EXPECT_STREQ(nvi::slotTypeName(nvi::SlotType::OEM), "OEM");
    // Any out-of-range cast falls into the function's default-OEM tail.
    EXPECT_STREQ(nvi::slotTypeName(static_cast<nvi::SlotType>(999)), "OEM");
}

// ============================================================================
// Persistence helpers (persistence::*)
// ============================================================================

class PersistenceTest : public ::testing::Test
{
  protected:
    std::filesystem::path tmpdir;

    void SetUp() override
    {
        tmpdir = makeTmpDir("nvinfo-persist");
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(tmpdir, ec);
    }
};

TEST_F(PersistenceTest, FilenameRoundTrips)
{
    // Module 0..9 paired with a range of sockets, including multi-digit
    // and the 0/255 boundaries.
    static constexpr std::array kCases{
        pst::PersistedId{0, 0},  pst::PersistedId{0, 1},
        pst::PersistedId{1, 0},  pst::PersistedId{3, 7},
        pst::PersistedId{9, 9},  pst::PersistedId{0, 99},
        pst::PersistedId{2, 255}};
    for (const auto& id : kCases)
    {
        const auto p = pst::persistedPathFor(tmpdir.string(), id);
        EXPECT_EQ(p.parent_path(), tmpdir);
        const auto parsed = pst::persistedIdFromFilename(p.filename().string());
        ASSERT_TRUE(parsed.has_value()) << "no parse for " << p.filename();
        EXPECT_EQ(parsed->processorModuleIndex, id.processorModuleIndex);
        EXPECT_EQ(parsed->socket, id.socket);
    }
}

TEST_F(PersistenceTest, FilenameRejectsBadShapes)
{
    using pst::persistedIdFromFilename;
    EXPECT_FALSE(persistedIdFromFilename(""));
    EXPECT_FALSE(persistedIdFromFilename("Foo.json"));
    EXPECT_FALSE(persistedIdFromFilename(
        "ProcessorModule_0_Info.json"));              // legacy: no socket field
    EXPECT_FALSE(persistedIdFromFilename(
        "ProcessorModule_10_Socket_0_Info.json"));    // two-digit module
    EXPECT_FALSE(persistedIdFromFilename(
        "ProcessorModule_05_Socket_0_Info.json"));    // module leading zero
    EXPECT_FALSE(persistedIdFromFilename(
        "ProcessorModule_0_Socket_05_Info.json"));    // socket leading zero
    EXPECT_FALSE(persistedIdFromFilename(
        "ProcessorModule_0_Socket_256_Info.json"));   // socket > 255
    EXPECT_FALSE(persistedIdFromFilename(
        "ProcessorModule_0_Socket_-1_Info.json"));    // signed socket
    EXPECT_FALSE(persistedIdFromFilename(
        "ProcessorModule_0_Socket_a_Info.json"));     // non-digit socket
    EXPECT_FALSE(persistedIdFromFilename(
        "ProcessorModule_a_Socket_0_Info.json"));     // non-digit module
    EXPECT_FALSE(persistedIdFromFilename(
        "ProcessorModule_0_Socket__Info.json"));      // empty socket slot
    EXPECT_FALSE(persistedIdFromFilename(
        "ProcessorModule_0_Socket_0_Info.json.tmp")); // mid-write artifact
    EXPECT_FALSE(persistedIdFromFilename(
        "ProcessorModule_0_Socket_0_Info.txt"));      // wrong suffix
}

TEST_F(PersistenceTest, PersistWritesExactBytesAndOverwrites)
{
    const auto p =
        pst::persistedPathFor(tmpdir.string(), pst::PersistedId{3, 3});
    ASSERT_TRUE(
        pst::persistInfoJson(tmpdir.string(), pst::PersistedId{3, 3}, "first"));
    EXPECT_EQ(slurp(p), "first");
    // Overwriting must not throw and must replace cleanly.
    ASSERT_TRUE(pst::persistInfoJson(tmpdir.string(), pst::PersistedId{3, 3},
                                     "second"));
    EXPECT_EQ(slurp(p), "second");
}

TEST_F(PersistenceTest, PersistDistinctSocketsCoexist)
{
    // Two sockets of the same module must not clobber each other on disk.
    ASSERT_TRUE(pst::persistInfoJson(tmpdir.string(), pst::PersistedId{0, 0},
                                     "socket0"));
    ASSERT_TRUE(pst::persistInfoJson(tmpdir.string(), pst::PersistedId{0, 1},
                                     "socket1"));
    EXPECT_EQ(
        slurp(pst::persistedPathFor(tmpdir.string(), pst::PersistedId{0, 0})),
        "socket0");
    EXPECT_EQ(
        slurp(pst::persistedPathFor(tmpdir.string(), pst::PersistedId{0, 1})),
        "socket1");
}

TEST_F(PersistenceTest, PersistCreatesMissingDirectory)
{
    namespace fs = std::filesystem;
    auto sub = tmpdir / "nested" / "deep";
    ASSERT_FALSE(fs::exists(sub));
    EXPECT_TRUE(
        pst::persistInfoJson(sub.string(), pst::PersistedId{7, 7}, "{}"));
    EXPECT_TRUE(fs::exists(
        pst::persistedPathFor(sub.string(), pst::PersistedId{7, 7})));
}

TEST_F(PersistenceTest, PersistLeavesNoTempArtifact)
{
    namespace fs = std::filesystem;
    ASSERT_TRUE(
        pst::persistInfoJson(tmpdir.string(), pst::PersistedId{0, 0}, "hello"));
    int tmps = 0;
    for (const auto& e : fs::directory_iterator(tmpdir))
    {
        if (e.path().extension() == ".tmp")
        {
            ++tmps;
        }
    }
    EXPECT_EQ(tmps, 0);
}

TEST_F(PersistenceTest, RemoveOnExistingClearsFile)
{
    const auto p =
        pst::persistedPathFor(tmpdir.string(), pst::PersistedId{0, 0});
    ASSERT_TRUE(
        pst::persistInfoJson(tmpdir.string(), pst::PersistedId{0, 0}, "x"));
    ASSERT_TRUE(std::filesystem::exists(p));
    pst::removePersistedFile(tmpdir.string(), pst::PersistedId{0, 0});
    EXPECT_FALSE(std::filesystem::exists(p));
}

TEST_F(PersistenceTest, RemoveMissingIsNoThrow)
{
    EXPECT_NO_THROW(
        pst::removePersistedFile(tmpdir.string(), pst::PersistedId{8, 8}));
    EXPECT_FALSE(std::filesystem::exists(
        pst::persistedPathFor(tmpdir.string(), pst::PersistedId{8, 8})));
}

// ============================================================================
// Live D-Bus tests
//
// Skipped automatically when no system bus is reachable so the same test
// binary serves both `meson test` on a developer host and ad-hoc runs on
// a BMC. Each test isolates its filesystem state in a tmpdir and uses a
// per-PID inventory subtree so it never collides with the running
// nvidiainfo service. The constructor calls the mapper via
// async_method_call(); we never run the io_context, so those calls stay
// queued and harmless and the destructor cleans them up.
// ============================================================================

class NvidiaInfoLiveTest : public ::testing::Test
{
  protected:
    std::shared_ptr<boost::asio::io_context> io;
    std::shared_ptr<sdbusplus::asio::connection> conn;
    std::shared_ptr<sdbusplus::asio::object_server> objServer;
    std::filesystem::path tmpdir;
    std::string testInvPath;

    void SetUp() override
    {
        io = std::make_shared<boost::asio::io_context>();
        try
        {
            conn = std::make_shared<sdbusplus::asio::connection>(*io);
        }
        catch (const std::exception& e)
        {
            GTEST_SKIP() << "No D-Bus available: " << e.what();
        }
        objServer = std::make_shared<sdbusplus::asio::object_server>(conn);

        tmpdir = makeTmpDir("nvinfo-live");

        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        // Test names are alphanumeric; safe to embed in an object path.
        testInvPath =
            std::format("/xyz/openbmc_project/inventory/test_{}_{}", ::getpid(),
                        info != nullptr ? info->name() : "unknown");
    }

    void TearDown() override
    {
        // Drop NvidiaInfo (test-owned) first so it unregisters everything,
        // then the bus glue, then the tmpdir.
        objServer.reset();
        conn.reset();
        io.reset();
        if (!tmpdir.empty())
        {
            std::error_code ec;
            std::filesystem::remove_all(tmpdir, ec);
        }
    }

    // Per-test factory so tests can inject pre-existing persisted files
    // before construction (recovery cases).
    std::unique_ptr<nvi::NvidiaInfo> makeService()
    {
        return std::make_unique<nvi::NvidiaInfo>(io, conn, objServer,
                                                 testInvPath, tmpdir.string());
    }
};

TEST_F(NvidiaInfoLiveTest, RejectsIndexAboveNine)
{
    auto svc = makeService();
    EXPECT_THROW(svc->createInfoFromJsonString(10, validBase().dump()),
                 sdbusplus::exception::SdBusError);
}

TEST_F(NvidiaInfoLiveTest, RejectsNegativeIndex)
{
    auto svc = makeService();
    EXPECT_THROW(svc->createInfoFromJsonString(-1, validBase().dump()),
                 sdbusplus::exception::SdBusError);
}

TEST_F(NvidiaInfoLiveTest, RejectsMalformedJsonAndLeavesNoFile)
{
    auto svc = makeService();
    EXPECT_THROW(svc->createInfoFromJsonString(0, "this is not json"),
                 nvi::SchemaViolation);
    EXPECT_FALSE(std::filesystem::exists(
        pst::persistedPathFor(tmpdir.string(), pst::PersistedId{0, 0})));
}

TEST_F(NvidiaInfoLiveTest, RejectsSchemaInvalidAndLeavesNoFile)
{
    auto svc = makeService();
    Json bad = validBaseWithSocket(0);
    bad["Processor"][0].erase("Socket");
    EXPECT_THROW(svc->createInfoFromJsonString(0, bad.dump()),
                 nvi::SchemaViolation);
    EXPECT_FALSE(std::filesystem::exists(
        pst::persistedPathFor(tmpdir.string(), pst::PersistedId{0, 0})));
}

TEST_F(NvidiaInfoLiveTest, RejectsSocketModuleMismatch)
{
    // On a 1-socket-per-module build, socket must equal module; a socket
    // outside the module's range is rejected.
    auto svc = makeService();
    EXPECT_THROW(
        svc->createInfoFromJsonString(0, validBaseWithSocket(1).dump()),
        nvi::InvalidConfiguration);
    EXPECT_FALSE(std::filesystem::exists(
        pst::persistedPathFor(tmpdir.string(), pst::PersistedId{0, 1})));
}

TEST_F(NvidiaInfoLiveTest, RejectsMultipleProcessors)
{
    // One socket per payload: a payload with more than one Processor is
    // rejected rather than keyed off the first.
    auto svc = makeService();
    EXPECT_THROW(svc->createInfoFromJsonString(0, validBaseMulti().dump()),
                 nvi::SchemaViolation);
}

TEST_F(NvidiaInfoLiveTest, ValidPayloadPersistsExactBytes)
{
    auto svc = makeService();
    const std::string js = validBaseWithSocket(3).dump();
    ASSERT_NO_THROW(svc->createInfoFromJsonString(3, js));
    const auto p =
        pst::persistedPathFor(tmpdir.string(), pst::PersistedId{3, 3});
    ASSERT_TRUE(std::filesystem::exists(p));
    EXPECT_EQ(slurp(p), js);
}

TEST_F(NvidiaInfoLiveTest, MalformedRetryKeepsPriorFile)
{
    // A malformed payload has no parseable socket, so the daemon can't tell
    // which file it belongs to and leaves the last known-good file intact.
    auto svc = makeService();
    const std::string js = validBaseWithSocket(0).dump();
    ASSERT_NO_THROW(svc->createInfoFromJsonString(0, js));
    const auto p =
        pst::persistedPathFor(tmpdir.string(), pst::PersistedId{0, 0});
    ASSERT_TRUE(std::filesystem::exists(p));

    EXPECT_THROW(svc->createInfoFromJsonString(0, "not json"),
                 nvi::SchemaViolation);
    EXPECT_TRUE(std::filesystem::exists(p))
        << "unidentifiable malformed payload must not drop the good file";
    EXPECT_EQ(slurp(p), js);
}

TEST_F(NvidiaInfoLiveTest, RepublishIsIdempotent)
{
    auto svc = makeService();
    const std::string js = validBaseWithSocket(0).dump();
    EXPECT_NO_THROW(svc->createInfoFromJsonString(0, js));
    // Second call clears prior interfaces and re-registers; must not throw.
    EXPECT_NO_THROW(svc->createInfoFromJsonString(0, js));
    EXPECT_TRUE(std::filesystem::exists(
        pst::persistedPathFor(tmpdir.string(), pst::PersistedId{0, 0})));
}

TEST_F(NvidiaInfoLiveTest, DistinctModulesCoexist)
{
    // The overwrite bug: with (module, socket) identity the NVL72 layout
    // (module 0/socket 0, module 1/socket 1) keeps both files.
    auto svc = makeService();
    ASSERT_NO_THROW(
        svc->createInfoFromJsonString(0, validBaseWithSocket(0).dump()));
    ASSERT_NO_THROW(
        svc->createInfoFromJsonString(1, validBaseWithSocket(1).dump()));
    EXPECT_TRUE(std::filesystem::exists(
        pst::persistedPathFor(tmpdir.string(), pst::PersistedId{0, 0})));
    EXPECT_TRUE(std::filesystem::exists(
        pst::persistedPathFor(tmpdir.string(), pst::PersistedId{1, 1})));
}

TEST_F(NvidiaInfoLiveTest, RecoveryKeepsValidPersistedFile)
{
    // Pre-populate a valid persisted file before constructing NvidiaInfo;
    // loadPersistedInfoFiles in the ctor must accept it and leave the file
    // alone.
    const std::string js = validBaseWithSocket(5).dump();
    const auto p =
        pst::persistedPathFor(tmpdir.string(), pst::PersistedId{5, 5});
    {
        std::ofstream out(p);
        out << js;
    }
    ASSERT_TRUE(std::filesystem::exists(p));
    auto svc = makeService();
    EXPECT_TRUE(std::filesystem::exists(p));
}

TEST_F(NvidiaInfoLiveTest, RecoveryDropsCorruptPersistedFile)
{
    const auto p =
        pst::persistedPathFor(tmpdir.string(), pst::PersistedId{5, 5});
    {
        std::ofstream out(p);
        out << "{ corrupted";
    }
    ASSERT_TRUE(std::filesystem::exists(p));
    auto svc = makeService();
    EXPECT_FALSE(std::filesystem::exists(p));
}

TEST_F(NvidiaInfoLiveTest, RecoveryIgnoresNonMatchingFilenames)
{
    namespace fs = std::filesystem;
    // A legacy socket-less filename that the recovery parser must reject;
    // the file must be left alone (we only delete files we recognize).
    const auto stranger = tmpdir / "ProcessorModule_0_Info.json";
    {
        std::ofstream out(stranger);
        out << "garbage";
    }
    auto svc = makeService();
    EXPECT_TRUE(fs::exists(stranger))
        << "recovery must not touch unrecognized filenames";
}

TEST_F(NvidiaInfoLiveTest, AcceptsIndexZeroAndNine)
{
    // CreateInfo must accept the 0 and 9 boundaries of the 0..9 range
    // declared by createInfoFromJsonString. The "above 9" / "negative"
    // reject paths are covered separately. socket == module on a
    // 1-socket-per-module build.
    auto svc = makeService();
    for (int32_t idx : {0, 9})
    {
        const std::string js = validBaseWithSocket(idx).dump();
        ASSERT_NO_THROW(svc->createInfoFromJsonString(idx, js))
            << "index " << idx << " must be accepted";
        EXPECT_TRUE(std::filesystem::exists(
            pst::persistedPathFor(tmpdir.string(), pst::PersistedId{idx, idx})))
            << "index " << idx << " must persist";
    }
}

TEST_F(NvidiaInfoLiveTest, AcceptsMultipleInstancesPerSection)
{
    // Exercises the per-array DIMM/PCIe/TPM publish loops and their
    // rank-based index math for a single socket carrying two of each
    // component. Persisted file must be exact bytes.
    auto svc = makeService();
    const std::string js = multiComponentPayload(2).dump();
    ASSERT_NO_THROW(svc->createInfoFromJsonString(2, js));
    const auto p =
        pst::persistedPathFor(tmpdir.string(), pst::PersistedId{2, 2});
    ASSERT_TRUE(std::filesystem::exists(p));
    EXPECT_EQ(slurp(p), js);
}

TEST_F(NvidiaInfoLiveTest, RecoveryDropsSchemaInvalidFile)
{
    // Distinct from RecoveryDropsCorruptPersistedFile: that test feeds
    // unparseable JSON; this one feeds JSON that parses cleanly but
    // fails schema validation. Both paths should clean the file via
    // processAndPublish's catch -> removePersistedFile (recovery knows
    // the socket from the filename).
    Json bad = validBaseWithSocket(5);
    bad["Processor"][0].erase("Socket");
    const auto p =
        pst::persistedPathFor(tmpdir.string(), pst::PersistedId{5, 5});
    {
        std::ofstream out(p);
        out << bad.dump();
    }
    ASSERT_TRUE(std::filesystem::exists(p));
    auto svc = makeService();
    EXPECT_FALSE(std::filesystem::exists(p))
        << "recovery must drop schema-invalid persisted files";
}

TEST_F(NvidiaInfoLiveTest, RecoveryProcessesMultipleFiles)
{
    // Single-file recovery is covered by RecoveryKeepsValidPersistedFile.
    // This test pre-populates three valid files at non-contiguous
    // (module, socket) pairs and asserts the directory iterator processes
    // all of them (none rejected, all still on disk after construction).
    static constexpr std::array<int32_t, 3> kIdx{0, 3, 7};
    for (int32_t idx : kIdx)
    {
        std::ofstream out(
            pst::persistedPathFor(tmpdir.string(), pst::PersistedId{idx, idx}));
        out << validBaseWithSocket(idx).dump();
    }
    for (int32_t idx : kIdx)
    {
        ASSERT_TRUE(std::filesystem::exists(pst::persistedPathFor(
            tmpdir.string(), pst::PersistedId{idx, idx})));
    }
    auto svc = makeService();
    for (int32_t idx : kIdx)
    {
        EXPECT_TRUE(std::filesystem::exists(
            pst::persistedPathFor(tmpdir.string(), pst::PersistedId{idx, idx})))
            << "module " << idx << " must survive recovery";
    }
}
