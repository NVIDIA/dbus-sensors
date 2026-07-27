/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "IntelCPUMctpSensor.hpp"

#include "IntelCPUMctpTempSensor.hpp"
#include "MctpRequester.hpp"
#include "PeciMctp.hpp"
#include "Thresholds.hpp"
#include "Utils.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <phosphor-logging/lg2.hpp>
#include <phosphor-logging/lg2/flags.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

IntelCPUMctpSensor::IntelCPUMctpSensor(
    const std::string& /*objectType*/,
    sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& connIn,
    boost::asio::io_context& io, const std::string& sensorName,
    std::vector<thresholds::Threshold>&& thresholdsIn,
    const std::string& sensorConfigurationIn, int cpuId, bool /*show*/,
    double /*dtsOffset*/, std::weak_ptr<mctp::MctpRequester> requesterIn,
    uint8_t eid, unsigned int pollMs) :
    objServer(objectServer), conn(connIn), waitTimer(io),
    requester(std::move(requesterIn)), eid(eid), cpuId(cpuId),
    sensorConfiguration(sensorConfigurationIn), basePollMs(pollMs),
    pollTime(pollMs)
{
    baseName = escapeName(sensorName);
    if (baseName.ends_with("_Temp"))
    {
        baseName.resize(baseName.size() - 5);
    }

    // The CPU package temperature carries the configured thresholds.
    cpuTempSensor = std::make_shared<IntelCPUMctpTempSensor>(
        objServer, conn, baseName + "_Temp_0", sensorConfiguration,
        std::move(thresholdsIn));

    setupPowerMatch(conn);
}

IntelCPUMctpSensor::~IntelCPUMctpSensor()
{
    waitTimer.cancel();
    // cpuTempSensor and the per-DIMM sensors remove their own D-Bus interfaces
    // on destruction.
}

void IntelCPUMctpSensor::markAllUnavailable()
{
    cpuTempSensor->updateReading(std::numeric_limits<double>::quiet_NaN());
    for (auto& dimm : dimms)
    {
        if (dimm.sensor)
        {
            dimm.sensor->updateReading(
                std::numeric_limits<double>::quiet_NaN());
        }
    }
}

void IntelCPUMctpSensor::restart()
{
    // Cancelling aborts any pending wait (its handler early-returns on
    // operation_aborted), so we don't end up with two concurrent poll chains.
    waitTimer.cancel();
    setupRead();
}

void IntelCPUMctpSensor::restartRead()
{
    std::weak_ptr<IntelCPUMctpSensor> weakRef = weak_from_this();
    waitTimer.expires_after(std::chrono::milliseconds(pollTime));
    waitTimer.async_wait([weakRef](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            return;
        }
        if (auto self = weakRef.lock())
        {
            self->setupRead();
        }
    });
}

void IntelCPUMctpSensor::setupRead()
{
    if (!readingStateGood(PowerState::on))
    {
        markAllUnavailable();
        restartRead();
        return;
    }

    if (!tempTargetsValid || pollCycleCount++ >= tempTargetRefreshInterval)
    {
        pollCycleCount = 0;
        pollTempTargets();
        return;
    }

    pollGetTemp();
}

void IntelCPUMctpSensor::pollTempTargets()
{
    auto rc = peci_mctp::serializeRdPkgConfig(tempTargetsTxBuf, 0,
                                              mbxIndexTempTarget, 0, 4);
    if (!rc)
    {
        lg2::error("Failed to serialize RdPkgConfig for '{NAME}'", "NAME",
                   baseName);
        restartRead();
        return;
    }

    auto req = requester.lock();
    if (!req)
    {
        restartRead();
        return;
    }
    std::weak_ptr<IntelCPUMctpSensor> weakRef = weak_from_this();
    req->sendRecvMsg(
        eid,
        std::span<const uint8_t>(tempTargetsTxBuf.data(),
                                 sizeof(peci_mctp::RdPkgConfigRequest)),
        [weakRef](const std::error_code& ec, std::span<const uint8_t> buffer) {
            if (auto self = weakRef.lock())
            {
                self->handleTempTargetsResponse(ec, buffer);
            }
        });
}

