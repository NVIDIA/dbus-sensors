#include "MCTPEndpointUtils.hpp"

#include "Utils.hpp"

#include <phosphor-logging/device_error_log.hpp>
#include <phosphor-logging/lg2.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

PHOSPHOR_LOG2_USING;

// Use the nv::lg2 namespace for error logging
using nv::lg2::CommitDeviceError;
using nv::lg2::ErrorClass;

// Global set of EIDs for suppressing errors during recovery or health check
std::set<uint8_t> suppressedHealthCheckEids;

void logMCTPPingFailure(const std::string& deviceName, uint8_t eid)
{
    // Use the MCTP transport failure error code for ping timeout
    int64_t errCode =
        nv::lg2::ErrorCode::MCTP::MCTP_TRANSPORT_FAIL_PING_TIMEOUT;

    // Fallback to EID if device name is empty
    std::string name =
        deviceName.empty() ? ("EID_" + std::to_string(eid)) : deviceName;

    std::string errorMessage = "MCTP ping failed due to timeout for the device";
    std::string resolution =
        "If problem persists, perform power cycle of the system to recover the device.";

    std::map<std::string, std::string> additionalData = {
        {"REDFISH_MESSAGE_ID", "ResourceEvent.1.0.ResourceErrorsDetected"},
        {"REDFISH_MESSAGE_ARGS", name + ", " + errorMessage},
        {"REDFISH_RESOLUTION", resolution},
        {"REDFISH_SEVERITY", "Critical"},
        {"REDFISH_ORIGIN_OF_CONDITION", name}};

    CommitDeviceError(eid, errCode, ErrorClass::MCTP, additionalData);
}

std::optional<uint8_t> getPollingInterval(const SensorBaseConfigMap& iface)
{
    std::optional<uint8_t> interval;

    if (auto it = iface.find("PollingInterval"); it != iface.end())
    {
        try
        {
            auto val =
                std::stoul(std::visit(VariantToStringVisitor(), it->second));
            if (val <= 180) // Accept 0 (disabled) or valid interval (1-180)
            {
                interval = val;
            }
        }
        catch (...)
        {
            debug("Invalid PollingInterval value in configuration, ignoring");
        }
    }

    return interval;
}

std::vector<std::string> getDeviceNames(const SensorBaseConfigMap& iface)
{
    std::vector<std::string> names;
    auto it = iface.find("Name");
    if (it == iface.end())
    {
        return names;
    }

    if (std::holds_alternative<std::string>(it->second))
    {
        std::string nameStr = std::get<std::string>(it->second);
        std::stringstream ss(nameStr);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            token.erase(0, token.find_first_not_of(" \t"));
            token.erase(token.find_last_not_of(" \t") + 1);
            if (!token.empty())
            {
                names.push_back(token);
            }
        }
    }
    else if (std::holds_alternative<std::vector<std::string>>(it->second))
    {
        names = std::get<std::vector<std::string>>(it->second);
    }
    return names;
}
