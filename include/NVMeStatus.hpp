#pragma once

#include <Utils.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Drive/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/server.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using ItemInterface = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Inventory::server::Item,
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;

using DriveInterface =
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Drive;

class NVMeStatus :
    public ItemInterface,
    public std::enable_shared_from_this<NVMeStatus>
{
  public:
    NVMeStatus(sdbusplus::asio::object_server& objectServer,
               std::shared_ptr<sdbusplus::asio::connection>& conn,
               boost::asio::io_service& io, const std::string& sensorName,
               const std::string& sensorConfiguration, unsigned int pollRate,
               uint8_t index, uint8_t busId, uint8_t cpldAddress,
               uint8_t statusReg);
    ~NVMeStatus() override;

    void monitor(void);

    std::string name;
    unsigned int sensorPollSec;
    uint8_t index;
    uint8_t busId;
    uint8_t cpldAddress;
    uint8_t statusReg;

  private:
    std::shared_ptr<sdbusplus::asio::dbus_interface> sensorInterface;
    sdbusplus::asio::object_server& objServer;
    int getCPLDRegsInfo(uint8_t regs, int16_t* pu16data);
    boost::asio::deadline_timer waitTimer;
};
