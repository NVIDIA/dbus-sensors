#include "DomainManager.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/async.hpp>

PHOSPHOR_LOG2_USING;

int main()
{
    constexpr auto path = "/com/nvidia/fwwriteprotect";
    constexpr auto serviceName = "com.nvidia.fwwriteprotect";
    sdbusplus::async::context ctx(sdbusplus::bus::new_system());
    sdbusplus::server::manager_t manager{ctx, path};
    sdbusplus::server::manager_t inventoryManager{
        ctx, "/xyz/openbmc_project/inventory"};
    sdbusplus::server::manager_t softwareManager{
        ctx, "/xyz/openbmc_project/software"};
    sdbusplus::server::manager_t stateManager{ctx,
                                              "/xyz/openbmc_project/state"};

    nvidia::write_protect::DomainManager domainManager(ctx);
    ctx.spawn(domainManager.start());

    ctx.request_name(serviceName);
    info("Requested name {NAME}", "NAME", serviceName);
    ctx.run();
    return 0;
}
