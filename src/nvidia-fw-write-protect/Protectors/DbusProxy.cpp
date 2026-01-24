#include "DbusProxy.hpp"

#include "AsyncSet.hpp"
#include "Error.hpp"

#include <com/nvidia/Async/Set/client.hpp>
#include <com/nvidia/Software/WriteProtection/client.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/async.hpp>
#include <sdbusplus/exception.hpp>
#include <xyz/openbmc_project/ObjectMapper/client.hpp>
#include <xyz/openbmc_project/Software/Settings/client.hpp>

#include <algorithm>
#include <exception>
#include <expected>
#include <format>
#include <stdexcept>
#include <variant>

PHOSPHOR_LOG2_USING;

namespace nvidia
{
namespace write_protect
{

using ObjectMapperIntf =
    sdbusplus::client::xyz::openbmc_project::ObjectMapper<>;
using SoftwareSettingsIntf =
    sdbusplus::client::xyz::openbmc_project::software::Settings<>;
using AsyncSetIntf = sdbusplus::client::com::nvidia::async::Set<>;
using NvidiaWriteProtectIntf =
    sdbusplus::client::com::nvidia::software::WriteProtection<>;
using PropertiesVariant = std::variant<bool>;

static const std::vector<std::string> supportedInterfaces{{
    SoftwareSettingsIntf::interface,
    AsyncSetIntf::interface,
    NvidiaWriteProtectIntf::interface,
}};

auto DbusProxy::init() -> sdbusplus::async::task<>
{
    std::string_view interface;

    switch (kind)
    {
        case Kind::SoftwareSettings:
        case Kind::AsyncSoftwareSettings:
            interface = SoftwareSettingsIntf::interface;
            break;

        case Kind::NvidiaWriteProtect:
            interface = NvidiaWriteProtectIntf::interface;
            break;

        default:
            throw std::runtime_error(std::format("Invalid protector kind: {}",
                                                 static_cast<int>(kind)));
    }

    namespace rules = sdbusplus::bus::match::rules;
    match = std::make_unique<sdbusplus::async::match>(
        *ctx, rules::propertiesChanged(path.str, interface));

    co_return;
}

auto DbusProxy::create(sdbusplus::async::context& ctx,
                       const sdbusplus::message::object_path& path)
    -> sdbusplus::async::task<std::optional<DbusProxy>>
{
    namespace rules = sdbusplus::bus::match::rules;

    // Create the match first so that we don't miss the signal if it's sent
    // slightly after we query the object mapper.
    sdbusplus::async::match protectorMatch(
        ctx, rules::interfacesAddedAtPath(path.str));

    auto detected = co_await detect(ctx, path);
    if (!detected)
    {
        auto token = co_await stdexec::get_stop_token();
        while (!detected)
        {
            if (token.stop_requested())
            {
                co_return {};
            }

            warning(
                "Underlying DBus object is not available yet - waiting for {PATH}",
                "PATH", path.str);

            co_await protectorMatch.next();
            detected = co_await detect(ctx, path);
        }
    }

    auto& [kind, service] = *detected;
    auto interface = DbusProxy(ctx, service, kind, path);
    co_await interface.init();
    co_return std::move(interface);
}

auto DbusProxy::detect(sdbusplus::async::context& ctx,
                       const sdbusplus::message::object_path& path)
    -> sdbusplus::async::task<
        std::optional<std::pair<DbusProxy::Kind, std::string>>>
{
    try
    {
        const auto result = co_await ObjectMapperIntf(ctx)
                                .service(ObjectMapperIntf::default_service)
                                .path(ObjectMapperIntf::instance_path)
                                .get_object(path, supportedInterfaces);

        for (auto& [service, interfaces] : result)
        {
            bool hasSoftwareSettings = std::ranges::contains(
                interfaces, SoftwareSettingsIntf::interface);
            bool hasAsyncSet =
                std::ranges::contains(interfaces, AsyncSetIntf::interface);
            bool hasNvidiaWriteProtect = std::ranges::contains(
                interfaces, NvidiaWriteProtectIntf::interface);

            if (hasNvidiaWriteProtect)
            {
                co_return std::pair(DbusProxy::Kind::NvidiaWriteProtect,
                                    service);
            }
            else if (hasAsyncSet && hasSoftwareSettings)
            {
                co_return std::pair(DbusProxy::Kind::AsyncSoftwareSettings,
                                    service);
            }
            else if (hasSoftwareSettings)
            {
                co_return std::pair(DbusProxy::Kind::SoftwareSettings, service);
            }
        }
    }
    catch (const std::exception& e)
    {
        // Object not found
    }

    co_return {};
}

auto DbusProxy::set(bool value) -> sdbusplus::async::task<Result<void>>
{
    auto detected = co_await detect(*ctx, path);
    if (!detected)
    {
        warning("Object {OBJECT} not available", "OBJECT", path);
        co_return std::unexpected(Error::Unavailable);
    }

    // Update kind and service in case they changed
    auto& [detectedKind, detectedService] = detected.value();
    kind = detectedKind;
    service = detectedService;

    try
    {
        switch (kind)
        {
            case DbusProxy::Kind::SoftwareSettings:
            {
                co_await SoftwareSettingsIntf(*ctx)
                    .service(service)
                    .path(path.str)
                    .write_protected(value);
                break;
            }

            case DbusProxy::Kind::AsyncSoftwareSettings:
            {
                auto result = co_await nvidia::async::set(
                    *ctx, service, path, SoftwareSettingsIntf::interface,
                    "WriteProtected", value);
                if (!result)
                {
                    error("AsyncSet completed with error: {ERROR}", "ERROR",
                          nvidia::async::tostr(result.error()));
                    co_return std::unexpected(convert(result.error()));
                }
                info("async set completed successfully");
                break;
            }

            case DbusProxy::Kind::NvidiaWriteProtect:
            {
                co_await NvidiaWriteProtectIntf(*ctx)
                    .service(service)
                    .path(path.str)
                    .set_write_protected(value);
                break;
            }
        }
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        co_return std::unexpected(Error::Io);
    }
    catch (const std::exception&)
    {
        co_return std::unexpected(Error::InternalError);
    }

    co_return {};
}

auto DbusProxy::changed() -> sdbusplus::async::task<Result<bool>>
{
    auto token = co_await stdexec::get_stop_token();
    while (!token.stop_requested())
    {
        auto [interface, properties] =
            co_await match
                ->next<std::string, std::map<std::string, PropertiesVariant>>();

        // All supported interfaces have a bool "WriteProtected" property
        if (auto search = properties.find("WriteProtected");
            search != properties.end())
        {
            co_return std::get<bool>(search->second);
        }
    }

    co_return std::unexpected(Error::Aborted);
}

auto DbusProxy::get() -> sdbusplus::async::task<Result<bool>>
{
    std::string_view interface;
    switch (kind)
    {
        case DbusProxy::Kind::SoftwareSettings:
        case DbusProxy::Kind::AsyncSoftwareSettings:
            interface = SoftwareSettingsIntf::interface;
            break;

        case DbusProxy::Kind::NvidiaWriteProtect:
            interface = NvidiaWriteProtectIntf::interface;
            break;

        default:
            error("Invalid protector kind: {KIND}", "KIND",
                  static_cast<int>(kind));
            co_return std::unexpected(Error::InternalError);
    }

    try
    {
        co_return co_await sdbusplus::async::proxy()
            .service(service)
            .interface(interface)
            .path(path.str)
            .get_property<bool>(*ctx, "WriteProtected");
    }
    catch (const std::exception&)
    {
        co_return std::unexpected(Error::Io);
    }
}

} // namespace write_protect
} // namespace nvidia
