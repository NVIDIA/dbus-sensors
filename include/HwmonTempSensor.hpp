#pragma once

#include <Thresholds.hpp>
#include <boost/asio/streambuf.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sensor.hpp>
#include "sharedMemUtils.hpp"

#include <string>
#include <vector>
#include <variant>



struct SensorParams
{
    double minValue;
    double maxValue;
    double offsetValue;
    double scaleValue;
    std::string units;
    std::string typeName;
    std::string platform;
    std::string inventoryChassis;
};

class HwmonTempSensor :
    public Sensor,
    public std::enable_shared_from_this<HwmonTempSensor>
{
  public:
    HwmonTempSensor(const std::string& path, const std::string& objectType,
                    sdbusplus::asio::object_server& objectServer,
                    std::shared_ptr<sdbusplus::asio::connection>& conn,
                    boost::asio::io_service& io, const std::string& sensorName,
                    std::vector<thresholds::Threshold>&& thresholds,
                    const struct SensorParams& thisSensorParameters,
                    const float pollRate,
                    const std::string& sensorConfiguration,
                    const PowerState powerState,
                    const std::string& sensorPhysicalContext);
    ~HwmonTempSensor() override;
    void setupRead(void);

  private:
    sdbusplus::asio::object_server& objServer;
    boost::asio::posix::stream_descriptor inputDev;
    boost::asio::steady_timer waitTimer;
    boost::asio::streambuf readBuf;
    std::string path;
    double offsetValue;
    double scaleValue;
    unsigned int sensorPollMs;

    std::string platform;
    std::string inventoryChassis;
    std::shared_ptr<sdbusplus::asio::dbus_interface> sensorMetricIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> areaIface;
    std::string physicalContext;

    void handleResponse(const boost::system::error_code& err);
    void restartRead();
    void checkThresholds(void) override;
};
