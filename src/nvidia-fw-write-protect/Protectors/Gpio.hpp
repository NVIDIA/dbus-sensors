#pragma once
#include "Protector.hpp"

#include <gpiod.hpp>
#include <sdbusplus/async.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace nvidia::write_protect
{

struct GpioConfig
{
    std::string lineName;
    bool activeLow = false;
    /// If set, poll at this interval instead of using edge interrupts.
    std::optional<std::chrono::milliseconds> pollInterval;

    bool isPolled() const
    {
        return pollInterval.has_value();
    }
};

/**
 * @brief Protector implementation backed by a GPIO line.
 *
 * Two modes of operation:
 *  - **Edge mode** (default) — monitors edge events via fdio.
 *  - **Poll mode** — periodically reads the line value at a configured
 *    interval.  Used for GPIO lines that do not support edge interrupts.
 *    Activated when @c GpioConfig::pollInterval is set.
 *
 * Not thread-safe: a single owner must ensure that get() and changed()
 * are never called concurrently.
 *
 * Read-only; set() returns Error::Unsupported.
 */
class Gpio : public Protector
{
    sdbusplus::async::context* ctx;
    GpioConfig config;
    gpiod::line line = {};
    std::optional<bool> lastValue = {};
    std::unique_ptr<sdbusplus::async::fdio> fdio = nullptr;

    static constexpr auto acquireLineRetryInterval =
        std::chrono::milliseconds(5000);
    static constexpr auto pollGpioChangedTimeout = std::chrono::minutes(2);

    explicit Gpio(sdbusplus::async::context& ctx, const GpioConfig& config) :
        ctx(&ctx), config(config)
    {}

    void teardown();
    auto acquire() -> bool;

    auto getWithRecovery() -> sdbusplus::async::task<Result<bool>>;

  public:
    Gpio(const Gpio&) = delete;
    Gpio& operator=(const Gpio&) = delete;

    Gpio(Gpio&& other) noexcept :
        ctx(std::exchange(other.ctx, nullptr)), config(std::move(other.config)),
        line(std::move(other.line)), lastValue(std::move(other.lastValue)),
        fdio(std::move(other.fdio))
    {
        other.line.reset();
    }

    Gpio& operator=(Gpio&& other) noexcept
    {
        if (this != &other)
        {
            teardown();
            ctx = std::exchange(other.ctx, nullptr);
            config = std::move(other.config);
            line = std::move(other.line);
            lastValue = std::move(other.lastValue);
            fdio = std::move(other.fdio);
            other.line.reset();
        }
        return *this;
    }

    ~Gpio() override
    {
        teardown();
    }

    /**
     * @brief Asynchronously create a Gpio protector from configuration.
     *
     * Locates the named GPIO line, retrying indefinitely until found or
     * cancelled via stop token.  In edge mode, requests the line for
     * both-edge events and creates an fdio watcher.  In poll mode (when
     * @c config.pollInterval is set), requests the line as input only.
     *
     * @param ctx    The sdbusplus async context.
     * @param config GPIO configuration (line name, polarity, poll interval).
     * @return The constructed Gpio, or std::nullopt if cancelled.
     */
    static auto create(sdbusplus::async::context& ctx, const GpioConfig& config)
        -> sdbusplus::async::task<std::optional<Gpio>>;

    /** @copydoc Protector::set */
    auto set(bool value) -> sdbusplus::async::task<Result<void>> override;
    /** @copydoc Protector::get */
    auto get() -> sdbusplus::async::task<Result<bool>> override;
    /** @copydoc Protector::changed */
    auto changed() -> sdbusplus::async::task<Result<bool>> override;
};

} // namespace nvidia::write_protect
