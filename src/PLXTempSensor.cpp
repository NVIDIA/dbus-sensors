#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <PLXTempSensor.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <array>
#include <iostream>
#include <istream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

static constexpr double maxReading = 127;
static constexpr double minReading = -128;

PLXTempSensor::PLXTempSensor(const std::string& objectType,
                             sdbusplus::asio::object_server& objectServer,
                             std::shared_ptr<sdbusplus::asio::connection>& conn,
                             boost::asio::io_service& io,
                             const std::string& sensorName,
                             std::vector<thresholds::Threshold>&& thresholdsIn,
                             const std::string& sensorConfiguration,
                             const PowerState powerState, uint8_t deviceBus,
                             uint8_t deviceAddress, const float pollRate) :
    Sensor(boost::replace_all_copy(sensorName, " ", "_"),
           std::move(thresholdsIn), sensorConfiguration, objectType, false,
           false, maxReading, minReading, conn, powerState),
    std::enable_shared_from_this<PLXTempSensor>(), objServer(objectServer),
    waitTimer(io), deviceBus(deviceBus), deviceAddress(deviceAddress),
    sensorPollMs(static_cast<unsigned int>(pollRate * 1000))
{
    // add interface under sensor so it can be viewed as a sensor
    sensorInterface = objectServer.add_interface(
        "/xyz/openbmc_project/sensors/temperature/" + name,
        "xyz.openbmc_project.Sensor.Value");

    for (const auto& threshold : thresholds)
    {
        std::string interface = thresholds::getInterface(threshold.level);
        thresholdInterfaces[static_cast<size_t>(threshold.level)] =
            objectServer.add_interface(
                "/xyz/openbmc_project/sensors/temperature/" + name, interface);
    }
    association = objectServer.add_interface(
        "/xyz/openbmc_project/sensors/temperature/" + name,
        association::interface);
    hwInit();
    setInitialProperties(sensor_paths::unitDegreesC);
}

PLXTempSensor::~PLXTempSensor()
{
    waitTimer.cancel();
    for (const auto& iface : thresholdInterfaces)
    {
        objServer.remove_interface(iface);
    }
    objServer.remove_interface(sensorInterface);
    objServer.remove_interface(association);
}

void PLXTempSensor::setupRead(void)
{
    if (!readingStateGood())
    {
        markAvailable(false);
        updateValue(std::numeric_limits<double>::quiet_NaN());
        restartRead();
        return;
    }
    if (!updateReading())
    {
        restartRead();
    }
}

void PLXTempSensor::restartRead()
{
    std::weak_ptr<PLXTempSensor> weakRef = weak_from_this();
    waitTimer.expires_from_now(boost::posix_time::milliseconds(sensorPollMs));
    waitTimer.async_wait([weakRef](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            return; // we're being canceled
        }
        std::shared_ptr<PLXTempSensor> self = weakRef.lock();
        if (!self)
        {
            return;
        }
        self->setupRead();
    });
}

// The format for I2C Register Write and Read as per Atlas
/* Register write :-
 * <slave address>,<CommandByte1>,<CommandByte2>,<CommandByte3>,CommandByte4>,
 * <DataByte1>,<DataByte2>,<DataByte3>,<DataByte4>
 */
/*Register Read
 * packet 1 for selecting register to read
 * <slave address>,<CommandByte1>,<CommandByte2>,<CommandByte3>,<CommandByte4>
 * packet 2 to read 32-bit register
 * <slave address>,<BufferByet3>,<BufferByte2>,<BufferByte1>,<BufferByte0>
 */

