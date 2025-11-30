#pragma once

#include "Utils.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// MCTP Control Message Type
enum
{
    MCTP_CTRL_HDR_MSG_TYPE = 0x00
};

// MCTP Control Command Codes (from DSP0236)
enum
{
    MCTP_CTRL_CMD_SET_ENDPOINT_ID = 0x01,
    MCTP_CTRL_CMD_GET_ENDPOINT_ID = 0x02,
    MCTP_CTRL_CMD_GET_ENDPOINT_UUID = 0x03,
    MCTP_CTRL_CMD_ALLOCATE_ENDPOINT_IDS = 0x08
};

// MCTP Direction value
enum
{
    MCTP_DIR_TX = 0,
    MCTP_DIR_RX = 1
};

// Structure to hold MCTP TransportError signal data
struct TransportErrorInfo
{
    uint32_t errorCode = 0;
    uint8_t direction = 0;
    uint8_t binding = 0;
    uint8_t srcEid = 0;
    uint8_t destEid = 0;
    uint8_t tag = 0;
    uint8_t msgType = 0;
    uint8_t commandCode = 0;
    std::string interface;
};

// Structure to hold MCTP command information
struct MCTPCommandInfo
{
    std::string timeoutErrorMessage;
    std::string logMessage;
    std::string driverOperation;
};

// Lookup table for MCTP control command information
static const std::map<uint8_t, MCTPCommandInfo> mctpCommandTable = {
    {MCTP_CTRL_CMD_SET_ENDPOINT_ID,
     {"MCTP device discovery failed due to device error SetEID Timeout",
      "MCTP SetEID Timeout on EID", "SetEndpointID"}},
    {MCTP_CTRL_CMD_GET_ENDPOINT_UUID,
     {"MCTP device discovery failed due to device error Get UUID Timeout",
      "MCTP Get UUID Timeout on EID", "MCTP Ping"}},
    {MCTP_CTRL_CMD_ALLOCATE_ENDPOINT_IDS,
     {"MCTP device discovery failed due to device error Allocate EID Timeout",
      "MCTP Allocate EID Timeout on EID", "AllocateEndpointIDs"}}};

/**
 * @brief Log MCTP error to Redfish
 * @param deviceName Name of the device
 * @param destEid EID of the device
 * @param errorCode Error code
 * @param errorMessage Error message description
 */
void logMCTPError(const std::string& deviceName, uint8_t destEid, int errorCode,
                  const std::string& errorMessage);

/**
 * @brief Create MCTP transport error Redfish event
 * @param errorCode Transport error code
 * @param direction Direction of message (TX/RX)
 * @param binding MCTP binding type
 * @param destEid Destination EID
 * @param driverOperation Driver operation name
 * @param deviceName Device name
 */
void createMctpTransportRedfishEvent(
    uint32_t errorCode, uint8_t direction, uint8_t binding, uint8_t destEid,
    const std::string& driverOperation, const std::string& deviceName);

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
