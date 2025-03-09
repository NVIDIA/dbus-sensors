/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "base.h"

#include <endian.h>
#include <limits.h>
#include <string.h>

uint8_t unpack_nsm_header(const struct nsm_msg_hdr *msg,
			  struct nsm_header_info *hdr)
{
	if (msg == NULL || hdr == NULL) {
		return NSM_SW_ERROR_NULL;
	}

	if (be16toh(msg->pci_vendor_id) != PCI_VENDOR_ID) {
		return NSM_SW_ERROR_DATA;
	}

	if (msg->ocp_type != OCP_TYPE) {
		return NSM_SW_ERROR_DATA;
	}

	if (msg->ocp_version != OCP_VERSION &&
	    msg->ocp_version != OCP_VERSION_V2) {
		return NSM_SW_ERROR_DATA;
	}

	if (msg->request == 0) {
		hdr->nsm_msg_type =
		    msg->datagram ? NSM_EVENT_ACKNOWLEDGMENT : NSM_RESPONSE;
	} else {
		hdr->nsm_msg_type = msg->datagram ? NSM_EVENT : NSM_REQUEST;
	}

	hdr->instance_id = msg->instance_id;
	hdr->nvidia_msg_type = msg->nvidia_msg_type;

	return NSM_SW_SUCCESS;
}
