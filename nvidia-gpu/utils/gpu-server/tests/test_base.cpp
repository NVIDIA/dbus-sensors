// NOLINTBEGIN
#include <gtest/gtest.h>
#include "base.h"
#include <endian.h>

class NsmHeaderTest : public ::testing::Test {
protected:
    struct nsm_msg_hdr msg;
    struct nsm_header_info hdr;

    void SetUp() override {
        memset(&msg, 0, sizeof(msg));
        memset(&hdr, 0, sizeof(hdr));
        msg.pci_vendor_id = htobe16(PCI_VENDOR_ID);
        msg.ocp_type = OCP_TYPE;
        msg.ocp_version = OCP_VERSION;
    }
};

// Test null parameters
TEST_F(NsmHeaderTest, NullParameters) {
    EXPECT_EQ(NSM_SW_ERROR_NULL, unpack_nsm_header(nullptr, &hdr));
    EXPECT_EQ(NSM_SW_ERROR_NULL, unpack_nsm_header(&msg, nullptr));
}

// Test invalid vendor ID
TEST_F(NsmHeaderTest, InvalidVendorId) {
    msg.pci_vendor_id = htobe16(0x1234); // Invalid vendor ID
    EXPECT_EQ(NSM_SW_ERROR_DATA, unpack_nsm_header(&msg, &hdr));
}

// Test invalid OCP type
TEST_F(NsmHeaderTest, InvalidOcpType) {
    msg.ocp_type = OCP_TYPE + 1; // Invalid OCP type
    EXPECT_EQ(NSM_SW_ERROR_DATA, unpack_nsm_header(&msg, &hdr));
}

// Test invalid OCP version
TEST_F(NsmHeaderTest, InvalidOcpVersion) {
    msg.ocp_version = OCP_VERSION_V2 + 1; // Invalid OCP version
    EXPECT_EQ(NSM_SW_ERROR_DATA, unpack_nsm_header(&msg, &hdr));
}

// Test valid OCP versions
TEST_F(NsmHeaderTest, ValidOcpVersions) {
    msg.ocp_version = OCP_VERSION;
    EXPECT_EQ(NSM_SW_SUCCESS, unpack_nsm_header(&msg, &hdr));

    msg.ocp_version = OCP_VERSION_V2;
    EXPECT_EQ(NSM_SW_SUCCESS, unpack_nsm_header(&msg, &hdr));
}

// Test message type combinations
TEST_F(NsmHeaderTest, MessageTypes) {
    // Test NSM_RESPONSE (request = 0, datagram = 0)
    msg.request = 0;
    msg.datagram = 0;
    EXPECT_EQ(NSM_SW_SUCCESS, unpack_nsm_header(&msg, &hdr));
    EXPECT_EQ(NSM_RESPONSE, hdr.nsm_msg_type);

    // Test NSM_EVENT_ACKNOWLEDGMENT (request = 0, datagram = 1)
    msg.request = 0;
    msg.datagram = 1;
    EXPECT_EQ(NSM_SW_SUCCESS, unpack_nsm_header(&msg, &hdr));
    EXPECT_EQ(NSM_EVENT_ACKNOWLEDGMENT, hdr.nsm_msg_type);

    // Test NSM_REQUEST (request = 1, datagram = 0)
    msg.request = 1;
    msg.datagram = 0;
    EXPECT_EQ(NSM_SW_SUCCESS, unpack_nsm_header(&msg, &hdr));
    EXPECT_EQ(NSM_REQUEST, hdr.nsm_msg_type);

    // Test NSM_EVENT (request = 1, datagram = 1)
    msg.request = 1;
    msg.datagram = 1;
    EXPECT_EQ(NSM_SW_SUCCESS, unpack_nsm_header(&msg, &hdr));
    EXPECT_EQ(NSM_EVENT, hdr.nsm_msg_type);
}

// Test instance ID and nvidia message type preservation
TEST_F(NsmHeaderTest, FieldPreservation) {
    msg.instance_id = 0x1F;  // Max 5-bit value
    msg.nvidia_msg_type = 0xFF;
    
    EXPECT_EQ(NSM_SW_SUCCESS, unpack_nsm_header(&msg, &hdr));
    EXPECT_EQ(0x1F, hdr.instance_id);
    EXPECT_EQ(0xFF, hdr.nvidia_msg_type);
}
// NOLINTEND
