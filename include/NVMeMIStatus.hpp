#pragma once

#include <Utils.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Drive/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/server.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using StatusInterface = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Inventory::server::Item,
    sdbusplus::xyz::openbmc_project::State::Decorator::server::
        OperationalStatus,
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;

using DriveInterface =
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Drive;

class NVMeMIStatus :
    public StatusInterface,
    public std::enable_shared_from_this<NVMeMIStatus>
{
  public:
    NVMeMIStatus(sdbusplus::asio::object_server& objectServer,
                 std::shared_ptr<sdbusplus::asio::connection>& conn,
                 boost::asio::io_service& io, const std::string& sensorName,
                 const std::string& sensorConfiguration, unsigned int pollRate,
                 uint8_t busId, uint8_t nvmeAddress);
    ~NVMeMIStatus() override;

    void monitor(void);

    std::string name;
    unsigned int sensorPollSec;
    uint8_t busId;
    uint8_t nvmeAddress;

  private:
    std::shared_ptr<sdbusplus::asio::dbus_interface> sensorInterface;
    sdbusplus::asio::object_server& objServer;
    int getNVMeInfo(int bus, uint8_t addr, std::vector<uint8_t>& resp);
    boost::asio::steady_timer waitTimer;
};
