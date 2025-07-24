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

constexpr int maxRetryCount = 3;

// Structure to hold context task info
struct ContextCommInfo
{
    std::shared_ptr<NVMeMiContext> context;
    nvme_mi_ep_t nvmeEp{};
    uint8_t eid;

    ContextCommInfo(boost::asio::io_context& io, uint8_t eid);
    ~ContextCommInfo();
};

class NVMeMiManager
{
  public:
    explicit NVMeMiManager(boost::asio::io_context& io);
    ~NVMeMiManager();

    // Add a context to be managed
    bool addContext(const std::shared_ptr<NVMeMiContext>& context, int net,
                    uint8_t eid);

    void removeContext(uint8_t eid);
    void start();
    void stop();

  private:
    void communicationThread();
    void processContextCommand(ContextCommInfo& commInfo);
    ssize_t processMiCommand(FileHandle& in, FileHandle& out, uint8_t eid);
    bool scanControllers(uint8_t eid, nvme_mi_ep_t& nvmeEp);

    boost::asio::io_context& io;
    std::map<uint8_t, std::unique_ptr<ContextCommInfo>> contexts;
    std::map<uint8_t, std::vector<nvme_mi_ctrl_t>> controllersByEid;
    std::jthread commThread;
    bool running{false};
    std::mutex contextsMutex;
    int retryCount{0};

    // Static NVMe root for all contexts
    static nvme_root_t nvmeRoot;
};
