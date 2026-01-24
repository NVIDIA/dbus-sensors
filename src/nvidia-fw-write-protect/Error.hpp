#pragma once

#include "AsyncSet.hpp"

#include <expected>

namespace nvidia
{
namespace write_protect
{

/**
 * @brief Error codes for write-protection operations.
 */
enum class Error
{
    /** @brief Unhandled internal error. */
    InternalError,

    /** @brief The requested operation is not supported. */
    Unsupported,

    /** @brief D-Bus or other I/O error. */
    Io,

    /** @brief The target resource is no longer available. */
    Unavailable,

    /** @brief The operation was stopped or aborted. */
    Aborted,
};

/**
 * @brief Convert an nvidia::async::Error to a write_protect::Error.
 * @param e The async error to convert.
 * @return The corresponding write_protect::Error value.
 */
inline auto convert(nvidia::async::Error e) -> Error
{
    using AsyncError = nvidia::async::Error;
    switch (e)
    {
        case AsyncError::InternalFailure:
            return Error::InternalError;
        case AsyncError::UnsupportedRequest:
            return Error::Unsupported;
        default:
            return Error::Io;
    }
}

template <typename T>
using Result = std::expected<T, Error>;

} // namespace write_protect
} // namespace nvidia
