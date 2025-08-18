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

#include <FileHandle.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>

class NVMeMiContext :
    public NVMeContext,
    public std::enable_shared_from_this<NVMeMiContext>
{
  public:
    NVMeMiContext(boost::asio::io_context& io, uint8_t eid);
    ~NVMeMiContext() override = default;
    void close() override;
    void pollNVMeDevices() override;
    void readAndProcessNVMeSensor() override;
    void processResponse(void* msg, size_t len);

    void setupPipes(FileHandle reqPipe, FileHandle respPipe,
                    boost::asio::posix::stream_descriptor reqStream,
                    boost::asio::posix::stream_descriptor respStream);

    // Get pipes for communication manager
    FileHandle& getRequestPipe()
    {
        return requestPipe;
    }
    FileHandle& getResponsePipe()
    {
        return responsePipe;
    }

  private:
    void sendNVMeMICommand();

    uint8_t eid{0};
    int consecutiveFailures{0};
    static constexpr int maxConsecutiveFailures = 3;

    // Communication pipes
    FileHandle requestPipe;
    FileHandle responsePipe;
    boost::asio::posix::stream_descriptor requestStream;
    boost::asio::posix::stream_descriptor responseStream;
};
