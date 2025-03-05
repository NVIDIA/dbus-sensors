// mctp.h
/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

// NOLINTBEGIN
#ifndef MCTP_H
#define MCTP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef uint8_t mctp_eid_t;

#define MCTP_MSG_TYPE_PCI_VDM 0x7E
#define MCTP_TAG_NSM 3
#define MCTP_MSG_TAG_REQ (MCTP_TAG_NSM | 1 << 3)

typedef enum nsm_requester_error_codes {
    NSM_REQUESTER_SUCCESS = 0,
    NSM_REQUESTER_NOT_NSM_MSG = -2,
    NSM_REQUESTER_NOT_RESP_MSG = -3,
    NSM_REQUESTER_RESP_MSG_TOO_SMALL = -5,
    NSM_REQUESTER_INSTANCE_ID_MISMATCH = -6,
    NSM_REQUESTER_RECV_FAIL = -8,
    NSM_REQUESTER_INVALID_RECV_LEN = -9,
    NSM_REQUESTER_RECV_TIMEOUT = -10,
    NSM_REQUESTER_EID_MISMATCH = -11,
} nsm_requester_rc_t;

nsm_requester_rc_t nsm_recv(mctp_eid_t eid, int mctp_fd, uint8_t instance_id,
                            uint8_t **nsm_resp_msg, size_t *resp_msg_len);
nsm_requester_rc_t nsm_recv_any(mctp_eid_t eid, int mctp_fd,
                                uint8_t **nsm_resp_msg, size_t *resp_msg_len,
                                uint8_t *tag);

#ifdef __cplusplus
}
#endif

#endif /* MCTP_H */
// NOLINTEND
