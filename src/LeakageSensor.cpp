#include <unistd.h>

#include "LeakageSensor.hpp"
#include <boost/asio/read_until.hpp>


#include <cerrno>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

/* CPLD definitions
1 - no event (leakage not detected)
0 - leakage event (leakage detected)
*/

namespace fs = std::filesystem;

const std::string compName{"Leakage Sensor"};
const std::string messageError{"The resource property Leakage Sensor has detected errors of type 'Leakage'."};
const std::string resolution{"Inspect for water leakage and consider power down switch tray."};
const std::string resourceErrorDetected{"ResourceEvent.1.0.ResourceErrorsDetected"};

LeakageSensor::LeakageSensor(sdbusplus::asio::object_server& objectServer,
                           std::shared_ptr<sdbusplus::asio::connection>& conn,
                           boost::asio::io_context& io,
                           const std::string& sensorName,
                           [[maybe_unused]]const std::string& sensorConfiguration,
                           float pollRate, uint8_t busId,uint8_t address,
                           bool polling, const std::string& driver):
    name(sensorName),
    sensorPollMs(static_cast<unsigned int>(pollRate * 1000)),
    busId(busId),
    address(address),
    polling(polling),
    driver(driver),
    objServer(objectServer), 
    waitTimer(io),
    dbusConnection(conn)
{
    sensorInterface = objectServer.add_interface(
        ("/xyz/openbmc_project/sensors/leakage/" + escapeName(sensorName)),
        "xyz.openbmc_project.LeakageSensorInterface");

     registerProperties();

    if (!sensorInterface->initialize())
    {
        std::cerr << "error initializing interface\n";
    }
    monitor();
}

LeakageSensor::~LeakageSensor()
{
    waitTimer.cancel();
    objServer.remove_interface(sensorInterface);
}

void LeakageSensor::findMatchingPaths(const std::string& base_path, const std::string& dir_pattern, const std::string& file_pattern)
{
    for (const auto& entry : fs::recursive_directory_iterator(base_path)) {
        if (entry.is_directory() && entry.path().filename().string().find(dir_pattern) != std::string::npos) {
            for (const auto& sub_entry : fs::directory_iterator(entry.path())) {
                if (sub_entry.path().filename().string().find(file_pattern) == 0) {
                    matching_paths.emplace_back(entry.path().string(), sub_entry.path().filename().string());
                }
            }
        }
    }
}

void LeakageSensor::registerProperties()
{
    std::string base_path = "/sys/bus/i2c/devices/i2c-" + std::to_string(busId) + "/" + 
                            std::to_string(busId) + "-00" + std::to_string(address) + "/" +
                            driver + "/hwmon";
    std::string dir_pattern = "hwmon";
    std::string file_pattern = "leakage";

    findMatchingPaths(base_path, dir_pattern, file_pattern);

    if (!matching_paths.empty()) {
        std::cout << "Found matching sysfs paths:" << std::endl;
        for (const auto& [dir, file] : matching_paths) {
            std::cout << "Directory: " << dir << ", File: " << file << std::endl;
            sensorInterface->register_property(file,static_cast<int>(1));
        }
    } else {
        std::cout << "No matching sysfs paths found." << std::endl;
    }
}

int LeakageSensor::readLeakValue(const std::string& filePath) {
    std::ifstream file(filePath);
    int value = 1;
    if (file.is_open()) {
        file >> value;
        file.close();
    }
    return value;
}

int LeakageSensor::getLeakInfo([[maybe_unused]]int bus, [[maybe_unused]]uint8_t addr, [[maybe_unused]]std::vector<uint8_t>& resp)
{
    std::vector<std::pair<std::string,int>> leakVec;
    std::string leakMsg = "";

    for (const auto& [dir, file] : matching_paths) {
        auto leakVal = readLeakValue(dir + "/" + file);
        leakVec.push_back(std::make_pair(file,leakVal));
        sensorInterface->set_property(file,leakVal);
    }

    for (const auto& leak : leakVec)
    {
        if(!leak.second)
        {
            leakMsg += leak.first;
            leakMsg += " ";
        }
    }

    if(!leakMsg.empty())
    {
        createLeakageLogEntry(resourceErrorDetected,
                            leakMsg, 
                            "Leakage Detected",
                            resolution);
    }

    return 0;
}

void LeakageSensor::monitor(void)
{
    waitTimer.expires_after(std::chrono::milliseconds(sensorPollMs));
    waitTimer.async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            std::cerr << "Read operation aborted\n";
            return; // we're being cancelled
        }
        // read timer error
        if (ec)
        {
            std::cerr << "timer error\n";
            return;
        }
        std::vector<uint8_t> resp{};
        int ret = getLeakInfo(busId, address, resp);
        if (ret < 0)
        {
            std::cerr << "LeakageSensor::getLeakInfo error";
            std::cerr << "\n";
        }
       
        // Start read for next leakage status
        monitor();
    });
}

inline void LeakageSensor::createLeakageLogEntry(const std::string& messageID,
                           const std::string& arg0, const std::string& arg1,
                           const std::string& resolution,
                           const std::string logNamespace)
{
    using namespace sdbusplus::xyz::openbmc_project::Logging::server;
    using Level =
        sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

    std::map<std::string, std::string> addData;
    addData["REDFISH_MESSAGE_ID"] = messageID;
    Level level = Level::Critical;

    if (messageID == resourceErrorDetected)
    {
        addData["REDFISH_MESSAGE_ARGS"] = (arg0 + "," + arg1);
        level = Level::Critical;
    }
    else
    {
        lg2::error("Message Registry messageID is not recognised", "MESSAGEID",
                   messageID);
        return;
    }

    if (!resolution.empty())
    {
        addData["xyz.openbmc_project.Logging.Entry.Resolution"] = resolution;
    }

    if (!logNamespace.empty())
    {
        addData["namespace"] = logNamespace;
    }

    auto severity =
        sdbusplus::xyz::openbmc_project::Logging::server::convertForMessage(
            level);
    dbusConnection->async_method_call(
        [](boost::system::error_code ec) {
            if (ec)
            {
                lg2::error("error while logging message registry: ",
                           "ERROR_MESSAGE", ec.message());
                return;
            }
        },
        "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
        "xyz.openbmc_project.Logging.Create", "Create", messageID, severity,
        addData);
    return;
}