#include "Domain.hpp"

#include "DomainManager.hpp"
#include "Error.hpp"

#include <xyz/openbmc_project/Common/error.hpp>

namespace nvidia::write_protect
{

Domain::Domain(sdbusplus::async::context& ctx,
               const sdbusplus::message::object_path& path,
               DomainManager* manager, SourceNodeId settableSource,
               const sdbusplus::message::object_path& chassisPath) :
    DomainIntf(ctx, path.str.c_str()), path(path), manager(manager),
    settableSource(settableSource)
{
    if (!chassisPath.str.empty())
    {
        associations({{"chassis", "write_protect_domains", chassisPath.str}});
    }
}

auto Domain::method_call(set_write_protected_t, bool value)
    -> sdbusplus::async::task<>
{
    if (settableSource == SourceNodeId{} || !manager)
    {
        throw sdbusplus::error::xyz::openbmc_project::common::NotAllowed();
    }

    auto result = co_await manager->setWriteProtected(settableSource, value);
    if (!result)
    {
        switch (result.error())
        {
            case Error::Unsupported:
                throw sdbusplus::error::xyz::openbmc_project::common::
                    NotAllowed();
            default:
                throw sdbusplus::error::xyz::openbmc_project::common::
                    InternalFailure();
        }
    }
}

} // namespace nvidia::write_protect
