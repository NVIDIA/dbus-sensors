/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "IntelCPUMctpTempSensor.hpp"
#include "MctpRequester.hpp"
#include "PeciMctp.hpp"
#include "Thresholds.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

class IntelCPUMctpSensor :
    public std::enable_shared_from_this<IntelCPUMctpSensor>
{
  public:
    IntelCPUMctpSensor(
        const std::string& objectType,
        sdbusplus::asio::object_server& objectServer,
        std::shared_ptr<sdbusplus::asio::connection>& conn,
        boost::asio::io_context& io, const std::string& sensorName,
        std::vector<thresholds::Threshold>&& thresholds,
        const std::string& sensorConfigurationIn, int cpuId, bool show,
        double dtsOffset, std::weak_ptr<mctp::MctpRequester> requester,
        uint8_t eid, unsigned int pollMs = sensorPollMs);
    ~IntelCPUMctpSensor();

    static constexpr unsigned int sensorPollMs = 1000;

    void setupRead();

    // Re-point at a freshly-opened requester (e.g. after the MCTP socket is
    // re-instantiated on a host power cycle). The sensor object persists and
    // only ever holds a weak ref, so it never keeps the requester alive.
    void setRequester(std::weak_ptr<mctp::MctpRequester> newRequester)
    {
        requester = std::move(newRequester);
    }

    // Cancel any pending cycle and start a fresh poll. Needed after the
    // requester is re-instantiated, since a request in flight on the old
    // requester loses its completion callback when that requester is destroyed.
    void restart();

  private:
    struct DimmTemp
    {
        uint8_t domainId;
        uint8_t chanRank;
        uint8_t dimmOrder;
        std::shared_ptr<IntelCPUMctpTempSensor> sensor;
    };

    static constexpr uint8_t mbxIndexTempTarget = 16;
    static constexpr uint8_t mbxIndexDimmTemp = 14;
    static constexpr uint8_t tempTargetRefreshInterval = 8;
    static constexpr uint8_t maxChanRanks = 12;
    static constexpr uint8_t maxDimmIdx = 2;

    sdbusplus::asio::object_server& objServer;
    std::shared_ptr<sdbusplus::asio::connection> conn;
    boost::asio::steady_timer waitTimer;
    std::weak_ptr<mctp::MctpRequester> requester;
    uint8_t eid;
    int cpuId;
    std::string sensorConfiguration;
    std::string baseName;

    // Published sensors.
    std::shared_ptr<IntelCPUMctpTempSensor> cpuTempSensor;

    double tjmax{0};
    bool tempTargetsValid{false};
    uint8_t pollCycleCount{0};

    // Base poll interval; configurable via the constructor (CLI stress test).
    size_t basePollMs{sensorPollMs};
    size_t pollTime{sensorPollMs};
    bool loggedInterfaceDown{false};

    // DIMM temperature state. Discovery re-runs at the tail of every poll
    // cycle until at least one DIMM reports good, mirroring the legacy PECI
    // daemon: memory training finishes well after the CPU answers PECI, so an
    // empty sweep means "not yet", never "none fitted".
    bool dimmsDiscovered{false};
    std::vector<DimmTemp> dimms;
    // Unique (domainId, chanRank) pairs that have populated DIMMs
    std::vector<std::pair<uint8_t, uint8_t>> activeDimmKeys;

    void markAllUnavailable();
    void restartRead();

    void pollTempTargets();
    void handleTempTargetsResponse(const std::error_code& ec,
                                   std::span<const uint8_t> buffer);

    void pollGetTemp();
    void handleGetTempResponse(const std::error_code& ec,
                               std::span<const uint8_t> buffer);

    void pollDimmPhase();
    void discoverDimms(uint8_t domainId, uint8_t chanRank);
    void handleDiscoverResponse(uint8_t domainId, uint8_t chanRank,
                                const std::error_code& ec,
                                std::span<const uint8_t> buffer);
    void advanceDiscovery(uint8_t domainId, uint8_t chanRank);
    void createDimmSensors();
    void pollDimmTemps(size_t keyIdx);
    void handleDimmTempResponse(size_t keyIdx, const std::error_code& ec,
                                std::span<const uint8_t> buffer);

    std::array<uint8_t, sizeof(peci_mctp::RdPkgConfigRequest)>
        tempTargetsTxBuf{};
    std::array<uint8_t, sizeof(peci_mctp::GetTempRequest)> getTempTxBuf{};
    std::array<uint8_t, sizeof(peci_mctp::RdPkgConfigRequest)> dimmTempTxBuf{};
};
