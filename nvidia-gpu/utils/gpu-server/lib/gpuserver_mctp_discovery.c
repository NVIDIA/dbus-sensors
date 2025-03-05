//NOLINTBEGIN
#include "gpuserver_mctp_discovery.h"

#include "gpuserver.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int gpuserver_mctp_add_endpoint(gpuserver_ctx* ctx,
                                 mctp_endpoint_event event, uint8_t eid,
                                 uint8_t type, uint8_t protocol,
                                 const uint8_t* address, size_t address_len)
{
    if (!ctx || !address || address_len == 0)
    {
        return -EINVAL;
    }

    size_t msg_size = sizeof(struct mctp_endpoint_msg) + address_len;
    uint8_t* buffer = malloc(msg_size);
    if (!buffer)
    {
        return -ENOMEM;
    }

    struct mctp_endpoint_msg* msg = (struct mctp_endpoint_msg*)buffer;
    msg->event = event;
    msg->eid = eid;
    msg->type = type;
    msg->protocol = protocol;
    msg->address_len = address_len;
    memcpy(msg->address, address, address_len);

    ssize_t result = gpuserver_send_msg(ctx, GPUSERVER_API_MCTP_DISCOVERY, 0,
                                            buffer, msg_size);
    free(buffer);
    return result < 0 ? result : 0;
}
//NOLINTEND
