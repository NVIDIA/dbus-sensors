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

std::map<uint16_t, uint8_t> sensorMap = {
    // offset, length
    {0x19, 4},  // GPU_SXM_1_DRAM_0_Temp_0
    {0x1D, 4},  // GPU_SXM_2_DRAM_0_Temp_0
    {0x21, 4},  // GPU_SXM_3_DRAM_0_Temp_0
    {0x25, 4},  // GPU_SXM_4_DRAM_0_Temp_0
    {0x29, 4},  // GPU_SXM_5_DRAM_0_Temp_0
    {0x2D, 4},  // GPU_SXM_6_DRAM_0_Temp_0
    {0x31, 4},  // GPU_SXM_7_DRAM_0_Temp_0
    {0x35, 4},  // GPU_SXM_8_DRAM_0_Temp_0
    {0x3A, 4},  // NVSwitch_0_TEMP_0
    {0x3E, 4},  // NVSwitch_1_TEMP_0
    {0x42, 4},  // NVSwitch_2_TEMP_0
    {0x46, 4},  // NVSwitch_3_TEMP_0
    {0x4B, 4},  // GPU_SXM_1_Power_0
    {0x4F, 4},  // GPU_SXM_2_Power_0
    {0x53, 4},  // GPU_SXM_3_Power_0
    {0x57, 4},  // GPU_SXM_4_Power_0
    {0x5B, 4},  // GPU_SXM_5_Power_0
    {0x5F, 4},  // GPU_SXM_6_Power_0
    {0x63, 4},  // GPU_SXM_7_Power_0
    {0x67, 4},  // GPU_SXM_8_Power_0
    {0x6C, 4},  // GPU_SXM_1_TEMP_1
    {0x70, 4},  // GPU_SXM_2_TEMP_1
    {0x74, 4},  // GPU_SXM_3_TEMP_1
    {0x78, 4},  // GPU_SXM_4_TEMP_1
    {0x7C, 4},  // GPU_SXM_5_TEMP_1
    {0x80, 4},  // GPU_SXM_6_TEMP_1
    {0x84, 4},  // GPU_SXM_7_TEMP_1
    {0x88, 4},  // GPU_SXM_8_TEMP_1
    {0x8D, 8},  // GPU_SXM_1_Energy_0
    {0x95, 8},  // GPU_SXM_2_Energy_0
    {0x9D, 8},  // GPU_SXM_3_Energy_0
    {0xA5, 8},  // GPU_SXM_4_Energy_0
    {0xAD, 8},  // GPU_SXM_5_Energy_0
    {0xB5, 8},  // GPU_SXM_6_Energy_0
    {0xBD, 8},  // GPU_SXM_7_Energy_0
    {0xC5, 8},  // GPU_SXM_8_Energy_0
    {0xCE, 4},  // GPU_SXM_1_DRAM_0_Power_0
    {0xD2, 4},  // GPU_SXM_2DRAM_0_Power_0
    {0xD6, 4},  // GPU_SXM_3_DRAM_0_Power_0
    {0xDA, 4},  // GPU_SXM_4_DRAM_0_Power_0
    {0xDE, 4},  // GPU_SXM_5_DRAM_0_Power_0
    {0xE2, 4},  // GPU_SXM_6_DRAM_0_Power_0
    {0xE6, 4},  // GPU_SXM_7_DRAM_0_Power_0
    {0xEA, 4},  // GPU_SXM_8_DRAM_0_Power_0
    {0xEF, 4},  // Temperature_Sensor
    {0x280, 4}, // ProcessorModule_0_CPU_0_Energy_0
    {0x284, 4}, // ProcessorModule_0_CPU_0_Power_0
    {0x288, 4}, // ProcessorModule_0_Vreg_0_CpuPower_0
    {0x28C, 4}, // ProcessorModule_0_Vreg_0_SocPower_0
    {0x290, 4}, // ProcessorModule_0_CPU_0_TempAvg_0
    {0x294, 4}, // ProcessorModule_0_CPU_0_TempLimit_0
    {0x298, 4}, // ProcessorModule_0_Vreg_0_CpuVoltage_0
    {0x29C, 4}, // ProcessorModule_0_Vreg_0_SocVoltage_0
    {0x300, 4}, // ProcessorModule_1_CPU_0_Energy_0
    {0x304, 4}, // ProcessorModule_1_CPU_0_Power_0
    {0x308, 4}, // ProcessorModule_1_Vreg_0_CpuPower_0
    {0x30C, 4}, // ProcessorModule_1_Vreg_0_SocPower_0
    {0x310, 4}, // ProcessorModule_1_CPU_0_TempAvg_0
    {0x314, 4}, // ProcessorModule_1_CPU_0_TempLimit_0
    {0x318, 4}, // ProcessorModule_1_Vreg_0_CpuVoltage_0
    {0x31C, 4}, // ProcessorModule_1_Vreg_0_SocVoltage_0
    {0x380, 4}, // ProcessorModule_2_CPU_0_Energy_0
    {0x384, 4}, // ProcessorModule_2_CPU_0_Power_0
    {0x388, 4}, // ProcessorModule_2_Vreg_0_CpuPower_0
    {0x38C, 4}, // ProcessorModule_2_Vreg_0_SocPower_0
    {0x390, 4}, // ProcessorModule_2_CPU_0_TempAvg_0
    {0x394, 4}, // ProcessorModule_2_CPU_0_TempLimit_0
    {0x398, 4}, // ProcessorModule_2_Vreg_0_CpuVoltage_0
    {0x39C, 4}, // ProcessorModule_2_Vreg_0_SocVoltage_0
    {0x400, 4}, // ProcessorModule_3_CPU_0_Energy_0
    {0x404, 4}, // ProcessorModule_3_CPU_0_Power_0
    {0x408, 4}, // ProcessorModule_3_Vreg_0_CpuPower_0
    {0x40C, 4}, // ProcessorModule_3_Vreg_0_SocPower_0
    {0x410, 4}, // ProcessorModule_3_CPU_0_TempAvg_0
    {0x414, 4}, // ProcessorModule_3_CPU_0_TempLimit_0
    {0x418, 4}, // ProcessorModule_3_Vreg_0_CpuVoltage_0
    {0x41C, 4} // ProcessorModule_3_Vreg_0_SocVoltage_0
};
struct SatelliteSensor : public Sensor
{
    SatelliteSensor(std::shared_ptr<sdbusplus::asio::connection>& conn,
                    boost::asio::io_context& io, const std::string& name,
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
    int readEepromData(uint16_t regs,uint8_t length ,double* data);
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
