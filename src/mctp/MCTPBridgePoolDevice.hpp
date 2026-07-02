/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "MCTPEndpoint.hpp"
#include "Utils.hpp"

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>

/**
 * @brief Bridge-pool MCTP device — downstream consumer of
 *        @c xyz.openbmc_project.Configuration.MCTPBridgePoolDevice.
 *
 * Unlike @c MCTPDDevice based consumers (I2C/I3C/USB/SPI), this consumer never
 * calls @c AssignEndpoint. mctpd's @c peer_endpoint_poll already creates the
 * pool-member endpoint behind a bridge; this consumer only supplies the missing
 * @c configured_by association so that bridge-downstream endpoints become
 * indistinguishable from direct-attach endpoints to every consumer (pldmd).
 *
 * Flow:
 *  1. Resolve @c BridgeName → the sibling bridge's EM object → its mctpd
 *     @c Endpoint1. If the bridge is not set up yet, defer.
 *  2. Read @c au.com.codeconstruct.MCTP.Bridge1.PoolStart (and the network id)
 *     on the bridge endpoint. If @c Bridge1 is not attached yet
 *     (@c endpoint_allocate_eids still in flight), defer.
 *  3. Compute @c bridgedEid = @c PoolStart + @c PoolIndex; predicted endpoint
 *     path @c /au/com/codeconstruct/mctp1/networks/<NetworkId>/endpoints/<eid>.
 *  4. If the endpoint already exists (warm boot) publish @c configured_by
 *     immediately; otherwise arm an @c InterfacesAdded watch on the predicted
 *     path and publish when mctpd creates it.
 *  5. On @c InterfacesRemoved drop @c configured_by and re-arm the watch.
 *
 * The class implements both @c MCTPDevice and @c MCTPEndpoint so it slots into
 * the existing reactor lifecycle: @c setup() resolves the bridge and, on
 * appearance of the pool-member endpoint, invokes the supplied callback with
 * itself as the @c MCTPEndpoint. @c MCTPReactor::trackEndpoint then publishes
 * @c configured_by via the existing association path — @c network() / @c eid()
 * yield exactly the predicted pool-member endpoint path.
 */
class BridgePoolMCTPDevice :
    public MCTPDevice,
    public MCTPEndpoint,
    public std::enable_shared_from_this<BridgePoolMCTPDevice>
{
  public:
    BridgePoolMCTPDevice() = delete;
    BridgePoolMCTPDevice(const BridgePoolMCTPDevice&) = delete;
    BridgePoolMCTPDevice(BridgePoolMCTPDevice&&) = delete;
    BridgePoolMCTPDevice& operator=(const BridgePoolMCTPDevice&) = delete;
    BridgePoolMCTPDevice& operator=(BridgePoolMCTPDevice&&) = delete;

    static bool match(const std::set<std::string>& interfaces);
    static std::optional<SensorBaseConfigMap> match(const SensorData& config);
    static std::shared_ptr<BridgePoolMCTPDevice> from(
        const std::shared_ptr<sdbusplus::asio::connection>& connection,
        const SensorBaseConfigMap& iface);

    BridgePoolMCTPDevice(
        const std::shared_ptr<sdbusplus::asio::connection>& connection,
        std::string name, std::string bridgeName, uint8_t poolIndex);
    ~BridgePoolMCTPDevice() override = default;

    // MCTPDevice interface
    void setup(std::function<void(const std::error_code& ec,
                                  const std::shared_ptr<MCTPEndpoint>& ep)>&&
                   added) override;
    void remove() override;
    std::string describe() const override;
    std::optional<std::string> getNameForEid(uint8_t eid) const override;
    std::size_t id() const override;

    // MCTPEndpoint interface
    int network() const override;
    uint8_t eid() const override;
    void subscribe(Event&& degraded, Event&& available,
                   Event&& removed) override;

  private:
    static constexpr const char* configType = "MCTPBridgePoolDevice";

    std::shared_ptr<sdbusplus::asio::connection> connection;
    std::string name;
    std::string bridgeName;
    uint8_t poolIndex;

    // Resolved once the bridge's Bridge1 interface is available.
    std::optional<int> networkId;
    std::optional<uint8_t> bridgedEid;
    bool resolved = false;

    std::function<void(const std::error_code& ec,
                       const std::shared_ptr<MCTPEndpoint>& ep)>
        setupCallback;
    MCTPEndpoint::Event notifyRemoved;
    std::unique_ptr<sdbusplus::bus::match_t> endpointAddedMatch;
    std::unique_ptr<sdbusplus::bus::match_t> endpointRemovedMatch;

    /**
     * @brief Resolve the bridge endpoint, read Bridge1.PoolStart and the
     *        network id, compute bridgedEid, then either publish configured_by
     *        immediately (endpoint already present) or arm the InterfacesAdded
     *        watch.
     *
     * @return false if resolution must be deferred (bridge not set up, or
     *         Bridge1 not yet attached), true once bridgedEid is computed.
     */
    bool resolveBridge();

    /** @brief Predicted pool-member endpoint path for the resolved eid. */
    std::string predictedEndpointPath() const;

    /** @brief Arm the InterfacesAdded / InterfacesRemoved watches on the
     *         predicted pool-member endpoint path. */
    void armWatches();

    /** @brief Notify the reactor the endpoint exists so configured_by gets
     *         published via the existing association path. */
    void notifyEndpointPresent();

    void onEndpointAdded(sdbusplus::message_t& msg);
    void onEndpointRemoved(sdbusplus::message_t& msg);

    // MCTPEndpoint::device() implementation
    std::shared_ptr<MCTPDevice> device() const override
    {
        return std::const_pointer_cast<BridgePoolMCTPDevice>(
            shared_from_this());
    }
};
