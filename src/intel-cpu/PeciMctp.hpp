/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
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

constexpr uint8_t cmdPing = 0x00;
constexpr uint8_t cmdGetTemp = 0x01;
constexpr uint8_t cmdRdPkgConfig = 0xA1;

constexpr uint8_t ccSuccess = 0x40;

// PECI CC bits[7:6]: 01=pass, 10=retry (data still valid)
bool ccHasValidData(uint8_t cc);

struct VdmHeader
{
    uint16_t vendorId;
    uint8_t instanceReqD;
    uint8_t vendorOpcode;
} __attribute__((packed));

struct GetTempRequest
{
    VdmHeader hdr;
    uint8_t txLen;
    uint8_t rxLen;
    uint8_t cmdCode;
} __attribute__((packed));

struct GetTempResponse
{
    VdmHeader hdr;
    int16_t tempRaw;
} __attribute__((packed));

struct RdPkgConfigRequest
{
    VdmHeader hdr;
    uint8_t txLen;
    uint8_t rxLen;
    uint8_t cmdCode;
    uint8_t domainId;
    uint8_t index;
    uint16_t param;
} __attribute__((packed));

struct RdPkgConfigResponse
{
    VdmHeader hdr;
    uint8_t cc;
    std::array<uint8_t, 8> data;
} __attribute__((packed));

// TODO: Find a better command for this
struct PingRequest
{
    VdmHeader hdr;
    uint8_t txLen;
    uint8_t rxLen;
} __attribute__((packed));

struct PingResponse
{
    VdmHeader hdr;
} __attribute__((packed));

bool isIntelPeciMessage(std::span<const uint8_t> buf);
std::optional<uint8_t> getIid(std::span<const uint8_t> buf);
std::optional<bool> isRequestMessage(std::span<const uint8_t> buf);
std::optional<bool> getDatagramBit(std::span<const uint8_t> buf);
std::expected<void, std::error_code> injectIid(std::span<uint8_t> buf,
                                               uint8_t iid);

std::expected<void, std::system_error> serializePing(std::span<uint8_t> buf);
std::expected<void, std::system_error> serializeGetTemp(std::span<uint8_t> buf);
std::expected<void, std::system_error> serializeRdPkgConfig(
    std::span<uint8_t> buf, uint8_t domainId, uint8_t index, uint16_t param,
    uint8_t readLen);

std::expected<void, std::system_error> deserializePing(
    std::span<const uint8_t> resp);
std::expected<void, std::system_error> deserializeGetTemp(
    std::span<const uint8_t> resp, int16_t& tempRaw);
std::expected<void, std::system_error> deserializeRdPkgConfig(
    std::span<const uint8_t> resp, uint8_t& cc, std::span<uint8_t> data);

} // namespace peci_mctp
