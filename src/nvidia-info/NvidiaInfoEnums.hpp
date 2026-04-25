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

// DIMM form factors. Unknown is the NLOHMANN_JSON_SERIALIZE_ENUM fallback;
// the schema's enum allow-list rejects it.
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

// DIMM DeviceType values; Unknown is the schema-rejected fallback.
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

// Storage technology behind the DIMM. memoryMediaTechName() remaps NAND
// to "Other" and Intel3DXPoint to "IntelOptane" for the DBus MemoryTech
// suffix.
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

// PCIe slot form factor. OEM is the fallback for unrecognized strings.
// JSON accepts both "M_2"/"U_2" and "M2"/"U2"; slotTypeName() always
// emits the underscored DBus suffix.
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
