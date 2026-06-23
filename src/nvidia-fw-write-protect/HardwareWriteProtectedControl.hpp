#pragma once

#include "Utility.hpp"

#include <com/nvidia/State/HardwareWriteProtectedControl/aserver.hpp>
#include <sdbusplus/async.hpp>
#include <xyz/openbmc_project/Inventory/Item/Chassis/aserver.hpp>

namespace nvidia::write_protect
{

class HardwareWriteProtectedControl;
using HardwareWriteProtectedControlIntf = sdbusplus::async::server_t<
    HardwareWriteProtectedControl,
    sdbusplus::aserver::com::nvidia::state::HardwareWriteProtectedControl,
    sdbusplus::aserver::xyz::openbmc_project::inventory::item::Chassis>;

/**
 * @brief D-Bus facade for chassis-level hardware write protection.
 *
 * Exposes @c com.nvidia.State.HardwareWriteProtectedControl and
 * @c xyz.openbmc_project.Inventory.Item.Chassis on the chassis object path.
 * The @c WriteProtectedControl property is driven by a reactive graph observer.
 */
class HardwareWriteProtectedControl : public HardwareWriteProtectedControlIntf
{
  public:
    explicit HardwareWriteProtectedControl(sdbusplus::async::context& ctx,
                                           const sdbusplus::object_path& path) :
        HardwareWriteProtectedControlIntf(ctx, path.str.c_str())
    {
        emitAdded(*this);
    }
};

} // namespace nvidia::write_protect
