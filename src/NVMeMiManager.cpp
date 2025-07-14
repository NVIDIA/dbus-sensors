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
#include "NVMeMiManager.hpp"

#include <poll.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>

// Initialize static member
nvme_root_t NVMeMiManager::nvmeRoot = nullptr;

ContextCommInfo::ContextCommInfo(boost::asio::io_context& io, uint8_t eid) :
    eid(eid)
{
    (void)io; // Suppress unused parameter warning
}

ContextCommInfo::~ContextCommInfo()
{
    if (nvmeEp)
    {
        nvme_mi_close(nvmeEp);
    }
}

NVMeMiManager::NVMeMiManager(boost::asio::io_context& io) : io(io)
{
    // Initialize the static NVMe root
    if (!nvmeRoot)
    {
        nvmeRoot = nvme_mi_create_root(stderr, DEFAULT_LOGLEVEL);
    }
}

NVMeMiManager::~NVMeMiManager()
{
    stop();
}

void NVMeMiManager::addContext(std::shared_ptr<NVMeMiContext> context, int net,
                               uint8_t eid)
{
    auto commInfo = std::make_unique<ContextCommInfo>(io, eid);
    commInfo->context = context;
    auto& ep = commInfo->nvmeEp;

    ep = nvme_mi_open_mctp(nvmeRoot, net, eid);
    if (!ep)
    {
        std::cerr << "Failed to create MCTP endpoint for eid: "
                  << static_cast<int>(eid) << std::endl;
        return;
    }

    if (!scanControllers(eid, ep))
    {
        lg2::error("Failed to scan controllers for eid: {EID}", "EID", eid);
        nvme_mi_close(ep);
        ep = nullptr;
        return;
    }
    // Create pipes for communication
    std::array<int, 2> requestPipeFds{};
    std::array<int, 2> responsePipeFds{};

    // Create request pipe
    if (::pipe(requestPipeFds.data()) == -1)
    {
        throw std::runtime_error("Failed to create request pipe");
    }

    // Create response pipe
    if (::pipe(responsePipeFds.data()) == -1)
    {
        ::close(requestPipeFds[0]);
        ::close(requestPipeFds[1]);
        throw std::runtime_error("Failed to create response pipe");
    }

    // Set up pipes in the context
    context->setupPipes(
        FileHandle(requestPipeFds[0]), FileHandle(responsePipeFds[1]),
        boost::asio::posix::stream_descriptor(io, requestPipeFds[1]),
        boost::asio::posix::stream_descriptor(io, responsePipeFds[0]));

    contexts[eid] = std::move(commInfo);

    lg2::info("Added context for eid: {EID}", "EID", static_cast<int>(eid));
}

void NVMeMiManager::removeContext(uint8_t eid)
{
    std::lock_guard<std::mutex> lock(contextsMutex);
    contexts.erase(eid);
}

void NVMeMiManager::start()
{
    if (running)
    {
        return;
    }

    running = true;
    commThread = std::jthread([this]() { communicationThread(); });
    std::cout << "NVMe-Mi manager started" << std::endl;
}

void NVMeMiManager::stop()
{
    if (!running)
    {
        return;
    }

    running = false;

    if (commThread.joinable())
    {
        commThread.join();
    }

    std::cout << "NVMe-Mi manager stopped" << std::endl;
}

