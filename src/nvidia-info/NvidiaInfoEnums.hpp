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

#include <nlohmann/json.hpp>

namespace nvidia
{
namespace info
{

using Json = nlohmann::json;

// FormFactor: DIMM form factors. Unknown is the first entry and is the
// fallback that NLOHMANN_JSON_SERIALIZE_ENUM decodes to when the incoming
// JSON string matches no listed value. validate() (in later commits)
// rejects Unknown. String spellings are sourced from schema.json and
// match the phosphor-dbus-interfaces
// xyz.openbmc_project.Inventory.Item.Dimm.FormFactor.* suffixes.
enum class FormFactor
{
    Unknown,
    RDIMM,
    UDIMM,
    SO_DIMM,
    LRDIMM,
    Mini_RDIMM,
    Mini_UDIMM,
    SO_RDIMM_72b,
    SO_UDIMM_72b,
    SO_DIMM_16b,
    SO_DIMM_32b,
    Die,
    SOCAMM,
};

const char* formFactorName(FormFactor f);

NLOHMANN_JSON_SERIALIZE_ENUM(
    FormFactor,
    {
        {FormFactor::Unknown, nullptr},
        {FormFactor::RDIMM, "RDIMM"},
        {FormFactor::UDIMM, "UDIMM"},
        {FormFactor::SO_DIMM, "SO_DIMM"},
        {FormFactor::LRDIMM, "LRDIMM"},
        {FormFactor::Mini_RDIMM, "Mini_RDIMM"},
        {FormFactor::Mini_UDIMM, "Mini_UDIMM"},
        {FormFactor::SO_RDIMM_72b, "SO_RDIMM_72b"},
        {FormFactor::SO_UDIMM_72b, "SO_UDIMM_72b"},
        {FormFactor::SO_DIMM_16b, "SO_DIMM_16b"},
        {FormFactor::SO_DIMM_32b, "SO_DIMM_32b"},
        {FormFactor::Die, "Die"},
        {FormFactor::SOCAMM, "SOCAMM"},
    })

// MemoryType: DIMM DeviceType values. Unknown is the fallback for unknown
// strings; validate() rejects it. Names match the
// xyz.openbmc_project.Inventory.Item.Dimm.DeviceType.* enum suffixes.
enum class MemoryType
{
    Unknown,
    DDR,
    DDR2,
    DDR3,
    DDR4,
    DDR4E_SDRAM,
    DDR5,
    LPDDR5_SDRAM,
    LPDDR4_SDRAM,
    LPDDR3_SDRAM,
    DDR2_SDRAM_FB_DIMM,
    DDR2_SDRAM_FB_DIMM_PROBE,
    DDR_SGRAM,
    ROM,
    SDRAM,
    EDO,
    FastPageMode,
    PipelinedNibble,
    Logical,
    HBM,
    HBM2,
    HBM3,
};

const char* memoryTypeName(MemoryType t);

NLOHMANN_JSON_SERIALIZE_ENUM(
    MemoryType,
    {
        {MemoryType::Unknown, nullptr},
        {MemoryType::DDR, "DDR"},
        {MemoryType::DDR2, "DDR2"},
        {MemoryType::DDR3, "DDR3"},
        {MemoryType::DDR4, "DDR4"},
        {MemoryType::DDR4E_SDRAM, "DDR4E_SDRAM"},
        {MemoryType::DDR5, "DDR5"},
        {MemoryType::LPDDR5_SDRAM, "LPDDR5_SDRAM"},
        {MemoryType::LPDDR4_SDRAM, "LPDDR4_SDRAM"},
        {MemoryType::LPDDR3_SDRAM, "LPDDR3_SDRAM"},
        {MemoryType::DDR2_SDRAM_FB_DIMM, "DDR2_SDRAM_FB_DIMM"},
        {MemoryType::DDR2_SDRAM_FB_DIMM_PROBE, "DDR2_SDRAM_FB_DIMM_PROBE"},
        {MemoryType::DDR_SGRAM, "DDR_SGRAM"},
        {MemoryType::ROM, "ROM"},
        {MemoryType::SDRAM, "SDRAM"},
        {MemoryType::EDO, "EDO"},
        {MemoryType::FastPageMode, "FastPageMode"},
        {MemoryType::PipelinedNibble, "PipelinedNibble"},
        {MemoryType::Logical, "Logical"},
        {MemoryType::HBM, "HBM"},
        {MemoryType::HBM2, "HBM2"},
        {MemoryType::HBM3, "HBM3"},
    })

// MemoryMedia: storage technology behind the DIMM. The JSON spelling is
// fixed (see schema.json); the phosphor-dbus-interfaces MemoryTech suffix
// differs for NAND (remaps to Other) and Intel3DXPoint (remaps to
// IntelOptane). memoryMediaTechName() performs that remap.
enum class MemoryMedia
{
    Unknown,
    DRAM,
    NAND,
    Intel3DXPoint,
};

const char* memoryMediaTechName(MemoryMedia m);

NLOHMANN_JSON_SERIALIZE_ENUM(MemoryMedia,
                             {
                                 {MemoryMedia::Unknown, nullptr},
                                 {MemoryMedia::DRAM, "DRAM"},
                                 {MemoryMedia::NAND, "NAND"},
                                 {MemoryMedia::Intel3DXPoint, "Intel3DXPoint"},
                             })

// SlotType: PCIe slot form factor. OEM is the first entry and is the
// fallback decoded from unrecognized strings (validate() rejects empty
// SlotType in a later commit). Enum names omit underscores; slotTypeName()
// inserts them to produce the
// xyz.openbmc_project.Inventory.Item.PCIeSlot.SlotTypes.* suffix (e.g.
// SlotType::M2 -> "M_2"). The JSON decoder accepts both the underscored
// form ("M_2", "U_2") and the compact form ("M2", "U2").
enum class SlotType
{
    OEM,
    FullLength,
    HalfLength,
    LowProfile,
    Mini,
    M2,
    OCP3Small,
    OCP3Large,
    U2,
};

const char* slotTypeName(SlotType s);

NLOHMANN_JSON_SERIALIZE_ENUM(
    SlotType,
    {
        {SlotType::OEM, "OEM"},
        {SlotType::FullLength, "FullLength"},
        {SlotType::HalfLength, "HalfLength"},
        {SlotType::LowProfile, "LowProfile"},
        {SlotType::Mini, "Mini"},
        {SlotType::M2, "M_2"},
        {SlotType::M2, "M2"},
        {SlotType::OCP3Small, "OCP3Small"},
        {SlotType::OCP3Large, "OCP3Large"},
        {SlotType::U2, "U_2"},
        {SlotType::U2, "U2"},
    })

} // namespace info
} // namespace nvidia
