#pragma once

#include "gpuserver.h"
#include "gpuserver_mctp_discovery.h"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/server.hpp>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>

namespace gpuserver
{

using PropertyValue =
    std::variant<bool, uint8_t, uint32_t, std::string, std::vector<uint8_t>>;
using Properties = std::map<std::string, PropertyValue>;
using InterfaceMap = std::map<std::string, Properties>;

class MctpDiscovery
{
  public:
    /** @brief Constructor
     *  @param[in] bus - The D-Bus connection
     *  @param[in] socketPath - Path to gpuserver socket
     *  @throw std::runtime_error if connection to gpuserver fails
     */
    MctpDiscovery(sdbusplus::bus::bus& bus, const std::string& socketPath);
    ~MctpDiscovery();

  private:
    /** @brief D-Bus interface for MCTP endpoints */
    static constexpr const char* mctpEndpointIntf =
        "xyz.openbmc_project.MCTP.Endpoint";
    /** @brief D-Bus interface for Unix socket properties */
    static constexpr const char* unixSocketIntf =
        "xyz.openbmc_project.Common.UnixSocket";
    /** @brief D-Bus interface for enable/disable properties */
    static constexpr const char* enableIntf =
        "xyz.openbmc_project.Object.Enable";
    /** @brief Base path for MCTP objects */
    static constexpr const char* mctpBasePath = "/xyz/openbmc_project/mctp";

    /** @brief Handle new MCTP endpoint appearing */
    void handleEndpointAdded(sdbusplus::message::message& msg);
    /** @brief Handle property changes on MCTP endpoints */
    void handlePropertiesChanged(sdbusplus::message::message& msg);
    /** @brief Scan for existing MCTP endpoints at startup */
    void scanExistingEndpoints();

    /** @brief Process an MCTP endpoint and register/remove it with gpuserver
     *  @param[in] path - D-Bus object path of the endpoint
     *  @param[in] service - D-Bus service name owning the endpoint
     *  @param[in] enabled - Whether the endpoint is enabled
     */
    void processEndpoint(const std::string& path, const std::string& service,
                         bool enabled = true);

    sdbusplus::bus::bus& bus;
    gpuserver_ctx* ctx;

    std::unique_ptr<sdbusplus::bus::match_t> endpointAddedMatch;
    std::map<std::string, std::unique_ptr<sdbusplus::bus::match_t>>
        enableMatches;
};

} // namespace gpuserver
