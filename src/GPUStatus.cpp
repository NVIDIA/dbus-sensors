#include <unistd.h>

#include "GPUStatus.hpp"
#include <boost/asio/read_until.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <string>
#include <vector>

GPUStatus::GPUStatus(sdbusplus::asio::object_server& objectServer,
                     std::shared_ptr<sdbusplus::asio::connection>& conn,
                     const std::string& sensorName,
                     const std::string& gpuService,
                     const std::string& gpuObject,
                     const std::string& gpuInterface,
                     const std::string& gpuProperty, int totalGPU,
                     const std::string& sensorConfiguration) :
    AssocInterface(
        static_cast<sdbusplus::bus::bus&>(*conn),
        ("/xyz/openbmc_project/sensors/GPU/" + escapeName(sensorName)).c_str(),
        AssocInterface::action::defer_emit),
    std::enable_shared_from_this<GPUStatus>(), name(sensorName),
    totalGPU(totalGPU), objServer(objectServer)
{
    sensorInterface = objectServer.add_interface(
        ("/xyz/openbmc_project/sensors/GPU/" + escapeName(sensorName)).c_str(),
        "xyz.openbmc_project.Inventory.Item.GPU");

    fs::path p(sensorConfiguration);
    AssociationList assocs = {};
    assocs.emplace_back(
        std::make_tuple("chassis", "all_sensors", p.parent_path().string()));
    sdbusplus::xyz::openbmc_project::Association::server::Definitions::
        associations(assocs);

    /* Get initial status by reading all GPU Reset status */
    for (int i = 1; i <= totalGPU; i++)
    {
        std::variant<bool> value;
        std::string objPath = gpuObject + std::to_string(i);
        auto method = static_cast<sdbusplus::bus::bus&>(*conn).new_method_call(
            gpuService.c_str(), objPath.c_str(),
            "org.freedesktop.DBus.Properties", "Get");

        method.append(gpuInterface, gpuProperty);
        try
        {
            auto reply = static_cast<sdbusplus::bus::bus&>(*conn).call(method);
            reply.read(value);
        }
        catch (const sdbusplus::exception_t&)
        {
            std::cerr << "error getting resetRequired status";
        }

        std::string propName = "GPU" + std::to_string(i);
        gpuStatus[propName] = std::get<bool>(value);
    }

    sensorInterface->register_property(
        "GPUResetReq", gpuStatus,
        [&](const std::map<std::string, bool>& newStatus,
            std::map<std::string, bool>& oldStatus) {
            oldStatus = newStatus;
            gpuStatus = newStatus;
            return 1;
        });

    if (!sensorInterface->initialize())
    {
        std::cerr << "error initializing value interface\n";
    }

    auto gpuEventMatcherCallback = [this,
                                    conn](sdbusplus::message::message& msg) {
        bool resetRequired;
        try
        {
            msg.read(resetRequired);
        }
        catch (const sdbusplus::exception_t&)
        {
            std::cerr << "error getting resetRequired data from "
                      << msg.get_path() << "\n";
            return;
        }
        std::string path = msg.get_path();

        uint8_t gpuIndex = 1;
        std::size_t indexLast = path.size();
        std::size_t indexFirst = path.find_last_not_of("0123456789");
        if (indexFirst == std::string::npos)
        {
            return;
        }
        indexFirst++;
        if (indexFirst != indexLast)
        {
            gpuIndex = std::stoi(path.substr(indexFirst, indexLast));
        }
        std::string gpuName = "GPU" + std::to_string(gpuIndex);
        gpuStatus[gpuName] = resetRequired;
    };

    std::size_t indexLast = gpuObject.find_last_of("/");
    if (indexLast == std::string::npos)
    {
        return;
    }
    std::string gpuBase = gpuObject.substr(0, indexLast);

    gpuEventMatcher = std::make_shared<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*conn),
        "type='signal',interface='org.freedesktop.DBus.Properties',member='"
        "PropertiesChanged',"
        "path_namespace='" +
            gpuBase + "',arg0namespace='" + gpuInterface + "'",
        std::move(gpuEventMatcherCallback));
}

GPUStatus::~GPUStatus()
{
    objServer.remove_interface(sensorInterface);
}