void IntelCPUMctpSensor::handleTempTargetsResponse(
    const std::error_code& ec, std::span<const uint8_t> buffer)
{
    if (ec)
    {
        lg2::error("'{NAME}' temp targets query failed: {ERR}", "NAME",
                   baseName, "ERR", ec.message());
        cpuTempSensor->updateReading(std::numeric_limits<double>::quiet_NaN());
        restartRead();
        return;
    }

    uint8_t cc = 0;
    std::array<uint8_t, 4> pkgCfg{};
    auto result = peci_mctp::deserializeRdPkgConfig(buffer, cc, pkgCfg);
    if (!result || cc != peci_mctp::ccSuccess)
    {
        lg2::error("'{NAME}' temp targets RdPkgConfig failed, cc=0x{CC}",
                   "NAME", baseName, "CC", lg2::hex, cc);
        cpuTempSensor->updateReading(std::numeric_limits<double>::quiet_NaN());
        restartRead();
        return;
    }

    if (pkgCfg[2] == 0)
    {
        // Tjmax byte is zero — BIOS hasn't configured temp targets yet.
        lg2::debug("'{NAME}' Tjmax not yet available", "NAME", baseName);
        restartRead();
        return;
    }

    tjmax = static_cast<double>(pkgCfg[2]);
    tempTargetsValid = true;

    pollGetTemp();
}

void IntelCPUMctpSensor::pollGetTemp()
{
    auto rc = peci_mctp::serializeGetTemp(getTempTxBuf);
    if (!rc)
    {
        lg2::error("Failed to serialize GetTemp for '{NAME}'", "NAME",
                   baseName);
        restartRead();
        return;
    }

    auto req = requester.lock();
    if (!req)
    {
        restartRead();
        return;
    }
    std::weak_ptr<IntelCPUMctpSensor> weakRef = weak_from_this();
    req->sendRecvMsg(
        eid,
        std::span<const uint8_t>(getTempTxBuf.data(),
                                 sizeof(peci_mctp::GetTempRequest)),
        [weakRef](const std::error_code& ec, std::span<const uint8_t> buffer) {
            if (auto self = weakRef.lock())
            {
                self->handleGetTempResponse(ec, buffer);
            }
        });
}

void IntelCPUMctpSensor::handleGetTempResponse(const std::error_code& ec,
                                               std::span<const uint8_t> buffer)
{
    if (ec)
    {
        if (ec == std::errc::timed_out)
        {
            if (!loggedInterfaceDown)
            {
                lg2::error("'{NAME}' MCTP PECI timeout", "NAME", baseName);
                loggedInterfaceDown = true;
            }
            pollTime = basePollMs * 10U;
        }
        else
        {
            lg2::error("'{NAME}' MCTP PECI error: {ERR}", "NAME", baseName,
                       "ERR", ec.message());
        }
        cpuTempSensor->updateReading(std::numeric_limits<double>::quiet_NaN());
        restartRead();
        return;
    }

    loggedInterfaceDown = false;
    pollTime = basePollMs;

    int16_t tempRaw = 0;
    auto result = peci_mctp::deserializeGetTemp(buffer, tempRaw);
    if (!result)
    {
        lg2::error("'{NAME}' failed to deserialize GetTemp response", "NAME",
                   baseName);
        cpuTempSensor->updateReading(std::numeric_limits<double>::quiet_NaN());
        restartRead();
        return;
    }

    double tempC = tjmax + (static_cast<double>(tempRaw) / 64.0);
    cpuTempSensor->updateReading(tempC);

    // CPU temperature is in hand; sweep the DIMMs and schedule the next poll
    // cycle.
    pollDimmPhase();
}

// Tail of every poll cycle: sweep for DIMMs until one reports good, then just
// read the ones we found.
void IntelCPUMctpSensor::pollDimmPhase()
{
    if (!dimmsDiscovered)
    {
        dimms.clear();
        activeDimmKeys.clear();
        discoverDimms(0, 0);
        return;
    }

    pollDimmTemps(0);
}

void IntelCPUMctpSensor::advanceDiscovery(uint8_t domainId, uint8_t chanRank)
{
    if (chanRank + 1 < maxChanRanks)
    {
        discoverDimms(domainId, chanRank + 1);
        return;
    }

    createDimmSensors();
}

void IntelCPUMctpSensor::discoverDimms(uint8_t domainId, uint8_t chanRank)
{
    auto rc = peci_mctp::serializeRdPkgConfig(dimmTempTxBuf, domainId,
                                              mbxIndexDimmTemp, chanRank, 4);
    if (!rc)
    {
        advanceDiscovery(domainId, chanRank);
        return;
    }

    auto req = requester.lock();
    if (!req)
    {
        restartRead();
        return;
    }
    std::weak_ptr<IntelCPUMctpSensor> weakRef = weak_from_this();
    req->sendRecvMsg(
        eid,
        std::span<const uint8_t>(dimmTempTxBuf.data(),
                                 sizeof(peci_mctp::RdPkgConfigRequest)),
        [weakRef, domainId,
         chanRank](const std::error_code& ec, std::span<const uint8_t> buffer) {
            if (auto self = weakRef.lock())
            {
                self->handleDiscoverResponse(domainId, chanRank, ec, buffer);
            }
        });
}

