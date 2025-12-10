#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <gpiod.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <cstddef>
#include <memory>
#include <string>

class ChassisIntrusionSensor
{
  public:
    explicit ChassisIntrusionSensor(
        bool autoRearm, sdbusplus::asio::object_server& objServer,
        const std::shared_ptr<sdbusplus::asio::connection>& conn = nullptr,
        std::string sensorName = "");

    virtual ~ChassisIntrusionSensor();

    void start();

  protected:
    virtual int readSensor() = 0;
    virtual void pollSensorStatus() = 0;
    void updateValue(const size_t& value);
    sdbusplus::asio::object_server& getObjectServer()
    {
        return mObjServer;
    }
    virtual uint64_t getIntrusionTimestamp() const
    {
        return 0;
    }
    virtual bool hasTimestampSupport() const
    {
        return false;
    }
    virtual void onEventTrigger() {}

  private:
    enum class IntrusionEvent
    {
        Assert,
        Deassert
    };
    std::string mValue;
    // If this sensor uses automatic rearm method. Otherwise, manually rearm it
    bool mAutoRearm;
    std::shared_ptr<sdbusplus::asio::dbus_interface> mIface;
    sdbusplus::asio::object_server& mObjServer;
    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;
    std::string mSensorName;
    bool mOverridenState = false;
    bool mInternalSet = false;
    bool mRearmFlag = false;

    int setSensorValue(const std::string& req, std::string& propertyValue);
    void sendEventToLoggingService(IntrusionEvent event,
                                   const std::string& timestamp = "");
    static void sendAssertEventToJournal(bool hasTimestamp,
                                         const std::string& timestamp);
    static void sendDeassertEventToJournal();
};

class ChassisIntrusionPchSensor :
    public ChassisIntrusionSensor,
    public std::enable_shared_from_this<ChassisIntrusionPchSensor>
{
  public:
    ChassisIntrusionPchSensor(bool autoRearm, boost::asio::io_context& io,
                              sdbusplus::asio::object_server& objServer,
                              int busId, int slaveAddr);

    ~ChassisIntrusionPchSensor() override;

  private:
    int mBusFd{-1};
    int mSlaveAddr{-1};
    boost::asio::steady_timer mPollTimer;
    int readSensor() override;
    void pollSensorStatus() override;
};

class ChassisIntrusionGpioSensor :
    public ChassisIntrusionSensor,
    public std::enable_shared_from_this<ChassisIntrusionGpioSensor>
{
  public:
    ChassisIntrusionGpioSensor(bool autoRearm, boost::asio::io_context& io,
                               sdbusplus::asio::object_server& objServer,
                               bool gpioInverted);

    ~ChassisIntrusionGpioSensor() override;

  private:
    bool mGpioInverted{false};
    std::string mPinName = "CHASSIS_INTRUSION";
    gpiod::line mGpioLine;
    boost::asio::posix::stream_descriptor mGpioFd;
    int readSensor() override;
    void pollSensorStatus() override;
};

class ChassisIntrusionHwmonSensor :
    public ChassisIntrusionSensor,
    public std::enable_shared_from_this<ChassisIntrusionHwmonSensor>
{
  public:
    ChassisIntrusionHwmonSensor(
        bool autoRearm, boost::asio::io_context& io,
        sdbusplus::asio::object_server& objServer, std::string hwmonName,
        std::string sensorName,
        const std::shared_ptr<sdbusplus::asio::connection>& conn,
        std::string expectedDeviceName = "");

    ~ChassisIntrusionHwmonSensor() override;
    static constexpr uint64_t invalidTimestamp = UINT64_MAX;

  protected:
    uint64_t getIntrusionTimestamp() const override
    {
        return readTimestampFromRtc();
    }
    bool hasTimestampSupport() const override
    {
        return mIsTimestampMode;
    }
    void onEventTrigger() override
    {
        if (!mIsTimestampMode)
        {
            return;
        }

        mTimeIface->set_property("Elapsed", readTimestampFromRtc());
    }

  private:
    std::string mHwmonName;
    std::string mHwmonPath;
    std::string mTimestampPath;
    boost::asio::steady_timer mPollTimer;
    std::shared_ptr<sdbusplus::asio::dbus_interface> mTimeIface;
    bool mIsTimestampMode = false;
    int readSensor() override;
    void pollSensorStatus() override;
    uint64_t readTimestampFromRtc() const;
};
