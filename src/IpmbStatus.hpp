#pragma once
#include "sensor.hpp"

#include <boost/asio/steady_timer.hpp>
#include <boost/container/flat_map.hpp>

#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class IpmbType
{
    none,
    meSensor
};

namespace ipmi
{
namespace sensor
{
constexpr uint8_t netFn = 0x04;
constexpr uint8_t getSensorReading = 0x2d;

} // namespace sensor
} // namespace ipmi
namespace fs = std::filesystem;

struct IpmbSensor
{
    IpmbSensor(std::shared_ptr<sdbusplus::asio::connection>& conn,
               boost::asio::io_context& io, const std::string& name,
               const std::string& sensorConfiguration,
               sdbusplus::asio::object_server& objectServer,
               uint8_t deviceAddress, uint8_t channelAddress, float pollRate);
    ~IpmbSensor();

    void read();
    void init();
    void loadDefaults();
    void runInitCmd();
    bool processReading(const std::vector<uint8_t>& data, double& resp) const;

    IpmbType type = IpmbType::none;
    uint8_t commandAddress = 0;
    uint8_t netfn = 0;
    uint8_t command = 0;
    uint8_t deviceAddress = 0;
    uint8_t channelAddress = 0;
    std::vector<uint8_t> commandData;
    std::optional<uint8_t> initCommand;
    std::vector<uint8_t> initData;
    int sensorPollMs;
    bool sensorReport = false;
    bool sensorMaskEnable = false;
    std::string statusSensorName;

  private:
    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;
    std::shared_ptr<sdbusplus::asio::dbus_interface> sensorInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> association;
    sdbusplus::asio::object_server& objectServer;
    boost::asio::steady_timer waitTimer;
};
