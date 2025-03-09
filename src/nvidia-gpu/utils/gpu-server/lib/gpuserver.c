#include "gpuserver.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct gpuserver_ctx
{
    int fd;
};

gpuserver_ctx* gpuserver_connect(const char* socket_path)
{
    gpuserver_ctx* ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
    {
        return NULL;
    }

    ctx->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx->fd < 0)
    {
        free(ctx);
        return NULL;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(ctx->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        close(ctx->fd);
        free(ctx);
        return NULL;
    }

    return ctx;
}

ssize_t gpuserver_send_msg(gpuserver_ctx* ctx,
                               gpuserver_api_type api_type, uint32_t device_id,
                               const uint8_t* payload, size_t payload_len)
{
    if (!ctx || !payload)
    {
        return -EINVAL;
    }

    // Calculate total message size including headers
    size_t total_size = sizeof(struct gpuserver_api_msg) + payload_len - 1;
    uint8_t* buffer = malloc(total_size);
    if (!buffer)
    {
        return -ENOMEM;
    }

    // Construct the message
    struct gpuserver_api_msg* msg = (struct gpuserver_api_msg*)buffer;
    msg->api_type = api_type;
    if (api_type == GPUSERVER_API_PASSTHROUGH_EID)
    {
        msg->device.eid = (uint8_t)device_id;
    }
    else
    {
        msg->device.uid = device_id;
    }
    msg->payload_len = payload_len;
    memcpy(msg->payload, payload, payload_len);

    // Send the message
    ssize_t result = send(ctx->fd, buffer, total_size, 0);
    free(buffer);
    return result;
}

ssize_t gpuserver_recv(gpuserver_ctx* ctx, uint8_t* resp_buf,
                           size_t resp_len)
{
    if (!ctx || !resp_buf)
    {
        return -EINVAL;
    }

    return recv(ctx->fd, resp_buf, resp_len, 0);
}

int gpuserver_get_fd(gpuserver_ctx* ctx)
{
    return ctx ? ctx->fd : -EINVAL;
}

void gpuserver_close(gpuserver_ctx* ctx)
{
    if (ctx)
    {
        if (ctx->fd >= 0)
        {
            close(ctx->fd);
        }
        free(ctx);
    }
}
