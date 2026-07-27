/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "PeciMctp.hpp"

#include <endian.h>

#include <bit>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <system_error>

namespace peci_mctp
{

static void fillVdmHeader(VdmHeader& hdr, uint8_t reqResp)
{
    hdr.vendorId = htobe16(intelVendorId);
    hdr.instanceReqD = reqResp & (requestBitMask | datagramBitMask);
    hdr.vendorOpcode = vendorOpcodePeci;
}

static std::unexpected<std::system_error> bufferTooSmall()
{
    return std::unexpected(
        std::system_error(std::make_error_code(std::errc::no_buffer_space)));
}

static std::unexpected<std::system_error> invalidResponse()
{
    return std::unexpected(
        std::system_error(std::make_error_code(std::errc::bad_message)));
}

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

std::expected<void, std::system_error> serializePing(std::span<uint8_t> buf)
{
    if (buf.size() < sizeof(PingRequest))
    {
        return bufferTooSmall();
    }

    auto* req = new (buf.data()) PingRequest{};
    fillVdmHeader(req->hdr, requestBitMask);
    req->txLen = 0;
    req->rxLen = 0;

    return {};
}

std::expected<void, std::system_error> serializeGetTemp(std::span<uint8_t> buf)
{
    if (buf.size() < sizeof(GetTempRequest))
    {
        return bufferTooSmall();
    }

    auto* req = new (buf.data()) GetTempRequest{};
    fillVdmHeader(req->hdr, requestBitMask);
    req->txLen = 1;
    req->rxLen = 2;
    req->cmdCode = cmdGetTemp;

    return {};
}

std::expected<void, std::system_error> serializeRdPkgConfig(
    std::span<uint8_t> buf, uint8_t domainId, uint8_t index, uint16_t param,
    uint8_t readLen)
{
    if (buf.size() < sizeof(RdPkgConfigRequest))
    {
        return bufferTooSmall();
    }

    auto* req = new (buf.data()) RdPkgConfigRequest{};
    fillVdmHeader(req->hdr, requestBitMask);
    req->txLen = 5;
    req->rxLen = static_cast<uint8_t>(1 + readLen); // CC + data
    req->cmdCode = cmdRdPkgConfig;
    req->domainId = static_cast<uint8_t>(domainId << 1);
    req->index = index;
    req->param = htole16(param);

    return {};
}

static std::expected<void, std::system_error> validateVdmHeader(
    std::span<const uint8_t> resp)
{
    if (resp.size() < sizeof(VdmHeader))
    {
        return bufferTooSmall();
    }

    const auto* hdr = std::bit_cast<const VdmHeader*>(resp.data());

    if (be16toh(hdr->vendorId) != intelVendorId)
    {
        return invalidResponse();
    }
    if ((hdr->instanceReqD & requestBitMask) != 0)
    {
        return invalidResponse();
    }
    if (hdr->vendorOpcode != vendorOpcodePeci)
    {
        return invalidResponse();
    }

    return {};
}

std::expected<void, std::system_error> deserializePing(
    std::span<const uint8_t> resp)
{
    return validateVdmHeader(resp);
}

std::expected<void, std::system_error> deserializeGetTemp(
    std::span<const uint8_t> resp, int16_t& tempRaw)
{
    if (resp.size() < sizeof(GetTempResponse))
    {
        return bufferTooSmall();
    }

    auto rc = validateVdmHeader(resp);
    if (!rc)
    {
        return rc;
    }

    const auto* r = std::bit_cast<const GetTempResponse*>(resp.data());
    tempRaw = static_cast<int16_t>(le16toh(static_cast<uint16_t>(r->tempRaw)));

    return {};
}

std::expected<void, std::system_error> deserializeRdPkgConfig(
    std::span<const uint8_t> resp, uint8_t& cc, std::span<uint8_t> data)
{
    if (resp.size() < sizeof(VdmHeader) + 1) // at least VDM + CC
    {
        return bufferTooSmall();
    }

    auto rc = validateVdmHeader(resp);
    if (!rc)
    {
        return rc;
    }

    auto payload = resp.subspan(sizeof(VdmHeader));
    cc = payload[0];

    // A failing completion code carries no data bytes; the caller decides what
    // to do with cc.
    if (!ccHasValidData(cc))
    {
        return {};
    }

    // On success every requested byte must be present, otherwise the caller
    // reads its zero-initialized buffer as if it came from the CPU.
    auto readData = payload.subspan(1);
    if (readData.size() < data.size())
    {
        return bufferTooSmall();
    }
    std::memcpy(data.data(), readData.data(), data.size());

    return {};
}

bool ccHasValidData(uint8_t cc)
{
    return cc == ccSuccess;
}

} // namespace peci_mctp
