#pragma once

#include "Error.hpp"

#include <sdbusplus/async.hpp>

namespace nvidia
{
namespace write_protect
{

/**
 * @brief An interface for controlling write protection and monitoring for
 * changes.
 */
class Protector
{
  public:
    virtual ~Protector() = default;

    /**
     * @brief Attempts to set the value
     * @returns std::expected<void> if successful, std::unexpected<Error> if the
     * operation failed.
     */
    virtual auto set(bool value) -> sdbusplus::async::task<Result<void>> = 0;

    /**
     * @brief Attempts to get the value
     * @returns std::expected<bool> containing the value retrieved if
     * successful, std::unexpected<Error> if the operation failed.
     */
    virtual auto get() -> sdbusplus::async::task<Result<bool>> = 0;

    /**
     * @brief Listen for changes
     * @returns The new value, std::unexpected<Error> if an error occurred.
     */
    virtual auto changed() -> sdbusplus::async::task<Result<bool>> = 0;
};

} // namespace write_protect
} // namespace nvidia
