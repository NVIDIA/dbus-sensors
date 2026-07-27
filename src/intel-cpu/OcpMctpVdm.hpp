/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <system_error>

namespace ocp
{
namespace accelerator_management
{

constexpr uint8_t ocpType = 8;
constexpr uint8_t ocpVersion = 9;
constexpr uint8_t ocpTypeBitOffset = 4;
constexpr uint8_t ocpTypeBitMask = 0b11110000;
constexpr uint8_t ocpVersionBitMask = 0b00001111;
constexpr uint8_t instanceIdBitMask = 0b00011111;
constexpr uint8_t datagramBitMask = 0b01000000;
constexpr uint8_t requestBitMask = 0b10000000;

constexpr size_t messageHeaderSize = 5;
constexpr size_t instanceIdOffset = 2;

// Minimal OCP MCTP VDM header parsing. The requester is shared with the
// nvidia-gpu accelerator protocol, so it only needs to identify such messages
// and read/patch the instance id and request/datagram bits for routing.
bool isOcpAcceleratorMessage(std::span<const uint8_t> buffer,
                             uint16_t pciVendorId);

std::optional<uint8_t> getIid(std::span<const uint8_t> buffer);
std::optional<bool> isRequestMessage(std::span<const uint8_t> buffer);
std::optional<bool> getDatagramBit(std::span<const uint8_t> buffer);
std::expected<void, std::error_code> injectIid(std::span<uint8_t> buffer,
                                               uint8_t iid);

} // namespace accelerator_management
} // namespace ocp