void IntelCPUMctpSensor::handleDiscoverResponse(
    uint8_t domainId, uint8_t chanRank, const std::error_code& ec,
    std::span<const uint8_t> buffer)
{
    if (ec)
    {
        advanceDiscovery(domainId, chanRank);
        return;
    }

    uint8_t cc = 0;
    std::array<uint8_t, 4> data{};
    auto result = peci_mctp::deserializeRdPkgConfig(buffer, cc, data);
    if (!result || !peci_mctp::ccHasValidData(cc))
    {
        advanceDiscovery(domainId, chanRank);
        return;
    }

    bool hasActiveDimm = false;
    for (uint8_t idx = 0; idx < maxDimmIdx; idx++)
    {
        // Per Intel IntelCPUSensorMain.cpp: both 0 and 0xFF indicate
        // "DIMM not present" depending on generation.
        if (data[idx] != 0 && data[idx] != 0xFF)
        {
            dimms.push_back({domainId, chanRank, idx, nullptr});
            hasActiveDimm = true;
        }
    }
    if (hasActiveDimm)
    {
        activeDimmKeys.emplace_back(domainId, chanRank);
    }

    advanceDiscovery(domainId, chanRank);
}

void IntelCPUMctpSensor::createDimmSensors()
{
    // An empty sweep means memory training isn't done yet, not that the board
    // has no DIMMs: every channel reads back 0 until the host trains them. The
    // legacy PECI daemon keeps rescanning until a DIMM reports good, so do the
    // same — leave dimmsDiscovered false and try again next cycle. The package
    // temperature keeps publishing meanwhile.
    if (dimms.empty())
    {
        lg2::debug("'{NAME}' no DIMMs reported yet, retrying next cycle",
                   "NAME", baseName);
        restartRead();
        return;
    }

    dimmsDiscovered = true;

    for (auto& dimm : dimms)
    {
        std::string dimmName = "CPU" + std::to_string(cpuId) + "_D" +
                               std::to_string(dimm.domainId) + "_CH" +
                               std::to_string(dimm.chanRank) + "_DIMM" +
                               std::to_string(dimm.dimmOrder) + "_Temp";
        dimm.sensor = std::make_shared<IntelCPUMctpTempSensor>(
            objServer, conn, dimmName, sensorConfiguration,
            std::vector<thresholds::Threshold>{});
        lg2::info("Discovered DIMM sensor '{NAME}'", "NAME", dimmName);
    }

    lg2::info("DIMM discovery complete: {COUNT} DIMMs across {KEYS} keys",
              "COUNT", dimms.size(), "KEYS", activeDimmKeys.size());

    restartRead();
}

void IntelCPUMctpSensor::pollDimmTemps(size_t keyIdx)
{
    if (keyIdx >= activeDimmKeys.size())
    {
        restartRead();
        return;
    }

    auto [domainId, chanRank] = activeDimmKeys[keyIdx];
    auto rc = peci_mctp::serializeRdPkgConfig(dimmTempTxBuf, domainId,
                                              mbxIndexDimmTemp, chanRank, 4);
    if (!rc)
    {
        pollDimmTemps(keyIdx + 1);
        return;
    }

    auto req = requester.lock();
    if (!req)
    {
        restartRead();
        return;
    }
    std::weak_ptr<IntelCPUMctpSensor> weakRef = weak_from_this();
    req->sendRecvMsg(
        eid,
        std::span<const uint8_t>(dimmTempTxBuf.data(),
                                 sizeof(peci_mctp::RdPkgConfigRequest)),
        [weakRef,
         keyIdx](const std::error_code& ec, std::span<const uint8_t> buffer) {
            if (auto self = weakRef.lock())
            {
                self->handleDimmTempResponse(keyIdx, ec, buffer);
            }
        });
}

void IntelCPUMctpSensor::handleDimmTempResponse(
    size_t keyIdx, const std::error_code& ec, std::span<const uint8_t> buffer)
{
    if (ec)
    {
        pollDimmTemps(keyIdx + 1);
        return;
    }

    uint8_t cc = 0;
    std::array<uint8_t, 4> data{};
    auto result = peci_mctp::deserializeRdPkgConfig(buffer, cc, data);
    if (!result || !peci_mctp::ccHasValidData(cc))
    {
        pollDimmTemps(keyIdx + 1);
        return;
    }

    auto [domainId, chanRank] = activeDimmKeys[keyIdx];
    for (auto& dimm : dimms)
    {
        if (dimm.domainId == domainId && dimm.chanRank == chanRank &&
            dimm.sensor)
        {
            dimm.sensor->updateReading(
                static_cast<double>(data[dimm.dimmOrder]));
        }
    }

    pollDimmTemps(keyIdx + 1);
}
