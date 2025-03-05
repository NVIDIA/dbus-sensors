/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "socket_manager.hpp"
#include "types.hpp"
#include "utils.hpp"

#include <sdeventplus/event.hpp>
#include <sdeventplus/source/io.hpp>

#include <map>
#include <optional>
#include <unordered_map>

namespace requester
{
template <class>
class Handler;

class Request;
} // namespace requester

namespace mctp_socket
{

using PathName = std::string;

using namespace sdeventplus;
using namespace sdeventplus::source;

/** @class Handler
 *
 *  The Handler class abstracts the communication with multiple MCTP Tx/Rx
 *  daemons which supports different transport mechanisms. The initialisation of
 *  this class is driven by the discovery of MCTP.Endpoint interface which
 *  exposes the socket information to communicate with the endpoints.  This
 *  manager class handles the data to be read on the communication sockets by
 *  registering callbacks for EPOLLIN.
 */
class Handler
{
  public:
    Handler() = delete;
    Handler(const Handler&) = delete;
    Handler(Handler&&) = default;
    Handler& operator=(const Handler&) = delete;
    Handler& operator=(Handler&&) = delete;
    virtual ~Handler() = default;

    const uint8_t MCTP_MSG_TYPE_VDM = 0x7e;

    /** @brief Constructor
     *
     *  @param[in] event - NSM daemon's main event loop
     *  @param[in] handler - NSM request handler
     *  @param[in] verbose - Verbose tracing flag
     *  @param[in/out] manager - MCTP socket manager
     */
    explicit Handler(sdeventplus::Event& event,
                     requester::Handler<requester::Request>& handler,
                     Manager& manager,
                     bool verbose) :
        handler(handler),
        manager(manager), event(event),
        verbose(verbose)
    {}

    virtual int registerMctpEndpoint(eid_t eid, int type, int protocol,
                                     const std::vector<uint8_t>& pathName) = 0;

    virtual int sendMsg(uint8_t tag, eid_t eid, int mctpFd,
                        const uint8_t* nsmMsg, size_t nsmMsgLen) const = 0;

  private:
    virtual void handleReceivedMsg(IO& io, int fd, uint32_t revents) = 0;

    requester::Handler<requester::Request>& handler;

  protected:
    Manager& manager;
    sdeventplus::Event& event;
    bool verbose;

    std::optional<Response> processRxMsg(uint8_t tag, uint8_t eid, uint8_t type,
                                         const uint8_t* nsmMsg,
                                         size_t nsmMsgSize);
};

class InKernelHandler : public Handler
{
  public:
    using Handler::Handler;

    int registerMctpEndpoint(eid_t eid, int type, int protocol,
                             const std::vector<uint8_t>& pathName) override;

    int sendMsg(uint8_t tag, eid_t eid, int mctpFd, const uint8_t* nsmMsg,
                size_t nsmMsgLen) const override;

  private:
    void handleReceivedMsg(IO& io, int fd, uint32_t revents) override;

    std::unique_ptr<IO> io;
    int fd;
    int sendBufferSize;
    bool isFdValid{false};
};

class DaemonHandler : public Handler
{
  public:
    using Handler::Handler;

    int registerMctpEndpoint(eid_t eid, int type, int protocol,
                             const std::vector<uint8_t>& pathName) override;

    int sendMsg(uint8_t tag, eid_t eid, int mctpFd, const uint8_t* nsmMsg,
                size_t nsmMsgLen) const override;

  private:
    SocketInfo initSocket(eid_t eid, int type, int protocol,
                          const std::vector<uint8_t>& pathName);

    void handleReceivedMsg(IO& io, int fd, uint32_t revents) override;
    /** @brief Socket information for MCTP Tx/Rx daemons */
    std::map<std::vector<uint8_t>,
             std::tuple<std::unique_ptr<utils::CustomFD>, SendBufferSize,
                        std::unique_ptr<IO>>>
        socketInfoMap;
};

} // namespace mctp_socket
