#pragma once

#include "NVMeContext.hpp"

#include <boost/asio/io_service.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>

#define NVME_MI_BASIC_STATUS_LEN 6
#define NVME_MI_BASIC_CTEMP_TEMP_SENSOR_FAILURE 0x81

class NVMeBasicContext : public NVMeContext
{
  public:
    NVMeBasicContext(boost::asio::io_service& io, int rootBus);
    ~NVMeBasicContext() override = default;
    void pollNVMeDevices() override;
    void readAndProcessNVMeSensor() override;
    void processResponse(void* msg, size_t len) override;

  private:
    NVMeBasicContext(boost::asio::io_service& io, int rootBus, int cmdOut,
                     int streamIn, int streamOut, int cmdIn);
    boost::asio::io_service& io;
    boost::asio::posix::stream_descriptor reqStream;
    boost::asio::posix::stream_descriptor respStream;
};
