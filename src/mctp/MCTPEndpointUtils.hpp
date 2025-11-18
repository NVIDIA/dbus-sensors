#pragma once

#include "Utils.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Global set of EIDs for suppressing errors during recovery or health check
extern std::set<uint8_t> suppressedHealthCheckEids;

/**
 * @brief Log MCTP ping failure to Redfish
 * @param deviceName Name of the device that failed to respond
 * @param eid EID of the device
 */
void logMCTPPingFailure(const std::string& deviceName, uint8_t eid);

/**
 * @brief Get polling interval from configuration
 * @param iface Configuration map
 * @return Optional polling interval (0-180 seconds)
 */
std::optional<uint8_t> getPollingInterval(const SensorBaseConfigMap& iface);

/**
 * @brief Get device names from configuration
 * @param iface Configuration map
 * @return Vector of device names
 */
std::vector<std::string> getDeviceNames(const SensorBaseConfigMap& iface);
