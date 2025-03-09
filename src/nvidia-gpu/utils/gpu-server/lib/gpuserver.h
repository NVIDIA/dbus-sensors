// NOLINTBEGIN
#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> // for ssize_t

typedef struct gpuserver_ctx gpuserver_ctx;

typedef enum
{
    GPUSERVER_API_MCTP_DISCOVERY = 0,
    GPUSERVER_API_PASSTHROUGH_EID = 1,
} gpuserver_api_type;

struct gpuserver_api_msg
{
    uint8_t api_type;
    union
    {
        uint8_t eid;  // the EID when api_type=GPUSERVER_API_PASSTHROUGH_EID
        uint32_t uid; // the device UID when api_type=GPUSERVER_API_PASSTHROUGH
    } device;
    size_t payload_len;
    uint8_t payload[1]; // Flexible array member
};

/**
 * @brief Connect to the gpuserver daemon
 * @param socket_path Path to the Unix domain socket
 * @return Context handle on success, NULL on failure
 */
gpuserver_ctx* gpuserver_connect(const char* socket_path);

/**
 * @brief Send a message to the daemon with API headers
 * @param ctx The GPUSERVER context
 * @param api_type Type of API message
 * @param device_id Device EID or UID
 * @param payload Message buffer to send
 * @param payload_len Length of message to send
 * @return Number of bytes sent, negative on error
 */
ssize_t gpuserver_send_msg(gpuserver_ctx* ctx,
                               gpuserver_api_type api_type, uint32_t device_id,
                               const uint8_t* payload, size_t payload_len);

/**
 * @brief Receive a response from the daemon
 * @param ctx The GPUSERVER context
 * @param resp_buf Buffer to store response
 * @param resp_len Size of response buffer
 * @return Number of bytes received, negative on error
 */
ssize_t gpuserver_recv(gpuserver_ctx* ctx, uint8_t* resp_buf,
                           size_t resp_len);

/**
 * @brief Get the file descriptor for polling
 * @param ctx The GPUSERVER context
 * @return File descriptor, negative on error
 */
int gpuserver_get_fd(gpuserver_ctx* ctx);

/**
 * @brief Close the connection and free resources
 * @param ctx The GPUSERVER context
 */
void gpuserver_close(gpuserver_ctx* ctx);

#ifdef __cplusplus
}
#endif
// NOLINTEND
