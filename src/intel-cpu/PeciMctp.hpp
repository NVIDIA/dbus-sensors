/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <system_error>

namespace peci_mctp
{

constexpr uint8_t messageType = 0x7E;

constexpr uint16_t intelVendorId = 0x8086;
constexpr uint8_t vendorOpcodePeci = 0x02;

constexpr uint8_t requestBitMask = 0x80;
constexpr uint8_t datagramBitMask = 0x40;
constexpr uint8_t instanceIdBitMask = 0x1F;
constexpr uint8_t instanceMax = 31;

struct VdmHeader
{
    uint16_t vendorId;
    uint8_t instanceReqD;
    uint8_t vendorOpcode;
} __attribute__((packed));

bool isIntelPeciMessage(std::span<const uint8_t> buf);
std::optional<uint8_t> getIid(std::span<const uint8_t> buf);
std::optional<bool> isRequestMessage(std::span<const uint8_t> buf);
std::optional<bool> getDatagramBit(std::span<const uint8_t> buf);
std::expected<void, std::error_code> injectIid(std::span<uint8_t> buf,
                                               uint8_t iid);

} // namespace peci_mctp
