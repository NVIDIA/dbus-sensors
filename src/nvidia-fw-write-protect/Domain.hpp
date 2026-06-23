#pragma once

#include "ReactiveGraph.hpp"

#include <com/nvidia/Software/WriteProtection/aserver.hpp>
#include <sdbusplus/async.hpp>
#include <xyz/openbmc_project/Association/Definitions/aserver.hpp>

namespace nvidia::write_protect
{

class DomainManager;

class Domain;

using DomainIntf = sdbusplus::async::server_t<
    Domain, sdbusplus::aserver::com::nvidia::software::WriteProtection,
    sdbusplus::aserver::xyz::openbmc_project::association::Definitions>;

/**
 * @brief D-Bus facade for a write-protection domain.
 *
 * Created for @c WriteProtectDomain configs at
 * @c /xyz/openbmc_project/state/<Name>.  Exposes the
 * @c com.nvidia.Software.WriteProtection interface and
 * @c xyz.openbmc_project.Association.Definitions for chassis associations.
 *
 * The domain's first source is settable via @c SetWriteProtected, which
 * co_awaits the protector set through the DomainManager and propagates
 * errors back to the D-Bus caller.  If no settable source is configured,
 * @c SetWriteProtected throws @c NotAllowed.
 */
class Domain : public DomainIntf
{
  public:
    Domain(const Domain&) = delete;
    Domain& operator=(const Domain&) = delete;
    Domain(Domain&&) = delete;
    Domain& operator=(Domain&&) = delete;

    /**
     * @param ctx               The sdbusplus async context.
     * @param path              D-Bus object path for this domain.
     * @param manager           Back-pointer to the owning DomainManager
     *                          (used for SetWriteProtected delegation).
     * @param settableSource    Source node to set when SetWriteProtected
     *                          is called.  Default-constructed (invalid)
     *                          means the domain is read-only.
     * @param chassisPath       If non-empty, the domain creates a
     *                          @c write_protect_domains / @c chassis
     *                          association between the chassis and this
     *                          domain.
     */
    explicit Domain(sdbusplus::async::context& ctx,
                    const sdbusplus::object_path& path, DomainManager* manager,
                    SourceNodeId settableSource = {},
                    const sdbusplus::object_path& chassisPath = {});

    auto method_call(set_write_protected_t, bool wp)
        -> sdbusplus::async::task<>;

  private:
    sdbusplus::object_path path;
    DomainManager* manager;
    SourceNodeId settableSource;
};

} // namespace nvidia::write_protect
