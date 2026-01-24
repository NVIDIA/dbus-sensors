#include "Gpio.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/async.hpp>

#include <bitset>
#include <chrono>
#include <exception>

PHOSPHOR_LOG2_USING;

namespace nvidia::write_protect
{

static auto getGpioValue(const gpiod::line& line) -> Result<bool>
{
    try
    {
        int result = line.get_value();
        if (result < 0)
        {
            return std::unexpected(Error::Io);
        }
        return bool(result);
    }
    catch (const std::exception&)
    {
        return std::unexpected(Error::Io);
    }
}

auto Gpio::create(sdbusplus::async::context& ctx, const GpioConfig& config)
    -> sdbusplus::async::task<std::optional<Gpio>>
{
    gpiod::line line = gpiod::find_line(config.lineName);

    if (!line)
    {
        for (int i = 0; i < findLineRetries && !line; i++)
        {
            co_await sdbusplus::async::sleep_for(ctx, std::chrono::seconds(5));
            line = gpiod::find_line(config.lineName);
        }
    }

    if (!line)
    {
        error("Could not find GPIO line {LINE}", "LINE", config.lineName);
        co_return {};
    }

    info("Found GPIO line {LINE}", "LINE", config.lineName);

    std::bitset<32> flags =
        config.activeLow ? gpiod::line_request::FLAG_ACTIVE_LOW : 0;

    if (config.pollInterval)
    {
        try
        {
            line.request(
                {config.lineName, gpiod::line_request::DIRECTION_INPUT, flags});
        }
        catch (std::exception& e)
        {
            warning("Failed requesting line for {GPIO}: {ERROR}", "GPIO",
                    config.lineName, "ERROR", e.what());
            co_return {};
        }

        info("GPIO {LINE} using poll mode ({MS}ms)", "LINE", config.lineName,
             "MS", config.pollInterval->count());
        co_return Gpio(ctx, config, std::move(line), nullptr);
    }

    try
    {
        line.request(
            {config.lineName, gpiod::line_request::EVENT_BOTH_EDGES, flags});
    }
    catch (std::exception& e)
    {
        warning("Failed requesting line for {GPIO}: {ERROR}", "GPIO",
                config.lineName, "ERROR", e.what());
        co_return {};
    }

    int lineFd = line.event_get_fd();
    if (lineFd < 0)
    {
        warning("Failed to get event fd for GPIO line {GPIO}", "GPIO",
                config.lineName);
        co_return {};
    }

    co_return Gpio(ctx, config, std::move(line),
                   std::make_unique<sdbusplus::async::fdio>(ctx, lineFd));
}

auto Gpio::set([[maybe_unused]] bool value)
    -> sdbusplus::async::task<Result<void>>
{
    co_return std::unexpected(Error::Unsupported);
}

auto Gpio::getImpl() -> Result<bool>
{
    return getGpioValue(line);
}

auto Gpio::get() -> sdbusplus::async::task<Result<bool>>
{
    co_return getImpl();
}

auto Gpio::changed() -> sdbusplus::async::task<Result<bool>>
{
    if (fdio)
    {
        co_await fdio->next();
        line.event_read();
        co_return getImpl();
    }

    // Poll mode: sleep and re-read until the value differs from current.
    auto prev = getImpl();
    if (!prev)
    {
        co_return prev;
    }

    while (true)
    {
        co_await sdbusplus::async::sleep_for(*ctx, *config.pollInterval);
        auto cur = getImpl();
        if (!cur || *cur != *prev)
        {
            co_return cur;
        }
    }
}

} // namespace nvidia::write_protect
