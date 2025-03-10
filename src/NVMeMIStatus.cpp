#include "NVMeMIStatus.hpp"

#include "../src/Utils.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/server.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/common.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

extern "C"
{
#include <i2c/smbus.h>
#include <linux/i2c-dev.h>
}

// NVMe Status command
const static constexpr size_t nvmeStatusCmd = 0x00;

// Drive Fault bit masks
const static constexpr size_t nvmeDriveFaultMask = 0x20;
const static constexpr size_t nvmeDriveFailureStatus = 0x00;

NVMeMIStatus::NVMeMIStatus(sdbusplus::asio::object_server& objectServer,
                           std::shared_ptr<sdbusplus::asio::connection>& conn,
                           boost::asio::io_context& io,
                           const std::string& sensorName,
                           const std::string& sensorConfiguration,
                           unsigned int pollRate, uint8_t busId,
                           uint8_t nvmeAddress) :
    StatusInterface(
        static_cast<sdbusplus::bus::bus&>(*conn),
        ("/xyz/openbmc_project/sensors/drive/" + escapeName(sensorName))
            .c_str(),
        StatusInterface::action::defer_emit),
    name(sensorName), sensorPollSec(pollRate), busId(busId),
    nvmeAddress(nvmeAddress), objServer(objectServer), waitTimer(io)
{
    sensorInterface = objectServer.add_interface(
        ("/xyz/openbmc_project/sensors/drive/" + escapeName(sensorName)),
        DriveInterface::interface);

    fs::path p(sensorConfiguration);
    AssociationList assocs = {};
    assocs.emplace_back(
        std::make_tuple("chassis", "all_sensors", p.parent_path().string()));
    sdbusplus::xyz::openbmc_project::Association::server::Definitions::
        associations(assocs);
    if (!sensorInterface->initialize())
    {
        std::cerr << "error initializing interface\n";
    }
    monitor();
}

NVMeMIStatus::~NVMeMIStatus()
{
    waitTimer.cancel();
    objServer.remove_interface(sensorInterface);
}

int NVMeMIStatus::getNVMeInfo(int bus, uint8_t addr, std::vector<uint8_t>& resp)
{
    int32_t size = 0;
    int32_t statusCmd = nvmeStatusCmd;

    std::string i2cBus = "/dev/i2c-" + std::to_string(bus);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    int dev = open(i2cBus.c_str(), O_RDWR);
    if (dev < 0)
    {
        std::cerr << " unable to open i2c device " << i2cBus << ": "
                  << strerror(errno) << "\n";
        return -1;
    }

    /* Select the target device */
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (::ioctl(dev, I2C_SLAVE, addr) == -1)
    {
        std::cerr << "Failed to configure device address 0x" << std::hex
                  << (int)addr << " for bus " << std::dec << bus << ": "
                  << strerror(errno) << "\n";
        close(dev);
        return -1;
    }

    /* Issue the NVMe MI basic command */
    resp.reserve(UINT8_MAX + 1);
    // Read NVM Subsystem Health Status data from NVMe M2 drive
    // If command success, M2 drive present
    // If 5th bit of data 1 is not set, drive fault occured
    // If all bits of data 2 is not set, Predictive failure occured
    size = i2c_smbus_read_block_data(dev, statusCmd, resp.data());
    if (size < 0)
    {
        // Ignore the error message when the drive is not present.
        if (errno == ENXIO)
        {
            close(dev);
            return -1;
        }
        std::cerr << "Failed to read block data from device 0x" << std::hex
                  << (int)addr << " on bus " << std::dec << bus << ": "
                  << strerror(errno) << "\n";
        close(dev);
        return -1;
    }

    close(dev);

    return 0;
}

void NVMeMIStatus::monitor()
{
    waitTimer.expires_after(std::chrono::seconds(sensorPollSec));
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
        std::vector<uint8_t> resp{};
        int32_t statusMask = nvmeDriveFaultMask;
        int32_t statusFailure = nvmeDriveFailureStatus;
        int ret = getNVMeInfo(busId, nvmeAddress, resp);
        if (ret >= 0)
        {
            sdbusplus::xyz::openbmc_project::Inventory::server::Item::present(
                true);
            if ((resp[1] & statusMask) == 0)
            {
                sdbusplus::xyz::openbmc_project::State::Decorator::server::
                    OperationalStatus::state(
                        sdbusplus::xyz::openbmc_project::State::Decorator::
                            server::OperationalStatus::StateType::Fault);
            }
            else
            {
                sdbusplus::xyz::openbmc_project::State::Decorator::server::
                    OperationalStatus::state(
                        sdbusplus::xyz::openbmc_project::State::Decorator::
                            server::OperationalStatus::StateType::None);
            }
            if (resp[2] == statusFailure)
            {
                sdbusplus::xyz::openbmc_project::State::Decorator::server::
                    OperationalStatus::functional(false);
            }
            else
            {
                sdbusplus::xyz::openbmc_project::State::Decorator::server::
                    OperationalStatus::functional(true);
            }
        }
        else
        {
            // reset states to default, if drive not present
            sdbusplus::xyz::openbmc_project::Inventory::server::Item::present(
                false);
            sdbusplus::xyz::openbmc_project::State::Decorator::server::
                OperationalStatus::state(
                    sdbusplus::xyz::openbmc_project::State::Decorator::server::
                        OperationalStatus::StateType::None);
            sdbusplus::xyz::openbmc_project::State::Decorator::server::
                OperationalStatus::functional(true);
        }
        // Start read for next status
        monitor();
    });
}