void NVMeMiManager::communicationThread()
{
    std::cout << "NVMe-Mi thread started" << std::endl;

    while (running)
    {
        std::lock_guard<std::mutex> lock(contextsMutex);

        // Process each context
        for (auto& [eid, commInfo] : contexts)
        {
            if (commInfo->nvmeEp)
            {
                processContextCommand(*commInfo);
            }
        }

        // Small delay to prevent busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "NVMe-Mi thread stopped" << std::endl;
}

void NVMeMiManager::processContextCommand(ContextCommInfo& commInfo)
{
    // Check if there's data available on the request pipe
    struct pollfd pfd;
    pfd.fd = commInfo.context->getRequestPipe().handle();
    pfd.events = POLLIN;
    pfd.revents = 0;

    int pollResult = poll(&pfd, 1, 0); // Non-blocking poll

    if (pollResult > 0 && (pfd.revents & POLLIN))
    {
        // Process the command
        ssize_t rc = processMiCommand(commInfo.context->getRequestPipe(),
                                      commInfo.context->getResponsePipe(),
                                      commInfo.eid);
        if (rc < 0)
        {
            std::cerr << "Error processing command for eid: "
                      << static_cast<int>(commInfo.eid) << " error: " << rc
                      << std::endl;
        }
    }
}

ssize_t NVMeMiManager::processMiCommand(FileHandle& in, FileHandle& out,
                                        uint8_t eid)
{
    std::vector<uint8_t> resp{};
    ssize_t rc = 0;

    // Get controllers for this EID from the map
    auto& ctrlList = controllersByEid[eid];
    if (ctrlList.empty())
    {
        std::cerr << "No controllers found for eid: " << static_cast<int>(eid)
                  << std::endl;
        return -ENODEV;
    }

    std::array<uint8_t, sizeof(uint32_t)> req{};

    // Read the command parameters
    ssize_t readResult = ::read(in.handle(), req.data(), req.size());
    if (readResult != static_cast<ssize_t>(req.size()))
    {
        if (readResult != 0)
        {
            return -errno;
        }
        return -EIO;
    }

    nvme_mi_ctrl_t ctrl = ctrlList[0];
    int cmd = req[0];
    int nsid = req[1];

    if (cmd == NVME_LOG_LID_SMART)
    {
        resp.resize(sizeof(nvme_smart_log));
        nvme_smart_log* log =
            static_cast<nvme_smart_log*>(static_cast<void*>(resp.data()));

        constexpr int readLen = sizeof(nvme_smart_log) - sizeof(log->rsvd232);
        rc = nvme_mi_admin_get_nsid_log(ctrl, true, NVME_LOG_LID_SMART, nsid,
                                        readLen, log);
        if (rc != 0)
        {
            return -rc;
        }
    }
    else
    {
        lg2::error("Invalid command: {CMD}", "CMD", cmd);
        return -EINVAL;
    }

    // Write out the response length
    uint32_t respLen = htole32(static_cast<uint32_t>(resp.size()));
    ssize_t writeResult = ::write(out.handle(), &respLen, sizeof(respLen));
    if (writeResult != static_cast<ssize_t>(sizeof(respLen)))
    {
        return -errno;
    }

    // Write out the response data
    writeResult = ::write(out.handle(), resp.data(), resp.size());
    if (writeResult != static_cast<ssize_t>(resp.size()))
    {
        return -errno;
    }

    return 0;
}

bool NVMeMiManager::scanControllers(uint8_t eid, nvme_mi_ep_t& nvmeEp)
{
    lg2::debug("Scanning MI controllers for eid: {EID}", "EID", eid);
    // Scan for controllers with retry logic
    int rc = 0;
    const int maxRetries = 3;
    for (int attempt = 1; attempt <= maxRetries; ++attempt)
    {
        rc = nvme_mi_scan_ep(nvmeEp, true);
        if (rc == 0)
        {
            break; // Success, exit retry loop
        }

        if (attempt < maxRetries)
        {
            lg2::warning(
                "Scan attempt {ATTEMPT} failed for eid {EID}, retrying...",
                "ATTEMPT", attempt, "EID", eid);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    if (rc != 0)
    {
        lg2::error(
            "Failed to scan NVMe-MI endpoint after {RETRIES} attempts: {ERR} eid: {EID}",
            "RETRIES", maxRetries, "ERR", std::strerror(errno), "EID", eid);
        return false;
    }

    // Find all controllers for this EID
    nvme_mi_ctrl_t c = nullptr;
    nvme_mi_for_each_ctrl(nvmeEp, c)
    {
        controllersByEid[eid].push_back(c);
        lg2::debug("Found NVMe controller for eid: {EID}", "EID", eid);
    }

    return !controllersByEid[eid].empty();
}
