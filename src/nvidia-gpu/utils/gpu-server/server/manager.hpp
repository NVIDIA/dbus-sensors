#pragma once

#include "client_connection.hpp"
#include "mctp_endpoint_manager.hpp"
#include "socket_handler.hpp"
#include "handler.hpp"
#include "socket_manager.hpp"

#include <sdeventplus/event.hpp>
#include <sdeventplus/source/io.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <vector>

namespace gpuserver
{

using namespace sdeventplus;
using namespace sdeventplus::source;

class Manager
{
  public:
    Manager(sdeventplus::Event& event, const std::string& socketPath,
                     requester::Handler<requester::Request>& reqHandler,
                     mctp::EndpointManager& endpointManager, bool verbose);
    ~Manager();

  private:
    void initServerSocket();
    void handleClientConnection(IO& io, int fd, uint32_t revents);
    void cleanupSocket();
    void removeClientConnection(int fd);

    sdeventplus::Event& event;
    std::string socketPath;
    requester::Handler<requester::Request>& reqHandler;
    mctp::EndpointManager& endpointManager;
    bool verbose;
    int serverFd{-1};
    std::unique_ptr<IO> serverIO;

    // Map of client file descriptors to their IO handlers
    std::map<int, std::unique_ptr<ClientConnection>> clientConnections;
};

} // namespace gpuserver
