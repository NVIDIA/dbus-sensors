#pragma once

#include "Utils.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

const std::string hmcBridgeError{"ResourceEvent.1.0.ResourceErrorsDetected"};
const std::string hmcBridgeInfo{"ResourceEvent.1.1.ResourceStateChanged"};

// Redfish message for MCTP endpoint discovery command failures, which mctpd
// reports through the BusOwner1 DiscoveryCommandFailed signal
const std::string mctpDiscoveryCommandFailedMessageId{
    "NvidiaResourceEvent.1.0.MCTPDiscoveryCommandFailed"};
const std::string mctpDiscoveryCommandFailedResolution{
    "Collect the BMC logs, power-cycle the baseboard, then retry the firmware update. If the issue persists, contact support."};

// Error detail logged (ResourceErrorsDetected) when an EndpointPing times out
const std::string mctpPingTimedOutMessage{"MCTP Ping timed out"};

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

// Structure to hold MCTP GeneralError signal data
struct GeneralErrorInfo
{
    uint8_t eid = 0;
    std::string errorMessage;
    std::string resolution;
};

// Failure classes carried by the mctpd DiscoveryCommandFailed signal
enum
{
    MCTP_DISCOVERY_FAIL_REQUEST_NOT_SENT = 0,
    MCTP_DISCOVERY_FAIL_RESPONSE_TIMEOUT = 1,
    MCTP_DISCOVERY_FAIL_INVALID_RESPONSE = 2,
    MCTP_DISCOVERY_FAIL_COMPLETION_CODE = 3
};

// Structure to hold MCTP DiscoveryCommandFailed signal data ("ysyyuss")
struct DiscoveryCommandFailedInfo
{
    uint8_t commandCode = 0;
    std::string command; // "SetEndpointID", "AllocateEndpointIDs", ...
    uint8_t eid = 0;     // EID the command was issued for; the EID being
                         // assigned for Set Endpoint ID
    uint8_t kind = 0;    // MCTP_DISCOVERY_FAIL_*
    uint32_t code = 0;   // errno for kinds 0-2, completion code for kind 3
    std::string reason;  // rendered as the message's third argument
    std::string interface;
};

// Structure to hold MCTP command information
struct MCTPCommandInfo
{
    // Operation name used for transport error events of this command
    std::string driverOperation;
};

// Lookup table for MCTP control command information. Timeouts of these
// commands are not rendered from TransportError: mctpd reports their
// discovery failures through DiscoveryCommandFailed, and EndpointPing
// (Get Endpoint UUID) timeouts are logged by the device health check.
static const std::map<uint8_t, MCTPCommandInfo> mctpCommandTable = {
    {MCTP_CTRL_CMD_SET_ENDPOINT_ID, {"SetEndpointID"}},
    {MCTP_CTRL_CMD_GET_ENDPOINT_UUID, {"MCTP Ping"}},
    {MCTP_CTRL_CMD_ALLOCATE_ENDPOINT_IDS, {"AllocateEndpointIDs"}}};

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
 * @brief Create the MCTPDiscoveryCommandFailed Redfish event for a failed
 *        endpoint discovery command
 * @param failure Decoded DiscoveryCommandFailed signal
 * @param deviceName Device name, or empty to fall back to EID_<eid>
 */
void createMctpDiscoveryFailureRedfishEvent(
    const DiscoveryCommandFailedInfo& failure, const std::string& deviceName);

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

/**
 * @brief Write a value to a sysfs file
 * @param path Path to the sysfs file
 * @param value Value to write
 * @return True if successful, false otherwise
 */
bool writeSysfsFile(const std::string& path, const std::string& value);

/** @brief Create log entry with explicit severity and pre-formatted args
 *
 *  @param[in] conn - D-Bus connection
 *  @param[in] deviceName - Device name
 *  @param[in] messageID - Message ID
 *  @param[in] messageArgs - Pre-formatted message arguments string
 *  @param[in] resolution - Resolution field
 */
void createMCTPLogEntry(
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::string& deviceName, const std::string& messageID,
    const std::string& messageArgs, const std::string& resolution);
