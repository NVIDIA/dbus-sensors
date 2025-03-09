// NOLINTBEGIN
#include <gtest/gtest.h>
#include "utils.hpp"
#include <fcntl.h>
#include <unistd.h>

class CustomFDTest : public ::testing::Test {
protected:
    int createTempFile() {
        char filename[] = "/tmp/customfd_test_XXXXXX";
        return mkstemp(filename);
    }
};

// Test CustomFD constructor with valid file descriptor
TEST_F(CustomFDTest, ValidFileDescriptor) {
    int fd = createTempFile();
    ASSERT_GT(fd, 0);
    
    {
        utils::CustomFD customFd(fd);
        EXPECT_EQ(fd, customFd());
        EXPECT_EQ(fd, static_cast<int>(customFd));
        
        // Verify file descriptor is still valid
        EXPECT_EQ(0, fcntl(customFd(), F_GETFD));
    }
    
    // Verify file descriptor is closed after CustomFD goes out of scope
    EXPECT_EQ(-1, fcntl(fd, F_GETFD));
    EXPECT_EQ(EBADF, errno);
}

// Test CustomFD constructor with invalid file descriptor
TEST_F(CustomFDTest, InvalidFileDescriptor) {
    {
        utils::CustomFD customFd(-1);
        EXPECT_EQ(-1, customFd());
        EXPECT_EQ(-1, static_cast<int>(customFd));
    }
    // No crash should occur when destroying CustomFD with invalid fd
}

// Test CustomFD with multiple instances
TEST_F(CustomFDTest, MultipleInstances) {
    int fd1 = createTempFile();
    int fd2 = createTempFile();
    ASSERT_GT(fd1, 0);
    ASSERT_GT(fd2, 0);
    
    {
        utils::CustomFD customFd1(fd1);
        {
            utils::CustomFD customFd2(fd2);
            EXPECT_EQ(fd2, customFd2());
            // fd2 should be closed here
        }
        EXPECT_EQ(-1, fcntl(fd2, F_GETFD));
        EXPECT_EQ(EBADF, errno);
        
        EXPECT_EQ(fd1, customFd1());
        EXPECT_EQ(0, fcntl(customFd1(), F_GETFD));
        // fd1 should be closed here
    }
    EXPECT_EQ(-1, fcntl(fd1, F_GETFD));
    EXPECT_EQ(EBADF, errno);
}
// NOLINTEND
