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

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>

#include <memory>
#include <string>

/** Whether an IST run is in progress.
 *
 *  A USB clear-halt makes a CPU in IST mode reset XUSB, and nothing tells RAS
 *  that it has to reinitialize USB afterwards. The MCTP endpoints come back
 *  but the IST endpoints stay dead, so the run fails on its first transfer.
 *  There is no way to signal RAS after the fact, so during IST the clear-halt
 *  is simply the wrong recovery and must not be issued. Recovery is instead
 *  driven by the IST reset hook, which takes the CPUs through a full init.
 */
class IstRecovery
{
  public:
    virtual ~IstRecovery() = default;

    virtual bool isIstInProgress() const = 0;

    /** A service that announced a run and then stopped responding emits
     *  nothing further, so without this the suppression outlives the run it
     *  was protecting.
     */
    virtual void refreshState() = 0;
};

class DBusIstRecovery : public IstRecovery
{
  public:
    explicit DBusIstRecovery(
        const std::shared_ptr<sdbusplus::asio::connection>& bus);

    ~DBusIstRecovery() override = default;
    DBusIstRecovery(const DBusIstRecovery&) = delete;
    DBusIstRecovery(DBusIstRecovery&&) = delete;
    DBusIstRecovery& operator=(const DBusIstRecovery&) = delete;
    DBusIstRecovery& operator=(DBusIstRecovery&&) = delete;

    bool isIstInProgress() const override;
    void refreshState() override;

  private:
    void findIstService();
    void watchIstService(const std::string& service);
    void readIstInProgress();

    std::shared_ptr<sdbusplus::asio::connection> bus;

    /** Empty until the object mapper lookup succeeds. */
    std::string istService;

    /** Neither async call can be cancelled, so a reply arriving after this
     *  object is gone would otherwise write to freed members.
     */
    std::shared_ptr<void> lifetime = std::make_shared<char>();

    /** Cached so the decision point never makes a blocking D-Bus call.
     *
     *  Every path that cannot establish the state leaves this false. A
     *  clear-halt wrongly issued costs the run it interrupted; one wrongly
     *  suppressed strands a dead MCTP endpoint that has no other recovery.
     */
    bool istInProgress = false;

    std::unique_ptr<sdbusplus::bus::match_t> istStateMatch;
    std::unique_ptr<sdbusplus::bus::match_t> istOwnerMatch;
};
