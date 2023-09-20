#include <Utils.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <boost/asio/io_service.hpp>

constexpr const char* objectPath = "/xyz/openbmc_project/state/boot_progress/boot_progress_sensor";

int main()
{
    boost::asio::io_service io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    systemBus->request_name("xyz.openbmc_project.BootProgressSensor");

    sdbusplus::asio::object_server objectServer(systemBus, true);
    objectServer.add_manager(objectPath);

    std::shared_ptr<sdbusplus::asio::dbus_interface> iface =
        objectServer.add_interface(objectPath, "xyz.openbmc_project.State.Boot.Progress");

    std::string bootProgress;
    iface->register_property("BootProgress", bootProgress);
    iface->initialize();

    io.run();
    return 0;
}
