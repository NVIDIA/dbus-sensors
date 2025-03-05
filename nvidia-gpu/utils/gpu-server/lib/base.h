/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

// NOLINTBEGIN
#ifndef BASE_H
#define BASE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCI_VENDOR_ID 0x10de // NVIDIA

#define OCP_TYPE 8
#define OCP_VERSION 9
#define OCP_VERSION_V2 10

/*command(1byte) + error completion code(1byte) + reason code(2bytes)*/
#define NSM_RESPONSE_ERROR_LEN 4
/*The minimum size of NSM response message is the case response with error CC.*/
#define NSM_RESPONSE_MIN_LEN NSM_RESPONSE_ERROR_LEN

/** @brief NSM completion codes
 */
enum nsm_completion_codes {
    NSM_SUCCESS = 0x00,
    NSM_ERROR = 0x01,
};

/** @brief NSM Software Error codes
 */
enum nsm_sw_codes {
    NSM_SW_SUCCESS = 0x00,
    NSM_SW_ERROR = 0x01,
    NSM_SW_ERROR_DATA = 0x02,
    NSM_SW_ERROR_LENGTH = 0x03,
    NSM_SW_ERROR_NULL = 0x04,
    NSM_SW_ERROR_COMMAND_FAIL = 0x05
};

/** @enum MessageType
 *
 *  The different message types supported by the NSM specification.
 */
typedef enum {
    NSM_RESPONSE = 0,          //!< NSM response message
    NSM_EVENT_ACKNOWLEDGMENT = 1, //!< NSM event acknowledgement
    NSM_REQUEST = 2,          //!< NSM request message
    NSM_EVENT = 3,            //!< NSM event message
} NsmMessageType;

/** @struct nsm_msg_hdr
 *
 * Structure representing NSM message header fields
 */
struct nsm_msg_hdr {
    uint16_t pci_vendor_id; //!< PCI defined vendor ID for NVIDIA (0x10DE)

    uint8_t instance_id : 5; //!< Instance ID
    uint8_t reserved : 1;    //!< Reserved
    uint8_t datagram : 1;    //!< Datagram bit
    uint8_t request : 1;     //!< Request bit

    uint8_t ocp_version : 4; //!< OCP version
    uint8_t ocp_type : 4;    //!< OCP type

    uint8_t nvidia_msg_type; //!< NVIDIA Message Type
} __attribute__((packed));

/** @struct nsm_msg
 *
 * Structure representing NSM message
 */
struct nsm_msg {
    struct nsm_msg_hdr hdr; //!< NSM message header
    uint8_t payload[1];     //!< &payload[0] is the beginning of the payload
} __attribute__((packed));

/** @struct nsm_header_info
 *
 *  The information needed to prepare NSM header and this is passed to the
 *  pack_nsm_header and unpack_nsm_header API.
 */
struct nsm_header_info {
    uint8_t nsm_msg_type;
    uint8_t instance_id;
    uint8_t nvidia_msg_type;
};

/**
 * @brief Unpack the NSM header from the NSM message.
 *
 * @param[in] msg - Pointer to the NSM message header
 * @param[out] hdr - Pointer to the NSM header information
 *
 * @return 0 on success, otherwise NSM error codes.
 * @note   Caller is responsible for alloc and dealloc of msg
 *         and hdr params
 */
uint8_t unpack_nsm_header(const struct nsm_msg_hdr *msg,
                          struct nsm_header_info *hdr);

#ifdef __cplusplus
}
#endif
#endif /* BASE_H */
// NOLINTEND
