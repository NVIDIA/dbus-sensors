/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

//NOLINTBEGIN
#pragma once

#include "base.h"
#include "mctp.h"
#include "types.hpp"
#include "utils.hpp"
#include "socket_handler.hpp"

#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/timer.hpp>
#include <sdeventplus/event.hpp>

#include <chrono>
#include <functional>

namespace requester
{

/** @class RequestRetryTimer
 *
 *  The abstract base class for implementing the NSM request retry logic. This
 *  class handles number of times the NSM request needs to be retried if the
 *  response is not received and the time to wait between each retry. It
 *  provides APIs to start and stop the request flow.
 */
class RequestRetryTimer
{
  public:
    RequestRetryTimer() = delete;
    RequestRetryTimer(const RequestRetryTimer&) = delete;
    RequestRetryTimer(RequestRetryTimer&&) = delete;
    RequestRetryTimer& operator=(const RequestRetryTimer&) = delete;
    RequestRetryTimer& operator=(RequestRetryTimer&&) = delete;
    virtual ~RequestRetryTimer() = default;

    /** @brief Constructor
     *
     *  @param[in] event - reference to NSM daemon's main event loop
     *  @param[in] numRetries - number of request retries
     *  @param[in] timeout - time to wait between each retry in milliseconds
     */
    explicit RequestRetryTimer(sdeventplus::Event& event, uint8_t numRetries,
                               std::chrono::milliseconds timeout) :
        event(event),
        numRetries(numRetries), timeout(timeout),
        timer(event.get(), std::bind_front(&RequestRetryTimer::callback, this))
    {}

    /** @brief Starts the request flow and arms the timer for request retries
     *
     *  @return return NSM_SUCCESS on success and NSM_ERROR otherwise
     */
    int start()
    {
        auto rc = send();
        if (rc)
        {
            return rc;
        }

        try
        {
            if (numRetries)
            {
                timer.start(duration_cast<std::chrono::microseconds>(timeout),
                            true);
            }
        }
        catch (const std::runtime_error& e)
        {
            lg2::error("Failed to start the request timer.", "ERROR", e);
            return NSM_ERROR;
        }

        return NSM_SW_SUCCESS;
    }

    /** @brief Stops the timer and no further request retries happen */
    void stop()
    {
        auto rc = timer.stop();
        if (rc)
        {
            lg2::error("Failed to stop the request timer. RC={RC}", "RC",
                       unsigned(rc));
        }
    }

  protected:
    sdeventplus::Event& event; //!< reference to NSM daemon's main event loop
    uint8_t numRetries;        //!< number of request retries
    std::chrono::milliseconds
        timeout;            //!< time to wait between each retry in milliseconds
    sdbusplus::Timer timer; //!< manages starting timers and handling timeouts

    /** @brief Sends the NSM request message
     *
     *  @return return NSM_SUCCESS on success and NSM_ERROR otherwise
     */
    virtual int send() const = 0;

    /** @brief Callback function invoked when the timeout happens */
    void callback()
    {
        if (numRetries--)
        {
            send();
        }
        else
        {
            stop();
        }
    }
};

/** @class Request
 *
 *  The concrete implementation of RequestIntf. This class implements the send()
 *  to send the NSM request message over MCTP socket.
 *  This class encapsulates the NSM request message, the number of times the
 *  request needs to retried if the response is not received and the amount of
 *  time to wait between each retry. It provides APIs to start and stop the
 *  request flow.
 */
class Request final : public RequestRetryTimer
{
  public:
    Request() = delete;
    Request(const Request&) = delete;
    Request(Request&&) = delete;
    Request& operator=(const Request&) = delete;
    Request& operator=(Request&&) = delete;
    ~Request() = default;

    /** @brief Constructor
     *
     *  @param[in] fd - fd of the MCTP communication socket
     *  @param[in] eid - endpoint ID of the remote MCTP endpoint
     *  @param[in] event - reference to NSM daemon's main event loop
     *  @param[in] requestMsg - NSM request message
     *  @param[in] numRetries - number of request retries
     *  @param[in] timeout - time to wait between each retry in milliseconds
     */
    explicit Request(int fd, eid_t eid, uint8_t tag, sdeventplus::Event& event,
                     const mctp_socket::Handler* handler,
                     std::vector<uint8_t>&& requestMsg, uint8_t numRetries,
                     std::chrono::milliseconds timeout) :
        RequestRetryTimer(event, numRetries, timeout),
        fd(fd), eid(eid), tag(tag), requestMsg(std::move(requestMsg)),
        socketHandler(handler)
    {}

    uint8_t getInstanceId()
    {
        auto nsmMsg = reinterpret_cast<nsm_msg*>(requestMsg.data());
        return nsmMsg->hdr.instance_id;
    }

    void setInstanceId(uint8_t instanceId)
    {
        auto nsmMsg = reinterpret_cast<nsm_msg*>(requestMsg.data());
        nsmMsg->hdr.instance_id = instanceId;
    }

    std::string requestMsgToString() const
    {
        std::ostringstream oss;
        for (const auto& byte : requestMsg)
        {
            oss << std::setfill('0') << std::setw(2) << std::hex
                << static_cast<int>(byte) << " ";
        }
        return oss.str();
    }

  private:
    int fd;      //!< file descriptor of MCTP communications socket
    eid_t eid;   //!< endpoint ID of the remote MCTP endpoint
    uint8_t tag; //!< tag mctp message tag to be used
    std::vector<uint8_t> requestMsg;           //!< NSM request message
    const mctp_socket::Handler* socketHandler; // MCTP socket handler

    /** @brief Sends the NSM request message on the socket
     *
     *  @return return NSM_SUCCESS on success and NSM_ERROR otherwise
     */
    int send() const
    {
        auto rc = socketHandler->sendMsg(tag, eid, fd, requestMsg.data(),
                                         requestMsg.size());
        if (rc < 0)
        {
            lg2::error("Failed to send NSM message. RC={RC}, errno={ERRNO}",
                       "RC", unsigned(rc), "ERRNO", strerror(errno));
            return NSM_SW_ERROR;
        }
        return NSM_SW_SUCCESS;
    }
};

} // namespace requester
//NOLINTEND
