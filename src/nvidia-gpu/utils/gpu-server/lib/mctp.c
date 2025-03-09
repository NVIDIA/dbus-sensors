// mctp.c
/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

//NOLINTBEGIN
#include "mctp.h"
#include "base.h"
#include "config.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define MCTP_PREFIX_LEN 3 // tag, eid, mctp_type

static nsm_requester_rc_t mctp_recv(mctp_eid_t eid, int mctp_fd,
                                    uint8_t **nsm_resp_msg,
                                    size_t *resp_msg_len, uint8_t *tag)
{
    struct pollfd pollSet[1];
    pollSet[0].fd = mctp_fd;
    pollSet[0].events = POLLIN;
    int numFds = 1;
    int timeout = RESPONSE_TIME_OUT;

    int ret = poll(pollSet, numFds, timeout);
    if (ret <= 0) {
        return NSM_REQUESTER_RECV_TIMEOUT;
    }

    ssize_t min_len = MCTP_PREFIX_LEN + sizeof(struct nsm_msg_hdr);
    ssize_t length = recv(mctp_fd, NULL, 0, MSG_PEEK | MSG_TRUNC);
    if (length <= 0) {
        return NSM_REQUESTER_RECV_FAIL;
    } else if (length < min_len) {
        uint8_t buf[length];
        (void)recv(mctp_fd, buf, length, 0);
        return NSM_REQUESTER_INVALID_RECV_LEN;
    } else {
        struct iovec iov[2];
        uint8_t mctp_prefix[MCTP_PREFIX_LEN] = {0, 0, 0};
        size_t nsm_len = length - MCTP_PREFIX_LEN;
        iov[0].iov_len = sizeof(mctp_prefix);
        iov[0].iov_base = mctp_prefix;
        *nsm_resp_msg = malloc(nsm_len);
        iov[1].iov_len = nsm_len;
        iov[1].iov_base = *nsm_resp_msg;
        struct msghdr msg = {0};
        msg.msg_iov = iov;
        msg.msg_iovlen = sizeof(iov) / sizeof(iov[0]);
        ssize_t bytes = recvmsg(mctp_fd, &msg, 0);

        if (length != bytes) {
            free(*nsm_resp_msg);
            *nsm_resp_msg = NULL;
            return NSM_REQUESTER_INVALID_RECV_LEN;
        }

        if ((mctp_prefix[1] != eid) ||
            (mctp_prefix[2] != MCTP_MSG_TYPE_PCI_VDM)) {
            free(*nsm_resp_msg);
            *nsm_resp_msg = NULL;

            if (mctp_prefix[1] != eid) {
                return NSM_REQUESTER_EID_MISMATCH;
            }
            return NSM_REQUESTER_NOT_NSM_MSG;
        }
        *resp_msg_len = nsm_len;
        *tag = mctp_prefix[0];
        return NSM_REQUESTER_SUCCESS;
    }
}

nsm_requester_rc_t nsm_recv_any(mctp_eid_t eid, int mctp_fd,
                                uint8_t **nsm_resp_msg, size_t *resp_msg_len,
                                uint8_t *tag)
{
    nsm_requester_rc_t rc =
        mctp_recv(eid, mctp_fd, nsm_resp_msg, resp_msg_len, tag);
    if (rc != NSM_REQUESTER_SUCCESS) {
        return rc;
    }

    struct nsm_msg_hdr *hdr = (struct nsm_msg_hdr *)(*nsm_resp_msg);
    if (hdr->request != 0 || hdr->datagram != 0) {
        free(*nsm_resp_msg);
        *nsm_resp_msg = NULL;
        return NSM_REQUESTER_NOT_RESP_MSG;
    }

    if (*resp_msg_len <
        (sizeof(struct nsm_msg_hdr) + NSM_RESPONSE_MIN_LEN)) {
        free(*nsm_resp_msg);
        *nsm_resp_msg = NULL;
        return NSM_REQUESTER_RESP_MSG_TOO_SMALL;
    }

    return NSM_REQUESTER_SUCCESS;
}

nsm_requester_rc_t nsm_recv(mctp_eid_t eid, int mctp_fd, uint8_t instance_id,
                            uint8_t **nsm_resp_msg, size_t *resp_msg_len)
{
    uint8_t tag;
    nsm_requester_rc_t rc =
        nsm_recv_any(eid, mctp_fd, nsm_resp_msg, resp_msg_len, &tag);
    if (rc != NSM_REQUESTER_SUCCESS) {
        return rc;
    }

    struct nsm_msg_hdr *hdr = (struct nsm_msg_hdr *)(*nsm_resp_msg);
    if (hdr->instance_id != instance_id) {
        free(*nsm_resp_msg);
        *nsm_resp_msg = NULL;
        return NSM_REQUESTER_INSTANCE_ID_MISMATCH;
    }

    return NSM_REQUESTER_SUCCESS;
}
//NOLINTEND
