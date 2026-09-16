// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
#include "LpuPowerModeService.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/exception.hpp>

#include <cerrno>
#include <tuple>
#include <vector>

namespace lpu::power
{
namespace
{
[[noreturn]] void dbusError(const Error& error)
{
    lg2::error("LPU power-mode operation failed: {ERROR}", "ERROR",
               error.what());
    int code = EAGAIN;
    if (error.failure == Failure::invalidArgument)
    {
        code = EINVAL;
    }
    else if (error.failure == Failure::writeFailure)
    {
        code = EIO;
    }
    throw sdbusplus::exception::SdBusError(code, error.what());
}
} // namespace

Service::Service(sdbusplus::asio::object_server& server) : server(server)
{
    constexpr auto path = "/xyz/openbmc_project/control/power/TotalLPU_Power_0";
    server.add_manager("/xyz/openbmc_project/control");
    modeInterface = server.add_interface(path, "com.nvidia.LPUPowerMode");
    // Methods use live reads and synchronous verification, not a settings
    // cache. Both execute on the service's single io_context, serializing
    // requests with telemetry polling.
    // Return the live selector and its catalogue in one method reply. The
    // mapping is static platform data, not telemetry from unused 0x1B/0x1C.
    // Missing configuration must not disable the independent mode setter.
    modeInterface->register_method(
        "GetPowerProfile", [this]() -> ProfileSnapshot {
            try
            {
                std::string mode = controller.get();
                if (!catalog)
                {
                    return {mode, {}, "Unavailable"};
                }
                return {mode, catalog->profiles, catalog->status};
            }
            catch (const Error& error)
            {
                dbusError(error);
            }
        });
    modeInterface->register_method(
        "SetPowerMode", [this](const std::string& mode) {
            try
            {
                controller.set(mode);
            }
            catch (const Error& error)
            {
                dbusError(error);
            }
        });
    modeInterface->initialize();
    associationInterface = server.add_interface(
        path, "xyz.openbmc_project.Association.Definitions");
    using Association = std::tuple<std::string, std::string, std::string>;
    associationInterface->register_property(
        "Associations",
        std::vector<Association>{
            {"chassis", "power_controls",
             "/xyz/openbmc_project/inventory/system/chassis/HGX_Chassis_0"}});
    associationInterface->initialize();
}

void Service::updateProfileCatalog(const ManagedObjectType& objects)
{
    catalog = ProfileCatalog::fromManagedObjects(objects);
    if (!catalog)
    {
        lg2::error(
            "LPU power-profile Entity Manager configuration missing or invalid; watts unavailable");
        return;
    }
    if (catalog->status == "Tentative")
    {
        lg2::warning(
            "LPU power-profile watts are tentative, not final guaranteed limits");
    }
}

Service::~Service()
{
    server.remove_interface(associationInterface);
    server.remove_interface(modeInterface);
}
} // namespace lpu::power
