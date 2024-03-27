#pragma once
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/container/flat_map.hpp>
#include <sensor.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#ifdef AUTO_GEN_SENSOR_HEADER
#include <HmcSensor.hpp>
#endif

template <typename T>
    int i2cCmd(uint8_t bus, uint8_t addr, size_t offset, T* reading,
               int length);

struct SatelliteSensor : public Sensor
{
    SatelliteSensor(std::shared_ptr<sdbusplus::asio::connection>& conn,
                    boost::asio::io_context& io, const std::string& name,
                    const std::string& sensorConfiguration,
                    sdbusplus::asio::object_server& objectServer,
                    std::vector<thresholds::Threshold>&& thresholdData,
                    uint8_t busId, uint8_t addr, uint16_t offset,
                    std::string& sensorType, size_t pollTime, double minVal,
                    double maxVal);
    ~SatelliteSensor() override;

    void checkThresholds(void) override;
    size_t getPollRate() const
    {
      return pollRate;
    }
    void read(void);
    void init(void);

    uint8_t busId;
    uint8_t addr;
    uint16_t offset;


  private:
    int readEepromData(size_t off, uint8_t length, double* data) const;
    int getPLDMSensorReading(size_t off, uint8_t length, double* data) const;
    static uint8_t getLength(uint16_t offset)
    {
#ifdef AUTO_GEN_SENSOR_HEADER
        auto it = sensorMap.find(offset);
        // offset is not in the map.
        if (it == sensorMap.end())
        {
            return 0;
        }
        return sensorMap[offset];
#else 
      // return offset to avoid the unused variable error.
      return offset; 
#endif
    }
    sdbusplus::asio::object_server& objectServer;
    boost::asio::steady_timer waitTimer;
    size_t pollRate;
};