bool PLXTempSensor::updateReading()
{

    std::string i2cBus = "/dev/i2c-" + std::to_string(deviceBus);
    uint8_t slaveAddr = deviceAddress;
    int16_t rawValue = 0;
    int16_t respValue = 0;
    int16_t reading = 0;
    std::array<uint16_t, 2> regValue{};
    int file = open(i2cBus.c_str(), O_RDWR);
    if (file < 0)
    {
        std::cerr << "Plx temp sensor " << name << " not valid " << i2cBus
                  << std::string(std::strerror(errno)) << "\n";
        return false;
    }
    if (ioctl(file, I2C_SLAVE, slaveAddr) < 0)
    {
        std::cerr << "unable to set device address "
                  << std::string(std::strerror(errno)) << "\n";
        close(file);
        return false;
    }

    // setting register to read
    std::array<uint8_t, arrayLenWrite> setReg1{0x03, 0x58, 0x3c, 0x40,
                                               0xff, 0xe7, 0x85, 0x04};
    if (i2cWrite(file, setReg1.data(), static_cast<ssize_t>(setReg1.size())))
    {
        std::cerr << "Error while setting register to read, Register:";
        for (const auto itr : setReg1)
        {
            std::cerr << " 0x" << std::hex << static_cast<uint16_t>(itr);
        }
        return false;
    }

    std::array<uint8_t, arrayLenWrite> setReg2{0x03, 0x58, 0x3c, 0x41,
                                               0x20, 0x06, 0x53, 0xe8};
    if (i2cWrite(file, setReg2.data(), static_cast<ssize_t>(setReg2.size())))
    {
        std::cerr << "Error while setting register to read, Register:";
        for (const auto itr : setReg2)
        {
            std::cerr << " 0x" << std::hex << static_cast<uint16_t>(itr);
        }
        return false;
    }
    std::array<uint8_t, arrayLenWrite> setReg3{0x03, 0x58, 0x3c, 0x42,
                                               0x00, 0x00, 0x00, 0x01};
    if (i2cWrite(file, setReg3.data(), static_cast<ssize_t>(setReg3.size())))
    {
        std::cerr << "Error while setting register to read, Register:";
        for (const auto itr : setReg3)
        {
            std::cerr << " 0x" << std::hex << static_cast<uint16_t>(itr);
        }
        return false;
    }
    std::array<uint8_t, arrayLenWrite> setReg4{0x03, 0x58, 0x3c, 0x40,
                                               0xff, 0xe7, 0x85, 0x34};
    if (i2cWrite(file, setReg4.data(), static_cast<ssize_t>(setReg4.size())))
    {
        std::cerr << "Error while setting register to read, Register:";
        for (const auto itr : setReg4)
        {
            std::cerr << " 0x" << std::hex << static_cast<uint16_t>(itr);
        }
        return false;
    }
    std::array<uint8_t, arrayLenWrite> setReg5{0x03, 0x58, 0x3c, 0x42,
                                               0x00, 0x00, 0x00, 0x02};
    if (i2cWrite(file, setReg5.data(), static_cast<ssize_t>(setReg5.size())))
    {
        std::cerr << "Error while setting register to read, Register:";
        for (const auto itr : setReg5)
        {
            std::cerr << " 0x" << std::hex << static_cast<uint16_t>(itr);
        }
        return false;
    }

    // Getting Temperature reading

    std::array<uint8_t, arrayLenWrite> readRegValue1{0x03, 0x00, 0x3c, 0xb3,
                                                     0x00, 0x00, 0x00, 0x07};
    if (i2cWrite(file, readRegValue1.data(),
                 static_cast<ssize_t>(readRegValue1.size())))
    {
        std::cerr << "Error while writing to register:";
        for (const auto itr : readRegValue1)
        {
            std::cerr << " 0x" << std::hex << static_cast<uint16_t>(itr);
        }
        return false;
    }

    std::array<uint8_t, arrayLenWrite> readRegValue2{0x03, 0x58, 0x3c, 0x40,
                                                     0xff, 0xe7, 0x85, 0x38};
    if (i2cWrite(file, readRegValue2.data(),
                 static_cast<ssize_t>(readRegValue2.size())))
    {
        std::cerr << "Error while writing to register:";
        for (const auto itr : readRegValue2)
        {
            std::cerr << " 0x" << std::hex << static_cast<uint16_t>(itr);
        }
        return false;
    }

    std::array<uint8_t, arrayLenWrite> readRegValue3{0x03, 0x58, 0x3c, 0x42,
                                                     0x00, 0x00, 0x00, 0x02};
    if (i2cWrite(file, readRegValue3.data(),
                 static_cast<ssize_t>(readRegValue3.size())))
    {
        std::cerr << "Error while writing to register:";
        for (const auto itr : readRegValue3)
        {
            std::cerr << " 0x" << std::hex << static_cast<uint16_t>(itr);
        }
        return false;
    }

    std::array<uint8_t, arrayLenRead> selectReg1{0x04, 0x58, 0x3c, 0x42};
    if (i2cWrite(file, selectReg1.data(),
                 static_cast<ssize_t>(selectReg1.size())))
    {
        std::cerr << "Error while selecting register:";
        for (const auto itr : selectReg1)
        {
            std::cerr << " 0x" << std::hex << static_cast<uint16_t>(itr);
        }
        return false;
    }

    std::array<uint8_t, arrayLenRead> selectReg2{0x04, 0x58, 0x3c, 0x41};
    if (i2cWrite(file, selectReg2.data(),
                 static_cast<ssize_t>(selectReg2.size())))
    {
        std::cerr << "Error while selecting register:";
        for (const auto itr : selectReg2)
        {
            std::cerr << " 0x" << std::hex << static_cast<uint16_t>(itr);
        }
        return false;
    }
    if (read(file, regValue.data(), arrayLenRead) != 4)
    {
        std::cerr << "Error reading PLX at " << i2cBus
                  << std::string(std::strerror(errno)) << "\n";
        close(file);
        return false;
    }
    respValue = static_cast<int16_t>((regValue[0] >> 8) | (regValue[0] << 8));

    if (respValue & readingAvailableBit)
    {
        rawValue =
            static_cast<int16_t>((regValue[1] >> 8) | (regValue[1] << 8));
        // 2's complement
        if (rawValue & readingSignedBit)
        {
            rawValue = -((~rawValue & 0xffff) + 1);
        }
        reading = rawValue / 128;
        updateValue(reading);
    }

    close(file);
    restartRead();
    return true;
}

