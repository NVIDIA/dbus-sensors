#pragma once
#include "Protector.hpp"

#include <gpiod.hpp>
#include <sdbusplus/async.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace nvidia::write_protect
{

struct GpioConfig
{
    std::string lineName;
    bool activeLow = false;
    /// If set, poll at this interval instead of using edge interrupts.
    std::optional<std::chrono::milliseconds> pollInterval;
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
 * Read-only; set() returns Error::Unsupported.
 */
class Gpio : public Protector
{
    sdbusplus::async::context* ctx;
    GpioConfig config;
    gpiod::line line;
    std::unique_ptr<sdbusplus::async::fdio> fdio;

    static constexpr int findLineRetries = 5;

    explicit Gpio(sdbusplus::async::context& ctx, const GpioConfig& config,
                  const gpiod::line& line,
                  std::unique_ptr<sdbusplus::async::fdio>&& fdio) :
        ctx(&ctx), config(config), line(line), fdio(std::move(fdio))
    {}

    auto getImpl() -> Result<bool>;

  public:
    Gpio(const Gpio&) = delete;
    Gpio& operator=(const Gpio&) = delete;
    Gpio(Gpio&&) = default;
    Gpio& operator=(Gpio&&) = default;

    /**
     * @brief Asynchronously create a Gpio protector from configuration.
     *
     * Locates the named GPIO line (retrying up to @c findLineRetries times).
     * In edge mode, requests the line for both-edge events and creates an
     * fdio watcher.  In poll mode (when @c config.pollInterval is set),
     * requests the line as input only.
     *
     * @param ctx    The sdbusplus async context.
     * @param config GPIO configuration (line name, polarity, poll interval).
     * @return The constructed Gpio, or std::nullopt if the line cannot be
     *         found or requested.
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
