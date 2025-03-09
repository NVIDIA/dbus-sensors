#include "mctp_discovery.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <exception>

int main()
{
    auto bus = sdbusplus::bus::new_default();

    try
    {
        gpuserver::MctpDiscovery discovery(bus, "/run/gpuserverd.sock");

        // Process D-Bus messages
        while (true)
        {
            bus.process_discard();
            bus.wait();
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to start gpuserver-mctp-discovery: {ERROR}", "ERROR",
                   e.what());
        return -1;
    }

    return 0;
}
