/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "OcpMctpVdm.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <system_error>

namespace ocp
{
namespace accelerator_management
{

bool isOcpAcceleratorMessage(std::span<const uint8_t> buffer,
                             uint16_t pciVendorId)
{
    if (buffer.size() < messageHeaderSize)
    {
        return false;
    }

    // The PCI vendor id is stored big-endian in the first two bytes.
    uint16_t receivedPciVendorId =
        static_cast<uint16_t>((buffer[0] << 8) | buffer[1]);
    if (receivedPciVendorId != pciVendorId)
    {
        return false;
    }

    uint8_t ocpVersionAndType = buffer[instanceIdOffset + 1];
    return (ocpVersionAndType & ocpVersionBitMask) == ocpVersion &&
           (ocpVersionAndType & ocpTypeBitMask) ==
               (ocpType << ocpTypeBitOffset);
}

std::optional<uint8_t> getIid(std::span<const uint8_t> buffer)
{
    if (buffer.size() < messageHeaderSize)
    {
        return std::nullopt;
    }
    return buffer[instanceIdOffset] & instanceIdBitMask;
}

std::optional<bool> isRequestMessage(std::span<const uint8_t> buffer)
{
    if (buffer.size() < messageHeaderSize)
    {
        return std::nullopt;
    }
    return (buffer[instanceIdOffset] & requestBitMask) != 0;
}

std::optional<bool> getDatagramBit(std::span<const uint8_t> buffer)
{
    if (buffer.size() < messageHeaderSize)
    {
        return std::nullopt;
    }
    return (buffer[instanceIdOffset] & datagramBitMask) != 0;
}

std::expected<void, std::error_code> injectIid(std::span<uint8_t> buffer,
                                               uint8_t iid)
{
    if (buffer.size() < messageHeaderSize)
    {
        return std::unexpected(
            std::make_error_code(std::errc::invalid_argument));
    }

    if (iid > instanceIdBitMask)
    {
        return std::unexpected(
            std::make_error_code(std::errc::invalid_argument));
    }

    buffer[instanceIdOffset] &= ~instanceIdBitMask;
    buffer[instanceIdOffset] |= iid;
    return {};
}

} // namespace accelerator_management
} // namespace ocp
