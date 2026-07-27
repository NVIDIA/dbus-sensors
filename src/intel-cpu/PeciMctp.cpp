/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "PeciMctp.hpp"

#include <endian.h>

#include <bit>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <system_error>

namespace peci_mctp
{

bool isIntelPeciMessage(std::span<const uint8_t> buf)
{
    if (buf.size() < sizeof(VdmHeader))
    {
        return false;
    }

    const auto* hdr = std::bit_cast<const VdmHeader*>(buf.data());
    return be16toh(hdr->vendorId) == intelVendorId &&
           hdr->vendorOpcode == vendorOpcodePeci;
}

std::optional<uint8_t> getIid(std::span<const uint8_t> buf)
{
    if (buf.size() < sizeof(VdmHeader))
    {
        return std::nullopt;
    }

    const auto* hdr = std::bit_cast<const VdmHeader*>(buf.data());
    return hdr->instanceReqD & instanceIdBitMask;
}

std::optional<bool> isRequestMessage(std::span<const uint8_t> buf)
{
    if (buf.size() < sizeof(VdmHeader))
    {
        return std::nullopt;
    }

    const auto* hdr = std::bit_cast<const VdmHeader*>(buf.data());
    return (hdr->instanceReqD & requestBitMask) != 0;
}

std::optional<bool> getDatagramBit(std::span<const uint8_t> /*buf*/)
{
    return false;
}

std::expected<void, std::error_code> injectIid(std::span<uint8_t> buf,
                                               uint8_t iid)
{
    if (buf.size() < sizeof(VdmHeader))
    {
        return std::unexpected(
            std::make_error_code(std::errc::invalid_argument));
    }

    if (iid > instanceMax)
    {
        return std::unexpected(
            std::make_error_code(std::errc::invalid_argument));
    }

    auto* hdr = std::bit_cast<VdmHeader*>(buf.data());
    hdr->instanceReqD = (hdr->instanceReqD & ~instanceIdBitMask) | iid;
    return {};
}

} // namespace peci_mctp