void PLXTempSensor::checkThresholds(void)
{
    thresholds::checkThresholds(this);
}

void PLXTempSensor::hwInit()
{

    std::string i2cBus = "/dev/i2c-" + std::to_string(deviceBus);
    uint8_t slaveAddr = deviceAddress;
    int file = open(i2cBus.c_str(), O_RDWR);
    if (file < 0)
    {
        std::cerr << "Plx temp sensor " << name << " not valid " << i2cBus
                  << std::string(std::strerror(errno)) << "\n";
    }

    if (ioctl(file, I2C_SLAVE, slaveAddr) < 0)
    {
        std::cerr << "unable to set device address "
                  << std::string(std::strerror(errno)) << "\n";
        close(file);
        return;
    }

    std::array<uint8_t, arrayLenWrite> initRegValue1{0x03, 0x00, 0x3c, 0xb3,
                                                     0x00, 0x00, 0x00, 0x07};
    if (i2cWrite(file, initRegValue1.data(),
                 static_cast<ssize_t>(initRegValue1.size())))
    {
        std::cerr << "Error while initialization, register:";
        for (const auto itr : initRegValue1)
        {
            std::cerr << " 0x" << std::hex << static_cast<uint16_t>(itr);
        }
        return;
    }

    std::array<uint8_t, arrayLenWrite> initRegValue2{0x03, 0x58, 0x3c, 0x40,
                                                     0xff, 0xe7, 0x85, 0x04};
    if (i2cWrite(file, initRegValue2.data(),
                 static_cast<ssize_t>(initRegValue2.size())))
    {
        std::cerr << "Error while initialization, register:";
        for (const auto itr : initRegValue2)
        {
            std::cerr << " 0x" << std::hex << static_cast<uint16_t>(itr);
        }
        return;
    }

    std::array<uint8_t, arrayLenWrite> initRegValue3{0x03, 0x58, 0x3c, 0x42,
                                                     0x00, 0x00, 0x00, 0x02};
    if (i2cWrite(file, initRegValue3.data(),
                 static_cast<ssize_t>(initRegValue3.size())))
    {
        std::cerr << "Error while initialization, register:";
        for (const auto itr : initRegValue3)
        {
            std::cerr << " 0x" << std::hex << static_cast<uint16_t>(itr);
        }
        return;
    }

    close(file);
}
