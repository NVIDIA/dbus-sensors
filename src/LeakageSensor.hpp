#pragma once

#include "Utils.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

class LeakageSensor :
    public std::enable_shared_from_this<LeakageSensor>
{
  public:
    LeakageSensor(sdbusplus::asio::object_server& objectServer,
                 std::shared_ptr<sdbusplus::asio::connection>& conn,
                 boost::asio::io_context& io, const std::string& sensorName,
                 const std::string& sensorConfiguration, const float pollRate,
                 const uint8_t busId, const uint8_t address,bool polling,
                 const std::string& driver);
    ~LeakageSensor();

    void monitor(void);

    std::string name;
    unsigned int sensorPollMs;
    uint8_t busId;
    uint8_t address;
    bool polling;
    std::string driver;	

  private:
    void registerProperties();
    int getLeakInfo(int bus, uint8_t addr, std::vector<uint8_t>& resp);
    int readLeakValue(const std::string& filePath);
    void createLeakageLogEntry(const std::string& messageID,
		const std::string& arg0, const std::string& arg1,
        const std::string& resolution,
        const std::string logNamespace = "LeakageSensor");
	void findMatchingPaths(const std::string& base_path,
		const std::string& dir_pattern, const std::string& file_pattern);
	void setLeakageLabels();

    std::shared_ptr<sdbusplus::asio::dbus_interface> sensorInterface;
    sdbusplus::asio::object_server& objServer;
    boost::asio::steady_timer waitTimer;
    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;
	std::vector<std::pair<std::string, std::string>> matching_paths;
};
