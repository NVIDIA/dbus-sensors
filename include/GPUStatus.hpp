#pragma once

#include <Utils.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using AssocInterface = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;

class GPUStatus :
    public AssocInterface,
    public std::enable_shared_from_this<GPUStatus>
{
  public:
    GPUStatus(sdbusplus::asio::object_server& objectServer,
              std::shared_ptr<sdbusplus::asio::connection>& conn,
              const std::string& sensorName, const std::string& gpuService,
              const std::string& gpuObject, const std::string& gpuInterface,
              const std::string& gpuProperty, int totalGPU,
              const std::string& sensorConfiguration);
    ~GPUStatus() override;

    std::string name;
    int totalGPU;
    std::map<std::string, bool> gpuStatus;

  private:
    std::shared_ptr<sdbusplus::asio::dbus_interface> sensorInterface;
    sdbusplus::asio::object_server& objServer;
    std::shared_ptr<sdbusplus::bus::match::match> gpuEventMatcher;
};
