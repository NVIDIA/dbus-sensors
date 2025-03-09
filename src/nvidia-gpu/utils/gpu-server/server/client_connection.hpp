#pragma once

#include "gpuserver.h"
#include "gpuserver_mctp_discovery.h"

#include "coroutine.hpp" 
#include "globals.hpp"
#include "mctp_endpoint_manager.hpp"
#include "handler.hpp"

#include <sdeventplus/event.hpp>
#include <sdeventplus/source/io.hpp>

#include <array>
#include <functional>
#include <memory>

namespace gpuserver
{

class ClientConnection
{
  public:
    using DisconnectCallback = std::function<void(int)>;

    ClientConnection(sdeventplus::Event& event, int fd,
                     requester::Handler<requester::Request>& reqHandler,
                     mctp::EndpointManager& endpointManager, bool verbose,
                     DisconnectCallback cb = nullptr);
    ~ClientConnection();

    void setDisconnectCallback(DisconnectCallback cb)
    {
        disconnectCallback = std::move(cb);
    }

  private:
    void handleMessage(sdeventplus::source::IO& io, int fd, uint32_t revents);
    void wrapper(const uint8_t* msg, size_t len);
    requester::Coroutine handleRequest(const uint8_t* msg, size_t len);
    requester::Coroutine handlePassthroughRequest(uint8_t eid,
                                                  const uint8_t* payload,
                                                  size_t payloadLen);
    requester::Coroutine
        handleDiscoveryRequest(const mctp_endpoint_msg* mctpMsg);
    void handleResponse(const uint8_t* msg, size_t msgSize);
    void sendResponse(const nsm_msg* response, size_t responseLen);
    void logError(const char* message, int errnum);

    int fd;
    requester::Handler<requester::Request>& reqHandler;
    mctp::EndpointManager& endpointManager;
    bool verbose;
    std::unique_ptr<sdeventplus::source::IO> io;

    static constexpr size_t maxMessageSize = 4096;

    DisconnectCallback disconnectCallback;
};

} // namespace gpuserver
