#include <unistd.h>

#include <NVMeStatus.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <cerrno>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

extern "C"
{
#include <i2c/smbus.h>
#include <linux/i2c-dev.h>
}

NVMeStatus::NVMeStatus(sdbusplus::asio::object_server& objectServer,
                       std::shared_ptr<sdbusplus::asio::connection>& conn,
                       boost::asio::io_service& io,
                       const std::string& sensorName,
                       const std::string& sensorConfiguration,
                       unsigned int pollRate, uint8_t index, uint8_t busId,
                       uint8_t cpldAddress, uint8_t statusReg) :
    ItemInterface(
        static_cast<sdbusplus::bus::bus&>(*conn),
        ("/xyz/openbmc_project/sensors/drive/" + escapeName(sensorName))
            .c_str(),
        ItemInterface::action::defer_emit),
    std::enable_shared_from_this<NVMeStatus>(), name(sensorName),
    sensorPollSec(pollRate), index(index), busId(busId),
    cpldAddress(cpldAddress), statusReg(statusReg), objServer(objectServer),
    waitTimer(io)
{
    sensorInterface = objectServer.add_interface(
        ("/xyz/openbmc_project/sensors/drive/" + escapeName(sensorName))
            .c_str(),
        DriveInterface::interface);

    fs::path p(sensorConfiguration);
    AssociationList assocs = {};
    assocs.emplace_back(
        std::make_tuple("chassis", "all_sensors", p.parent_path().string()));
    sdbusplus::xyz::openbmc_project::Association::server::Definitions::
        associations(assocs);
    if (!sensorInterface->initialize())
    {
        std::cerr << "error initializing value interface\n";
    }
    monitor();
}

NVMeStatus::~NVMeStatus()
{
    waitTimer.cancel();
    objServer.remove_interface(sensorInterface);
}

int NVMeStatus::getCPLDRegsInfo(uint8_t regs, int16_t* pu16data)
{
    std::string i2cBus = "/dev/i2c-" + std::to_string(busId);
    int fd = open(i2cBus.c_str(), O_RDWR);

    if (fd < 0)
    {
        std::cerr << " unable to open i2c device " << i2cBus << ": "
                  << strerror(errno) << "\n";
        return -1;
    }

    if (ioctl(fd, I2C_SLAVE_FORCE, cpldAddress) < 0)
    {
        std::cerr << " unable to set device address: " << strerror(errno)
                  << "\n";
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

    if (!(funcs & I2C_FUNC_SMBUS_READ_WORD_DATA))
    {
        std::cerr << " not support I2C_FUNC_SMBUS_READ_WORD_DATA\n";
        close(fd);
        return -1;
    }

    *pu16data = i2c_smbus_read_word_data(fd, regs);
    close(fd);

    if (*pu16data < 0)
    {
        std::cerr << " read word data failed at " << static_cast<int>(regs)
                  << ": " << strerror(errno) << "\n";
        return -1;
    }

    return 0;
}

void NVMeStatus::monitor(void)
{

    waitTimer.expires_from_now(boost::posix_time::seconds(sensorPollSec));
    waitTimer.async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            std::cerr << "Read operation aborted\n";
            return; // we're being cancelled
        }
        // read timer error
        if (ec)
        {
            std::cerr << "timer error\n";
            return;
        }
        int16_t status = 0;
        int ret = getCPLDRegsInfo(statusReg, &status);
        if (ret >= 0)
        {
            if (status & (1 << index))
            {
                sdbusplus::xyz::openbmc_project::Inventory::server::Item::
                    present(false);
            }
            else
            {
                sdbusplus::xyz::openbmc_project::Inventory::server::Item::
                    present(true);
            }
        }
        else
        {
            std::cerr << "Invalid read getCPLDRegsInfo\n";
        }
        // Start read for next status
        monitor();
    });
}
