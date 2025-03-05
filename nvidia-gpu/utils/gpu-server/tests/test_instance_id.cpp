// NOLINTBEGIN
#include <gtest/gtest.h>
#include "instance-id.h"
#include <filesystem>
#include <fstream>

class InstanceIdTest : public ::testing::Test {
protected:
    struct instance_db* ctx = nullptr;
    std::string test_db_path;

    void SetUp() override {
        // Create a temporary test database file
        test_db_path = std::filesystem::temp_directory_path() / "test_instance_db";
        std::ofstream db_file(test_db_path);
        // Fill the file with required size (MCTP_EID_MAX * INST_ID_MAX)
        db_file.seekp((256 * 32) - 1);
        db_file.write("", 1);
        db_file.close();
    }

    void TearDown() override {
        if (ctx) {
            instance_db_destroy(ctx);
            ctx = nullptr;
        }
        std::filesystem::remove(test_db_path);
    }
};

// Test initialization with invalid parameters
TEST_F(InstanceIdTest, InitWithNullContext) {
    struct instance_db* null_ctx = nullptr;
    EXPECT_EQ(-EINVAL, instance_db_init(&null_ctx, nullptr));
}

// Test initialization with valid parameters
TEST_F(InstanceIdTest, InitWithValidPath) {
    EXPECT_EQ(0, instance_db_init(&ctx, test_db_path.c_str()));
    EXPECT_NE(nullptr, ctx);
}

// Test double initialization
TEST_F(InstanceIdTest, DoubleInit) {
    EXPECT_EQ(0, instance_db_init(&ctx, test_db_path.c_str()));
    struct instance_db* second_ctx = nullptr;
    EXPECT_EQ(0, instance_db_init(&second_ctx, test_db_path.c_str()));
    instance_db_destroy(second_ctx);
}

// Test destroy with null context
TEST_F(InstanceIdTest, DestroyNullContext) {
    EXPECT_EQ(0, instance_db_destroy(nullptr));
}

// Test basic instance ID allocation and freeing
TEST_F(InstanceIdTest, BasicAllocationAndFree) {
    ASSERT_EQ(0, instance_db_init(&ctx, test_db_path.c_str()));
    
    instance_id_t iid;
    mctp_eid_t eid = 0;
    
    // Allocate an instance ID
    EXPECT_EQ(0, instance_id_alloc(ctx, eid, &iid));
    EXPECT_LT(iid, 32); // Should be less than INST_ID_MAX
    
    // Free the allocated ID
    EXPECT_EQ(0, instance_id_free(ctx, eid, iid));
}

// Test allocation with null output parameter
TEST_F(InstanceIdTest, AllocationWithNullIid) {
    ASSERT_EQ(0, instance_db_init(&ctx, test_db_path.c_str()));
    EXPECT_EQ(-EINVAL, instance_id_alloc(ctx, 0, nullptr));
}

// Test freeing an unallocated ID
TEST_F(InstanceIdTest, FreeUnallocatedId) {
    ASSERT_EQ(0, instance_db_init(&ctx, test_db_path.c_str()));
    EXPECT_EQ(-EINVAL, instance_id_free(ctx, 0, 5)); // 5 is an arbitrary unallocated ID
}
// NOLINTEND
