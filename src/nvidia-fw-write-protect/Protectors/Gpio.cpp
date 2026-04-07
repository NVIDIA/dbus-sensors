#include "Gpio.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/async.hpp>

#include <bitset>
#include <chrono>
#include <exception>
#include <system_error>

PHOSPHOR_LOG2_USING;

namespace nvidia::write_protect
{

auto Gpio::acquire() -> bool
{
    gpiod::line tempLine;
    try
    {
        tempLine = gpiod::find_line(config.lineName);
    }
    catch (const std::exception& e)
    {
        warning("Error searching for GPIO line {LINE}: {ERROR}", "LINE",
                config.lineName, "ERROR", e.what());
        return false;
    }

    if (!tempLine)
    {
        return false;
    }

    info("Found GPIO line {LINE}", "LINE", config.lineName);

    std::bitset<32> flags =
        config.activeLow ? gpiod::line_request::FLAG_ACTIVE_LOW : 0;

    if (config.isPolled())
    {
        // poll mode
        try
        {
            tempLine.request(
                {config.lineName, gpiod::line_request::DIRECTION_INPUT, flags});
        }
        catch (std::exception& e)
        {
            warning("Failed requesting line for {GPIO}: {ERROR}", "GPIO",
                    config.lineName, "ERROR", e.what());
            return false;
        }

        info("GPIO {LINE} using poll mode ({MS}ms)", "LINE", config.lineName,
             "MS", config.pollInterval->count());
        line = std::move(tempLine);
        return true;
    }

    // fdio mode
    try
    {
        tempLine.request(
            {config.lineName, gpiod::line_request::EVENT_BOTH_EDGES, flags});
    }
    catch (std::exception& e)
    {
        warning("Failed requesting line for {GPIO}: {ERROR}", "GPIO",
                config.lineName, "ERROR", e.what());
        return false;
    }

    int lineFd = tempLine.event_get_fd();
    if (lineFd < 0)
    {
        warning("Failed to get event fd for GPIO line {GPIO}", "GPIO",
                config.lineName);
        tempLine.release();
        return false;
    }

    fdio = std::make_unique<sdbusplus::async::fdio>(*ctx, lineFd);
    line = std::move(tempLine);
    return true;
}

void Gpio::teardown()
{
    fdio.reset();
    if (line)
    {
        if (line.is_requested())
        {
            line.release();
        }
        line.reset();
    }
}

auto Gpio::create(sdbusplus::async::context& ctx, const GpioConfig& config)
    -> sdbusplus::async::task<std::optional<Gpio>>
{
    auto token = co_await stdexec::get_stop_token();
    auto gpio = Gpio(ctx, config);

    while (!token.stop_requested())
    {
        if (gpio.acquire())
        {
            co_return gpio;
        }
        co_await sdbusplus::async::sleep_for(ctx, acquireLineRetryInterval);
    }

    co_return {};
}

auto Gpio::set([[maybe_unused]] bool value)
    -> sdbusplus::async::task<Result<void>>
{
    co_return std::unexpected(Error::Unsupported);
}

auto Gpio::getWithRecovery() -> sdbusplus::async::task<Result<bool>>
{
    auto token = co_await stdexec::get_stop_token();
    while (!token.stop_requested())
    {
        std::error_code ec;
        try
        {
            co_return bool(line.get_value());
        }
        catch (const std::system_error& e)
        {
            ec = e.code();
        }

        if (ec != std::errc::no_such_device &&
            ec != std::errc::bad_file_descriptor)
        {
            co_return std::unexpected(Error::Io);
        }

        warning("GPIO chip for line {LINE} was removed - trying to reacquire",
                "LINE", config.lineName);

        // GPIO chip was removed, try to acquire it then retry reading the GPIO
        teardown();
        while (!token.stop_requested())
        {
            if (acquire())
            {
                break;
            }
            co_await sdbusplus::async::sleep_for(*ctx,
                                                 acquireLineRetryInterval);
        }
    }

    // Aborted if we never called teardown()
    auto error = line ? Error::Aborted : Error::Unavailable;
    co_return std::unexpected(error);
}

auto Gpio::get() -> sdbusplus::async::task<Result<bool>>
{
    Result<bool> res = co_await getWithRecovery();
    if (res)
    {
        lastValue = *res;
    }
    co_return res;
}

auto Gpio::changed() -> sdbusplus::async::task<Result<bool>>
{
    if (fdio)
    {
        // fdio mode: wait for line changed event sent by the kernel.
        // If the chip was removed, fdio->next() returns (POLLERR) and
        // event_read() throws. Discard the error and fall through to
        // get(), whose recovery loop will teardown/reacquire the line.
        co_await fdio->next();
        try
        {
            line.event_read();
        }
        catch (const std::system_error&)
        {}
        co_return co_await get();
    }

    // Poll mode: sleep and re-read until the value differs from current.
    auto token = co_await stdexec::get_stop_token();

    // Deadline for successfully reading the GPIO line once
    auto deadline = std::chrono::steady_clock::now() + pollGpioChangedTimeout;

    // Copy the last read value since Gpio::get() updates it for
    // successful reads.
    std::optional<bool> cur = lastValue;

    while (!token.stop_requested())
    {
        Result<bool> value = co_await get();
        if (value)
        {
            // Return the read value if there's no previously recorded value or
            // the value changed.
            if (!cur || *cur != *value)
            {
                co_return value;
            }

            // Refresh the deadline
            deadline = std::chrono::steady_clock::now() +
                       pollGpioChangedTimeout;
        }
        else
        {
            if (value.error() == Error::Unavailable)
            {
                co_return std::unexpected(Error::Unavailable);
            }

            auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                co_return std::unexpected(Error::Timeout);
            }
        }

        co_await sdbusplus::async::sleep_for(*ctx, *config.pollInterval);
    }

    co_return std::unexpected(Error::Aborted);
}

} // namespace nvidia::write_protect
