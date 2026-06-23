#pragma once

#include <sdbusplus/async.hpp>
#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/Software/Settings/aserver.hpp>

namespace nvidia::write_protect
{

class SoftwareComponent;
using SoftwareSettingsIntf = sdbusplus::async::server_t<
    SoftwareComponent,
    sdbusplus::aserver::xyz::openbmc_project::software::Settings>;

/**
 * @brief D-Bus object exposing @c xyz.openbmc_project.Software.Settings.
 *
 * Its @c WriteProtected property is driven by a reactive graph observer.
 * Properties are read-only from D-Bus; only the internal reactive graph
 * may update them.
 */
class SoftwareComponent : public SoftwareSettingsIntf
{
  public:
    /**
     * @brief Construct a SoftwareComponent and register it on D-Bus.
     * @param ctx  The sdbusplus async context.
     * @param path D-Bus object path (e.g. /xyz/openbmc_project/software/NAME).
     */
    explicit SoftwareComponent(sdbusplus::async::context& ctx,
                               const sdbusplus::object_path& path) :
        SoftwareSettingsIntf(ctx, path.str.c_str())
    {
        emit_added();
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    bool set_property(write_protected_t /*tag*/, sdbusplus::message_t& /*msg*/,
                      bool /*value*/)
    {
        throw sdbusplus::error::xyz::openbmc_project::common::NotAllowed();
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    bool set_property(write_protected_t /*tag*/, bool newValue)
    {
        bool changed = (newValue != write_protected_);
        write_protected_ = newValue;
        return changed;
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    bool set_property(write_protected_control_t /*tag*/,
                      sdbusplus::message_t& /*msg*/, bool /*value*/)
    {
        throw sdbusplus::error::xyz::openbmc_project::common::NotAllowed();
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    bool set_property(write_protected_control_t /*tag*/, bool newValue)
    {
        bool changed = (newValue != write_protected_control_);
        write_protected_control_ = newValue;
        return changed;
    }
};

} // namespace nvidia::write_protect
