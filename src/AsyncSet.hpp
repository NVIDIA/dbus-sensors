#pragma once

#include <com/nvidia/Async/Set/client.hpp>
#include <com/nvidia/Async/Status/client.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/async.hpp>

#include <chrono>
#include <expected>
#include <format>

PHOSPHOR_LOG2_USING;

namespace nvidia
{
namespace async
{

enum class Error
{
    Timeout,
    InvalidArgument,
    InternalFailure,
    ResourceNotFound,
    Unavailable,
    UnsupportedRequest,
    WriteFailure,
    NoResponse,
};

inline std::string_view tostr(Error e)
{
    switch (e)
    {
        case Error::Timeout:
            return "Timeout";
        case Error::InvalidArgument:
            return "InvalidArgument";
        case Error::InternalFailure:
            return "InternalFailure";
        case Error::ResourceNotFound:
            return "ResourceNotFound";
        case Error::Unavailable:
            return "Unavailable";
        case Error::UnsupportedRequest:
            return "UnsupportedRequest";
        case Error::WriteFailure:
            return "WriteFailure";
        case Error::NoResponse:
            return "NoResponse";
    }
    return "Unknown";
}

using Result = std::expected<void, Error>;

namespace exec = sdbusplus::async::execution;

using SetIntf = sdbusplus::client::com::nvidia::async::Set<>;
using StatusIntf = sdbusplus::client::com::nvidia::async::Status<>;
using Status = StatusIntf::AsyncOperationStatus;

/**
 * @brief Attempt to set the property to the specified value using the
 * com.nvidia.Async.Set interface.
 * @returns std::expected<void> if successful, std::unexpected<Error> if the
 * operation failed.
 */
auto set(sdbusplus::async::context& ctx, std::string service,
         sdbusplus::object_path path, std::string interface,
         std::string property, auto value,
         std::chrono::milliseconds timeout = std::chrono::milliseconds(30000))
    -> sdbusplus::async::task<Result>
{
    auto statusObjectPath =
        co_await SetIntf(ctx).service(service).path(path.str).set(
            interface, property, value);
    debug("got status path={PATH}", "PATH", statusObjectPath.str);

    auto client = StatusIntf(ctx).service(service).path(statusObjectPath.str);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    constexpr auto pollInterval = std::chrono::milliseconds(10);

    auto token = co_await stdexec::get_stop_token();
    while (!token.stop_requested())
    {
        auto status = co_await client.status();
        switch (status)
        {
            case Status::InProgress:
                break;

            case Status::Success:
                co_return {};

            case Status::Timeout:
                co_return std::unexpected(Error::Timeout);

            case Status::InvalidArgument:
                co_return std::unexpected(Error::InvalidArgument);

            case Status::InternalFailure:
                co_return std::unexpected(Error::InternalFailure);

            case Status::ResourceNotFound:
                co_return std::unexpected(Error::ResourceNotFound);

            case Status::Unavailable:
                co_return std::unexpected(Error::Unavailable);

            case Status::UnsupportedRequest:
                co_return std::unexpected(Error::UnsupportedRequest);

            case Status::WriteFailure:
                co_return std::unexpected(Error::WriteFailure);

            default:
                error("Unhandled async status {STATUS}", "STATUS",
                      static_cast<uint32_t>(status));
                co_return std::unexpected(Error::InternalFailure);
        }

        // TODO: better to select(async::set, timer) but match::next sender does
        // not correctly implement cancellation (dangling pointer in match
        // object).
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            co_return std::unexpected(Error::NoResponse);
        }

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        auto sleepTime = std::min(remaining, pollInterval);
        if (sleepTime > std::chrono::milliseconds::zero())
        {
            co_await sdbusplus::async::sleep_for(ctx, sleepTime);
        }
    }
}

} // namespace async
} // namespace nvidia
