#pragma once

#include <unistd.h>

#include <Thresholds.hpp>
#include <boost/asio/streambuf.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sensor.hpp>

#include <string>
#include <vector>

/**
 * @class PLXTempSensor
 */

class PLXTempSensor :
    public Sensor,
    public std::enable_shared_from_this<PLXTempSensor>
{
  public:
    /**
     * @brief Construct a PLXTempSensor object
     *
     * @param sensortype
     * @param dbus oject server
     * @param dbusConnection
     * @param ioservice
     * @param sensorname
     * @param sensor Thresholds
     * @param interface path
     * @param powerState
     * @param i2c bus number
     * @param device address
     * @param pollrate
     */

    PLXTempSensor(const std::string& objectType,
                  sdbusplus::asio::object_server& objectServer,
                  std::shared_ptr<sdbusplus::asio::connection>& conn,
                  boost::asio::io_service& io, const std::string& sensorName,
                  std::vector<thresholds::Threshold>&& thresholds,
                  const std::string& sensorConfiguration,
                  const PowerState powerState, uint8_t deviceBus,
                  uint8_t deviceAddress, const float pollRate);
    ~PLXTempSensor() override;
    void setupRead(void);

  private:
    sdbusplus::asio::object_server& objServer;
    boost::asio::steady_timer waitTimer;

    uint8_t deviceBus;
    uint8_t deviceAddress;
    unsigned int sensorPollMs;

    /**
     * @brief update sensor reading based on plx registers
     */
    bool updateReading();

    /**
     * @brief initialization of plx device
     */
    void hwInit();

    /**
     * @brief perform threshold monitoring
     */
    void checkThresholds(void) override;

    /**
     * @brief polling plx regsiters
     * It will monitor the register changes
     * with an interval of 0.5 seconds.
     */
    void restartRead();

    /**
     * @brief writes up to count bytes from the buffer starting at
     *  buf to the file referred to by the file descriptor fd.
     *  @param file descriptor
     *  @param buffer
     *  @param count bytes to write
     */
    int i2cWrite(int fd, const void* buf, ssize_t len)
    {
        if (write(fd, buf, len) != len)
        {
            std::cerr << "unable to write to i2c device "
                      << std::string(std::strerror(errno)) << "\n";
            close(fd);
            return -1;
        }
        return 0;
    }
};

static constexpr unsigned int readingSignedBit = 0x8000;
static constexpr unsigned int readingAvailableBit = 0x01;
static constexpr unsigned int arrayLenWrite = 8;
static constexpr unsigned int arrayLenRead = 4;
