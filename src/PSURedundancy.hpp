#pragma once

#include "Utils.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

constexpr auto OPERATIONAL_STATE_IFACE =
    "xyz.openbmc_project.State.Decorator.OperationalStatus";
constexpr auto PSU_SERVICE = "com.Nvidia.Powersupply";
constexpr auto PSU_OBJ =
    "/xyz/openbmc_project/inventory/system/chassis/motherboard/powersupply";
constexpr auto PSU_BASE_OBJ =
    "/xyz/openbmc_project/inventory/system/chassis/motherboard";

namespace fs = std::filesystem;

using AssocInterface = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;

class PSURedundancy :
    public AssocInterface,
    public std::enable_shared_from_this<PSURedundancy>
{
  public:
    PSURedundancy(sdbusplus::asio::object_server& objectServer,
                  std::shared_ptr<sdbusplus::asio::connection>& conn,
                  const std::string& sensorName, int totalPSUCount,
                  int redundantPSUCount, int sufficientPSUCount,
                  const std::string& sensorConfiguration);
    ~PSURedundancy() override;

    std::string name;
    std::string status;
    bool redundancyLost = false;
    int totalPSU;
    int redundantPSU;
    int workablePSU;
    int previousWorkablePSU;
    int sufficientPSU;

  private:
    std::shared_ptr<sdbusplus::asio::dbus_interface> sensorInterface;
    sdbusplus::asio::object_server& objServer;
    std::shared_ptr<sdbusplus::bus::match::match> psuEventMatcher;
    void setStatus();
};
