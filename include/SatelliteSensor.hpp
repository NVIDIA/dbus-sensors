#pragma once
#include <boost/asio/steady_timer.hpp>
#include <boost/container/flat_map.hpp>
#include <sensor.hpp>

#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <HmcSensor.hpp>

template <typename T>
    int i2cCmd(uint8_t bus, uint8_t addr, size_t offset, T* reading,
               int length);

struct SatelliteSensor : public Sensor
{
    SatelliteSensor(std::shared_ptr<sdbusplus::asio::connection>& conn,
                    boost::asio::io_service& io, const std::string& name,
                    const std::string& sensorConfiguration,
                    sdbusplus::asio::object_server& objectServer,
                    std::vector<thresholds::Threshold>&& thresholds,
                    uint8_t busId, uint8_t addr, uint16_t offset,
                    std::string& sensorType, size_t pollTime, double minVal, double maxVal);
    ~SatelliteSensor() override;

    void checkThresholds(void) override; 
    size_t getPollRate()
    {
      return pollRate;
    }
    void read(void);
    void init(void);

    uint8_t busId;
    uint8_t addr;
    uint16_t offset;


  private:
    int readEepromData(size_t off, uint8_t length ,double* data);
    int getPLDMSensorReading(size_t off, uint8_t length, double* data);
    uint8_t getLength(uint16_t offset)
    {
      auto it = sensorMap.find(offset);
      // offset is not in the map.
      if(it == sensorMap.end())
      {
        return 0;
      }
      return sensorMap[offset];
    }
    sdbusplus::asio::object_server& objectServer;
    boost::asio::steady_timer waitTimer;
    size_t pollRate;
};
