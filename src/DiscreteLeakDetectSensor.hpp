#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "Utils.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>
#include <xyz/openbmc_project/State/LeakDetectorState/server.hpp>

using LeakDetectIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::server::LeakDetectorState>;

class DiscreteLeakDetectSensor :
    public std::enable_shared_from_this<DiscreteLeakDetectSensor>
{
  public:
    DiscreteLeakDetectSensor(sdbusplus::bus::bus& bus,
                sdbusplus::asio::object_server& objectServer,
                std::shared_ptr<sdbusplus::asio::connection>& conn,
                boost::asio::io_context& io,
                const std::string& sensorType,
                const std::string& sensorSysfsPath,
                const std::string& sensorName,
                const std::string& sensorConfiguration, const float pollRate,
                const uint8_t busId, const uint8_t address,
                const std::string& driver);
    ~DiscreteLeakDetectSensor();

    void monitor(void);

    std::string sensorType;
    std::string sysfsPath;
    std::string name;
    unsigned int sensorPollMs;
    uint8_t busId;
    uint8_t address;
    std::string driver;

  private:
    int getLeakInfo();
    int readLeakValue(const std::string& filePath);
    void createLeakageLogEntry(const std::string& messageID,
        const std::string& arg0, const std::string& arg1,
        const std::string& resolution,
        const std::string logNamespace = "DiscreteLeakDetectSensor");
    void setLeakageLabels();

    std::shared_ptr<sdbusplus::asio::dbus_interface> sensorInterface;
    sdbusplus::asio::object_server& objServer;
    boost::asio::steady_timer waitTimer;
    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;
    std::unique_ptr<LeakDetectIntf> leakDetectIntf = nullptr;
};