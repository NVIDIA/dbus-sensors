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

#include "FileHandle.hpp"
#include "NVMeMiContext.hpp"

#include <endian.h>
#include <nvme/tree.h>
#include <sys/poll.h>
#ifdef __cplusplus
extern "C"
{
#endif

#include <nvme/log.h>
#include <nvme/mi.h>
#include <nvme/types.h>

#ifdef __cplusplus
}
#endif
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <phosphor-logging/lg2.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Initialize static member
nvme_root_t NVMeMiManager::nvmeRoot = nullptr;

ContextCommInfo::ContextCommInfo(boost::asio::io_context& io, uint8_t eid) :
    eid(eid)
{
    (void)io; // Suppress unused parameter warning
}

ContextCommInfo::~ContextCommInfo()
{
    if (nvmeEp != nullptr)
    {
        nvme_mi_close(nvmeEp);
    }
}

NVMeMiManager::NVMeMiManager(boost::asio::io_context& io) : io(io)
{
    // Initialize the static NVMe root
    if (nvmeRoot == nullptr)
    {
        nvmeRoot = nvme_mi_create_root(stderr, DEFAULT_LOGLEVEL);
    }
}

NVMeMiManager::~NVMeMiManager()
{
    stop();
}

bool NVMeMiManager::addContext(const std::shared_ptr<NVMeMiContext>& context,
                               int net, uint8_t eid)
{
    // This prevents duplicate sensor creation when multiple async callbacks
    {
        std::lock_guard<std::mutex> lock(contextsMutex);
        if (contexts.find(eid) != contexts.end())
        {
            return false;
        }
    }

    auto commInfo = std::make_unique<ContextCommInfo>(io, eid);
    commInfo->context = context;
    auto& ep = commInfo->nvmeEp;

    ep = nvme_mi_open_mctp(nvmeRoot, net, eid);
    if (ep == nullptr)
    {
        std::cerr << "Failed to create MCTP endpoint for eid: "
                  << static_cast<int>(eid) << std::endl;
        return false;
    }

    // Create pipes for communication
    std::array<int, 2> requestPipeFds{};
    std::array<int, 2> responsePipeFds{};

    // Create request pipe
    if (::pipe(requestPipeFds.data()) == -1)
    {
        return false;
    }

    // Create response pipe
    if (::pipe(responsePipeFds.data()) == -1)
    {
        ::close(requestPipeFds[0]);
        ::close(requestPipeFds[1]);
        return false;
    }

    // Set up pipes in the context
    context->setupPipes(
        FileHandle(requestPipeFds[0]), FileHandle(responsePipeFds[1]),
        boost::asio::posix::stream_descriptor(io, requestPipeFds[1]),
        boost::asio::posix::stream_descriptor(io, responsePipeFds[0]));

    {
        std::lock_guard<std::mutex> lock(contextsMutex);
        contexts[eid] = std::move(commInfo);
    }

    lg2::info("Added context for eid: {EID}", "EID", static_cast<int>(eid));
    return true;
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
            if (commInfo->nvmeEp != nullptr)
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
    struct pollfd pfd{};
    pfd.fd = commInfo.context->getRequestPipe().handle();
    pfd.events = POLLIN;
    pfd.revents = 0;

    int pollResult = poll(&pfd, 1, 0); // Non-blocking poll

    if (pollResult > 0 && ((pfd.revents & POLLIN) != 0))
    {
        // Process the command
        ssize_t rc =
            processMiCommand(commInfo.context->getRequestPipe(),
                             commInfo.context->getResponsePipe(), commInfo.eid);
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

    // Error handling - writes error response and returns
    auto handleError = [&out, eid](uint32_t respLen) -> ssize_t {
        uint32_t len = htole32(respLen);
        ssize_t writeResult = ::write(out.handle(), &len, sizeof(len));
        if (writeResult != static_cast<ssize_t>(sizeof(len)))
        {
            // EPIPE means the pipe was closed (likely during context removal)
            // This is not an error, just log debug message
            if (errno == EPIPE)
            {
                lg2::debug(
                    "Response pipe closed for eid: {EID} (context removed)",
                    "EID", static_cast<int>(eid));
            }
            return -errno;
        }
        return 0;
    };

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

    int cmd = req[0];

    constexpr uint8_t nvmeMiCmdHealthStatusPoll = 0x01;

    if (cmd == nvmeMiCmdHealthStatusPoll) // Health Status Polling
    {
        resp.resize(sizeof(struct nvme_mi_nvm_ss_health_status));
        // Get controllers for this EID from the map
        auto& ctrlList = controllersByEid[eid];
        if (ctrlList.empty())
        {
            // retry to get controllers
            auto it = contexts.find(eid);
            if (it != contexts.end() && (it->second->nvmeEp != nullptr))
            {
                if (scanControllers(eid, it->second->nvmeEp))
                {
                    ctrlList = controllersByEid[eid];
                    // delay to allow next command to be processed by drive
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }
        }

        if (ctrlList.empty())
        {
            return handleError(0);
        }

        // Get the endpoint for this EID
        auto it = contexts.find(eid);
        if (it == contexts.end() || it->second->nvmeEp == nullptr)
        {
            return handleError(0);
        }

        nvme_mi_ep_t ep = it->second->nvmeEp;

        struct nvme_mi_nvm_ss_health_status log{};
        std::memcpy(&log, resp.data(), sizeof(log));

        // Use the proper NVMe-MI health status polling function
        rc = nvme_mi_mi_subsystem_health_status_poll(ep, false, &log);

        if (rc != 0)
        {
            lg2::debug(
                "Failed to get health status for eid: {EID}, error: {ERR}",
                "EID", static_cast<int>(eid), "ERR", rc);
        }
        else
        {
            // Copy the updated log data back into the response buffer
            std::memcpy(resp.data(), &log, sizeof(log));
        }
    }
    else
    {
        lg2::error("Invalid command: {CMD}", "CMD", cmd);
        rc = -EINVAL;
    }

    if (rc != 0)
    {
        return handleError(0);
    }

    // Write out the response length
    uint32_t respLen = htole32(static_cast<uint32_t>(resp.size()));
    ssize_t writeResult = ::write(out.handle(), &respLen, sizeof(respLen));
    if (writeResult != static_cast<ssize_t>(sizeof(respLen)))
    {
        // EPIPE means the pipe was closed (likely during context removal)
        if (errno == EPIPE)
        {
            lg2::debug("Response pipe closed for eid: {EID} (context removed)",
                       "EID", static_cast<int>(eid));
        }
        return -errno;
    }

    // Write out the response data
    writeResult = ::write(out.handle(), resp.data(), resp.size());
    if (writeResult != static_cast<ssize_t>(resp.size()))
    {
        // EPIPE means the pipe was closed (likely during context removal)
        if (errno == EPIPE)
        {
            lg2::debug("Response pipe closed for eid: {EID} (context removed)",
                       "EID", static_cast<int>(eid));
        }
        return -errno;
    }

    return 0;
}

bool NVMeMiManager::scanControllers(uint8_t eid, nvme_mi_ep_t& nvmeEp)
{
    lg2::debug("Scanning MI controllers for eid: {EID}", "EID", eid);
    // Scan for controllers with retry logic
    int rc = 0;
    rc = nvme_mi_scan_ep(nvmeEp, true);
    if (rc != 0)
    {
        lg2::error(
            "Failed to scan NVMe-MI endpoint after {RETRIES} attempts: {ERR} eid: {EID}",
            "RETRIES", retryCount + 1, "ERR", std::strerror(errno), "EID", eid);
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
