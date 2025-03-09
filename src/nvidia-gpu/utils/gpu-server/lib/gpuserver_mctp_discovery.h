// NOLINTBEGIN
#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct gpuserver_ctx gpuserver_ctx;

typedef enum
{
    MCTP_ENDPOINT_ADDED = 0,
    MCTP_ENDPOINT_REMOVED = 1,
    MCTP_ENDPOINT_UPDATED = 2,
} mctp_endpoint_event;

struct mctp_endpoint_msg
{
    uint8_t event;
    uint8_t eid;
    uint8_t type;
    uint8_t protocol;
    uint16_t address_len;
    uint8_t address[1];
} __attribute__((packed));

/**
 * @brief Send MCTP endpoint discovery information to gpuserverd
 * @param ctx The GPUSERVER context
 * @param event Type of endpoint event (add/remove/update)
 * @param eid MCTP Endpoint ID
 * @param type Socket type
 * @param protocol Socket protocol
 * @param address Socket address bytes
 * @param address_len Length of address bytes
 * @return 0 on success, negative on error
 */
int gpuserver_mctp_add_endpoint(gpuserver_ctx* ctx,
                                mctp_endpoint_event event, uint8_t eid,
                                uint8_t type, uint8_t protocol,
                                const uint8_t* address, size_t address_len);

#ifdef __cplusplus
}
#endif
// NOLINTEND
