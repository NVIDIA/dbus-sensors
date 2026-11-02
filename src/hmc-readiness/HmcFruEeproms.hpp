#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <gpiod.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Follows the HMC ready GPIO once bmc-boot-complete.service is active: starts
// hmc-ready.target or hmc-not-ready.target with its level, as
// phosphor-gpio-monitor did, and binds the HMC-side FRU EEPROMs when it is
// high, then has FruDevice rescan their bus. They stay bound when it falls
// (an HMC reboot is not a FRU removal), as Meta's San Miguel does for the
// same EEPROMs.
class HmcFruEeproms
{
  public:
    struct Device
    {
        uint16_t bus = 0;
        std::string address; // written to new_device as listed
        unsigned addr = 0;
        std::string driver;
        std::string name;
        bool optional = false;
    };

    static std::unique_ptr<HmcFruEeproms> create(
        boost::asio::io_context& io,
        std::shared_ptr<sdbusplus::asio::connection> conn,
        const std::string& configPath);

    HmcFruEeproms(boost::asio::io_context& io,
                  std::shared_ptr<sdbusplus::asio::connection> conn,
                  const std::string& gpioName, uint16_t rescanBus,
                  std::vector<Device> devices);

  private:
    void onActiveState(const std::string& state);
    void watchGpio();
    void bindAll(int attempt);
    void startUnit(const char* unit);

    static constexpr int maxAttempts = 6;

    std::shared_ptr<sdbusplus::asio::connection> conn;
    uint16_t rescanBus;
    std::vector<Device> devices;
    gpiod::line line;
    boost::asio::posix::stream_descriptor gpioFd;
    boost::asio::steady_timer retry;
    std::unique_ptr<sdbusplus::bus::match_t> bootMatch;
    bool started = false;
};
