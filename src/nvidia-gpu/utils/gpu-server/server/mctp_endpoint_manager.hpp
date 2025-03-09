#pragma once

#include "gpuserver_mctp_discovery.h"

#include "socket_handler.hpp"
#include "socket_manager.hpp"

#include <map>
#include <vector>

namespace mctp
{

class EndpointManager
{
  public:
    explicit EndpointManager(mctp_socket::Handler& sockHandler,
                    bool verbose = false);

    int registerEndpoint(uint8_t eid, uint8_t type, uint8_t protocol,
                         const std::vector<uint8_t>& address);

  private:
    struct EndpointInfo
    {
        uint8_t type{};
        uint8_t protocol{};
        std::vector<uint8_t> address;
    };

    mctp_socket::Handler& sockHandler;
    std::map<uint8_t, EndpointInfo> endpoints;
    bool verbose;
};

} // namespace mctp
