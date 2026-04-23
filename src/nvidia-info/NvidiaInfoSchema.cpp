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

#include "NvidiaInfoSchema.hpp"

#include "NvidiaInfoEnums.hpp"

#include <cstddef>
#include <format>
#include <stdexcept>
#include <vector>

namespace nvidia
{
namespace info
{

const char* formFactorName(FormFactor f)
{
    switch (f)
    {
        case FormFactor::RDIMM:        return "RDIMM";
        case FormFactor::UDIMM:        return "UDIMM";
        case FormFactor::SO_DIMM:      return "SO_DIMM";
        case FormFactor::LRDIMM:       return "LRDIMM";
        case FormFactor::Mini_RDIMM:   return "Mini_RDIMM";
        case FormFactor::Mini_UDIMM:   return "Mini_UDIMM";
        case FormFactor::SO_RDIMM_72b: return "SO_RDIMM_72b";
        case FormFactor::SO_UDIMM_72b: return "SO_UDIMM_72b";
        case FormFactor::SO_DIMM_16b:  return "SO_DIMM_16b";
        case FormFactor::SO_DIMM_32b:  return "SO_DIMM_32b";
        case FormFactor::Die:          return "Die";
        case FormFactor::SOCAMM:       return "SOCAMM";
        case FormFactor::Unknown:      break;
    }
    return "Unknown";
}

const char* memoryTypeName(MemoryType t)
{
    switch (t)
    {
        case MemoryType::DDR:                      return "DDR";
        case MemoryType::DDR2:                     return "DDR2";
        case MemoryType::DDR3:                     return "DDR3";
        case MemoryType::DDR4:                     return "DDR4";
        case MemoryType::DDR4E_SDRAM:              return "DDR4E_SDRAM";
        case MemoryType::DDR5:                     return "DDR5";
        case MemoryType::LPDDR5_SDRAM:             return "LPDDR5_SDRAM";
        case MemoryType::LPDDR4_SDRAM:             return "LPDDR4_SDRAM";
        case MemoryType::LPDDR3_SDRAM:             return "LPDDR3_SDRAM";
        case MemoryType::DDR2_SDRAM_FB_DIMM:       return "DDR2_SDRAM_FB_DIMM";
        case MemoryType::DDR2_SDRAM_FB_DIMM_PROBE:
            return "DDR2_SDRAM_FB_DIMM_PROBE";
        case MemoryType::DDR_SGRAM:                return "DDR_SGRAM";
        case MemoryType::ROM:                      return "ROM";
        case MemoryType::SDRAM:                    return "SDRAM";
        case MemoryType::EDO:                      return "EDO";
        case MemoryType::FastPageMode:             return "FastPageMode";
        case MemoryType::PipelinedNibble:          return "PipelinedNibble";
        case MemoryType::Logical:                  return "Logical";
        case MemoryType::HBM:                      return "HBM";
        case MemoryType::HBM2:                     return "HBM2";
        case MemoryType::HBM3:                     return "HBM3";
        case MemoryType::Unknown:                  break;
    }
    return "Unknown";
}

// NAND has no dedicated MemoryTech value in phosphor-dbus-interfaces and is
// surfaced as "Other"; Intel3DXPoint surfaces as "IntelOptane". This matches
// the original NvidiaInfoDimm.cpp behaviour.
const char* memoryMediaTechName(MemoryMedia m)
{
    switch (m)
    {
        case MemoryMedia::DRAM:          return "DRAM";
        case MemoryMedia::NAND:          return "Other";
        case MemoryMedia::Intel3DXPoint: return "IntelOptane";
        case MemoryMedia::Unknown:       break;
    }
    return "Unknown";
}

// Enum identifiers in C++ cannot contain an embedded underscore in the
// middle of a digit pair like "M_2", so M2/U2 carry underscoreless names
// and this helper restores the phosphor-dbus-interfaces spelling.
const char* slotTypeName(SlotType s)
{
    switch (s)
    {
        case SlotType::OEM:        return "OEM";
        case SlotType::FullLength: return "FullLength";
        case SlotType::HalfLength: return "HalfLength";
        case SlotType::LowProfile: return "LowProfile";
        case SlotType::Mini:       return "Mini";
        case SlotType::M2:         return "M_2";
        case SlotType::OCP3Small:  return "OCP3Small";
        case SlotType::OCP3Large:  return "OCP3Large";
        case SlotType::U2:         return "U_2";
    }
    return "OEM";
}

void from_json(const Json& /*j*/, TerminusData& /*t*/)
{
    // Later commits populate sections here, e.g.:
    //   j.at("Processor").get_to(t.cpus);
    //   j.at("Memory").get_to(t.dimms);
}

namespace
{

// Used by later commits to validate each element of a per-section vector,
// tagging any std::invalid_argument with "<Section>[<index>]: <message>".
template <typename T>
[[maybe_unused]] void validateEach(std::vector<T>& v, const char* section)
{
    for (std::size_t i = 0; i < v.size(); ++i)
    {
        try
        {
            v[i].validate();
        }
        catch (const std::exception& e)
        {
            throw std::invalid_argument(
                std::format("{}[{}]: {}", section, i, e.what()));
        }
    }
}

} // namespace

void validate(TerminusData& /*t*/)
{
    // Later commits will call validateEach(...) per section.
}

} // namespace info
} // namespace nvidia
