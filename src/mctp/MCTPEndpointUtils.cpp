#include "MCTPEndpointUtils.hpp"

#include "Utils.hpp"

#include <phosphor-logging/device_error_log.hpp>
#include <phosphor-logging/lg2.hpp>
#include <phosphor-logging/mctp_error_registry.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

PHOSPHOR_LOG2_USING;

// Use the nv::lg2 namespace for error logging
using nv::lg2::CommitDeviceError;
using nv::lg2::ErrorClass;

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
        throw std::invalid_argument("No 'Name' key in configuration");
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
    if (names.empty())
    {
        throw std::invalid_argument("No valid device name in configuration");
    }
    return names;
}

void logMCTPError(const std::string& deviceName, uint8_t destEid, int errorCode,
                  const std::string& errorMessage)
{
    std::string name =
        deviceName.empty() ? ("EID_" + std::to_string(destEid)) : deviceName;

    std::string resolution =
        "If problem persists, perform power cycle of the system to recover the device.";

    std::map<std::string, std::string> additionalData = {
        {"REDFISH_MESSAGE_ID", "ResourceEvent.1.0.ResourceErrorsDetected"},
        {"REDFISH_MESSAGE_ARGS", name + ", " + errorMessage},
        {"REDFISH_RESOLUTION", resolution},
        {"REDFISH_SEVERITY",
         "xyz.openbmc_project.Logging.Entry.Level.Informational"},
        {"REDFISH_ORIGIN_OF_CONDITION", name}};

    CommitDeviceError(destEid, errorCode, ErrorClass::MCTP, additionalData);
}

void createMctpTransportRedfishEvent(
    uint32_t errorCode, uint8_t direction, uint8_t binding, uint8_t destEid,
    const std::string& driverOperation, const std::string& deviceName)
{
    // Log transport error details
    debug("MCTP Transport Error - {COMMAND} to EID {EID}, Binding={BINDING}, "
          "Direction={DIR}, Error={ERROR}",
          "COMMAND", driverOperation, "EID", destEid, "BINDING", binding, "DIR",
          direction, "ERROR", errorCode);

    /* Skip EID 0 (NULL/broadcast) to prevent log flooding during recovery.
     * mctpd uses destEid=0 for discovery/recovery polls which fail repeatedly
     * until device responds.
     */
    if (destEid == 0)
    {
        return;
    }

    // Convert raw types to library enums
    auto mctpBinding = static_cast<phosphor::logging::mctp::Binding>(binding);
    auto mctpDirection =
        static_cast<phosphor::logging::mctp::Direction>(direction);

    // Try to get Redfish registry information
    auto registry = phosphor::logging::mctp::errorToRedfishRegistry(
        errorCode, mctpDirection, mctpBinding, destEid, driverOperation);

    // Rate limiting: Allow up to 3 logs per 60 seconds for the same EID
    static std::map<uint8_t, std::vector<std::chrono::steady_clock::time_point>>
        errorTimestamps;
    auto now = std::chrono::steady_clock::now();
    auto& timestamps = errorTimestamps[destEid];

    // Remove timestamps older than 60 seconds
    std::erase_if(timestamps, [now](const auto& t) {
        return std::chrono::duration_cast<std::chrono::seconds>(now - t)
                   .count() >= 60;
    });

    // Check if we've reached the limit
    if (timestamps.size() >= 3)
    {
        return; // Suppress log (rate limit exceeded)
    }
    timestamps.push_back(now);

    if (registry)
    {
        std::string name = deviceName.empty()
                               ? ("EID_" + std::to_string(destEid))
                               : deviceName;

        // Build additional data map for Redfish event
        std::map<std::string, std::string> additionalData;
        additionalData["REDFISH_MESSAGE_ID"] = registry->registryId;

        // Build comma-separated args string
        std::ostringstream argsStr;
        for (size_t i = 0; i < registry->args.size(); i++)
        {
            if (i > 0)
            {
                argsStr << ",";
            }
            // Replace "EID_0x<EID>" placeholder with actual device name if
            // available
            if (registry->args[i].starts_with("EID_") && !deviceName.empty())
            {
                argsStr << name;
            }
            else
            {
                argsStr << registry->args[i];
            }
        }
        additionalData["REDFISH_MESSAGE_ARGS"] = argsStr.str();

        if (!registry->resolution.empty())
        {
            additionalData["REDFISH_RESOLUTION"] = registry->resolution;
        }

        additionalData["REDFISH_ORIGIN_OF_CONDITION"] = name;
        additionalData["REDFISH_SEVERITY"] =
            "xyz.openbmc_project.Logging.Entry.Level.Informational";

        // Commit the error
        CommitDeviceError(destEid, errorCode, ErrorClass::MCTP, additionalData);
    }
    else
    {
        // Fallback for unmapped errors - log generic error
        warning("No Redfish registry mapping for MCTP error {ERROR}", "ERROR",
                errorCode);
    }
}

// Helper function to write a value to a sysfs file
bool writeSysfsFile(const std::string& path, const std::string& value)
{
    std::ofstream file(path);
    if (!file)
    {
        return false;
    }
    file << value;
    return file.good();
}
