// NOLINTBEGIN
#include "mctp_endpoint_manager.hpp"

#include <phosphor-logging/lg2.hpp>

namespace mctp
{

EndpointManager::EndpointManager(mctp_socket::Handler& sockHandler,
                                 bool verbose) :
    sockHandler(sockHandler),
    verbose(verbose)
{
    if (verbose)
    {
        lg2::info("Initialized MCTP Endpoint Manager");
    }
}

int EndpointManager::registerEndpoint(uint8_t eid, uint8_t type,
                                      uint8_t protocol,
                                      const std::vector<uint8_t>& address)
{
    if (verbose)
    {
        lg2::info(
            "Registering MCTP endpoint - EID: {EID}, Type: {TYPE}, Protocol: {PROTO}",
            "EID", static_cast<int>(eid), "TYPE", static_cast<int>(type),
            "PROTO", static_cast<int>(protocol));
    }

    auto result = sockHandler.registerMctpEndpoint(eid, type, protocol,
                                                   address);
    if (result < 0)
    {
        if (verbose)
        {
            lg2::error("Failed to register MCTP endpoint {EID}: {ERROR}", "EID",
                       static_cast<int>(eid), "ERROR", strerror(-result));
        }
        return result;
    }

    endpoints[eid] = EndpointInfo{type, protocol, address};

    if (verbose)
    {
        lg2::info("Successfully registered MCTP endpoint {EID}", "EID",
                  static_cast<int>(eid));
    }
    return 0;
}

} // namespace mctp
// NOLINTEND
