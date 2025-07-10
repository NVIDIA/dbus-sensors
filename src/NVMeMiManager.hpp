/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
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

#include "MiContext.hpp"
#include "NVMeMiContext.hpp"

#include <libnvme-mi.h>

#include <FileHandle.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Structure to hold context task info
struct ContextCommInfo
{
    std::shared_ptr<NVMeMiContext> context;
    nvme_mi_ep_t nvmeEp;
    uint8_t eid;
    std::string sensorName;

    ContextCommInfo(boost::asio::io_context& io, uint8_t eid,
                    const std::string& sensorName);
    ~ContextCommInfo();
};

class NVMeMiManager
{
  public:
    NVMeMiManager(boost::asio::io_context& io);
    ~NVMeMiManager();

    // Add a context to be managed
    void addContext(std::shared_ptr<NVMeMiContext> context, uint8_t eid,
                    const std::string& sensorName,
                    const std::vector<uint8_t>& address);

    void removeContext(const std::string& sensorName);
    void start();
    void stop();

  private:
    void communicationThread();
    void processContextCommand(ContextCommInfo& commInfo);
    ssize_t processMiCommand(nvme_mi_ep_t& nvmeEp, FileHandle& in,
                             FileHandle& out, uint8_t eid);
    bool scanControllersForEid(uint8_t eid, nvme_mi_ep_t& nvmeEp);

    boost::asio::io_context& io;
    std::map<std::string, std::unique_ptr<ContextCommInfo>> contexts;
    std::map<uint8_t, std::vector<nvme_mi_ctrl_t>> controllersByEid;
    std::jthread commThread;
    bool running{false};
    std::mutex contextsMutex;

    // Static NVMe root for all contexts
    static nvme_root_t nvmeRoot;
};
