#include <sys/ioctl.h>
#include <unistd.h>

#include <iostream>

extern "C"
{
#include <i2c/smbus.h>
#include <linux/i2c-dev.h>
}
constexpr auto fpgaI2cAddress = 0x3c;
constexpr auto fpgaMidI2cBus = 2;

int32_t i2cRead(const uint8_t busId, const uint8_t slaveAddr,
                const uint8_t statusReg)
{
    std::string i2cBus = "/dev/i2c-" + std::to_string(busId);

    int fd = open(i2cBus.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        std::cerr << "unable to open i2c device \n";
        return -1;
    }
    if (ioctl(fd, I2C_SLAVE_FORCE, slaveAddr) < 0)
    {
        std::cerr << "unable to set device address\n";
        close(fd);
        return -1;
    }

    unsigned long funcs = 0;
    if (ioctl(fd, I2C_FUNCS, &funcs) < 0)
    {
        std::cerr << "not support I2C_FUNCS: " << strerror(errno) << "\n";
        close(fd);
        return -1;
    }

    int32_t statusValue = i2c_smbus_read_byte_data(fd, statusReg);
    close(fd);
    if (statusValue < 0)
    {
        std::cerr << "i2c_smbus_read_byte_data failed \n";
        return -1;
    }
    return statusValue;
}

int32_t i2cWrite(const uint8_t busId, const uint8_t slaveAddr,
                 const uint8_t reg, const uint8_t value)
{
    std::string i2cBus = "/dev/i2c-" + std::to_string(busId);

    int fd = open(i2cBus.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        std::cerr << "unable to open i2c device \n";
        return -1;
    }
    if (ioctl(fd, I2C_SLAVE_FORCE, slaveAddr) < 0)
    {
        std::cerr << "unable to set device address\n";
        close(fd);
        return -1;
    }

    unsigned long funcs = 0;
    if (ioctl(fd, I2C_FUNCS, &funcs) < 0)
    {
        std::cerr << "not support I2C_FUNCS: " << strerror(errno) << "\n";
        close(fd);
        return -1;
    }

    int32_t statusValue = i2c_smbus_write_byte_data(fd, reg, value);
    close(fd);
    if (statusValue < 0)
    {
        std::cerr << "i2c_smbus_write_byte_data failed \n";
        return -1;
    }
    return statusValue;
}

void setLedReg(const uint8_t reg, const uint8_t offset, bool thresholdStatus)
{
    int status = 0;
    int regValue = i2cRead(fpgaMidI2cBus, fpgaI2cAddress, reg);
    if (regValue < 0)
    {
        std::cerr << " Failed to get FAN Led status from FPGA \n ";
    }
    else
    {
        // false if a critical threshold has been crossed, true otherwise
        if (!thresholdStatus)
        {
            status =
                i2cWrite(fpgaMidI2cBus, fpgaI2cAddress, reg, (1 << offset));
            if (status < 0)
            {
                std::cerr << " Failed to set FAN Led to FPGA \n";
            }
        }
        else
        {
            status = i2cWrite(fpgaMidI2cBus, fpgaI2cAddress, reg,
                              (regValue ^ (1 << offset)));
            if (status < 0)
            {
                std::cerr << " Failed to clear FAN Led to FPGA \n";
            }
        }
    }
}
