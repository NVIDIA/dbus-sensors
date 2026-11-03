// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
#include "LpuPowerModeService.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/system/error_code.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/property.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace lpu::power
{
namespace
{
constexpr std::string_view pwrSeqService = "com.Nvidia.PwrSeqMonitor";
constexpr std::string_view lpuPathPrefix =
    "/xyz/openbmc_project/inventory/system/accelerator/LPU_";
constexpr std::string_view operationalStatusInterface =
    "xyz.openbmc_project.State.Decorator.OperationalStatus";
// pwrseq reports Enabled only while both PG and Boot-Complete are asserted.
constexpr std::string_view onlineState =
    "xyz.openbmc_project.State.Decorator.OperationalStatus.StateType.Enabled";
// The LPU SMBus can still be busy just after Boot-Complete; keep trying for a
// minute before giving up until the next reset.
constexpr size_t restoreAttempts = 12;
constexpr std::chrono::seconds restoreRetryDelay{5};

std::optional<size_t> slotFromPath(std::string_view path)
{
    if (!path.starts_with(lpuPathPrefix))
    {
        return std::nullopt;
    }
    const std::string_view suffix = path.substr(lpuPathPrefix.size());
    size_t index = 0;
    const auto [end, ec] =
        std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
    if (ec != std::errc{} || end != suffix.data() + suffix.size() ||
        index >= slotCount)
    {
        return std::nullopt;
    }
    return index;
}

[[noreturn]] void dbusError(const Error& error)
{
    lg2::error("LPU power-mode operation failed: {ERROR}", "ERROR",
               error.what());
    int code = EAGAIN;
    if (error.failure == Failure::invalidArgument)
    {
        code = EINVAL;
    }
    else if (error.failure == Failure::writeFailure)
    {
        code = EIO;
    }
    throw sdbusplus::exception::SdBusError(code, error.what());
}

// lputelemetry has one thread, and on the shared BMC eMMC a single fsync can
// take seconds while other logs are being written. Run the file work on its
// own thread and suspend only this D-Bus call until an eventfd reports that it
// finished, so telemetry and other requests keep running meanwhile.
void runOffLoop(boost::asio::io_context& io,
                const boost::asio::yield_context& yield,
                const std::function<void()>& work)
{
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0)
    {
        throw Error(
            Failure::writeFailure,
            "Cannot create eventfd: " +
                std::error_code(errno, std::generic_category()).message());
    }
    boost::asio::posix::stream_descriptor done(io, fd);
    std::exception_ptr failure;
    {
        std::jthread worker;
        try
        {
            worker = std::jthread([&work, &failure, fd] {
                try
                {
                    work();
                }
                catch (...)
                {
                    failure = std::current_exception();
                }
                const uint64_t one = 1;
                // Cannot fail: a fresh eventfd counter is far from overflow.
                std::ignore = ::write(fd, &one, sizeof(one));
            });
        }
        catch (const std::system_error& e)
        {
            throw Error(Failure::writeFailure,
                        std::string("Cannot start file thread: ") + e.what());
        }
        // A failed wait still ends in the join below, so the result is
        // complete either way.
        boost::system::error_code ec;
        done.async_wait(boost::asio::posix::stream_descriptor::wait_read,
                        yield[ec]);
    }
    if (failure)
    {
        std::rethrow_exception(failure);
    }
}
} // namespace

Service::Service(boost::asio::io_context& io,
                 const std::shared_ptr<sdbusplus::asio::connection>& connection,
                 sdbusplus::asio::object_server& server) :
    io(io), connection(connection), server(server)
{
    retryTimers.reserve(slotCount);
    for (size_t slot = 0; slot < slotCount; ++slot)
    {
        retryTimers.emplace_back(io);
    }

    constexpr auto path = "/xyz/openbmc_project/control/power/TotalLPU_Power_0";
    server.add_manager("/xyz/openbmc_project/control");
    modeInterface = server.add_interface(path, "com.nvidia.LPUPowerMode");
    // Methods use live reads and synchronous verification. The only saved
    // state is the last applied selection, used to re-apply it after resets.
    // LPU access stays on the service's single io_context, serializing
    // requests with telemetry polling; only the file access of SetPowerMode
    // runs on a worker thread.
    // Return the live selector and its catalogue in one method reply. The
    // mapping is static platform data, not telemetry from unused 0x1B/0x1C.
    // Missing configuration must not disable the independent mode setter.
    modeInterface->register_method(
        "GetPowerProfile", [this]() -> ProfileSnapshot {
            try
            {
                std::string mode = controller.get();
                if (!catalog)
                {
                    return {mode, {}, "Unavailable"};
                }
                return {mode, catalog->profiles, catalog->status};
            }
            catch (const Error& error)
            {
                dbusError(error);
            }
        });
    modeInterface->register_method(
        "SetPowerMode",
        [this](boost::asio::yield_context yield, const std::string& mode) {
            setPowerMode(std::move(yield), mode);
        });
    modeInterface->initialize();
    associationInterface = server.add_interface(
        path, "xyz.openbmc_project.Association.Definitions");
    using Association = std::tuple<std::string, std::string, std::string>;
    associationInterface->register_property(
        "Associations",
        std::vector<Association>{
            {"chassis", "power_controls",
             "/xyz/openbmc_project/inventory/system/chassis/HGX_Chassis_0"}});
    associationInterface->initialize();

    // LPU firmware comes back unset after any reset. Re-apply the saved
    // selection each time pwrseq reports an LPU online again.
    lpuStateMatch = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*connection),
        "type='signal',member='PropertiesChanged',interface='org.freedesktop."
        "DBus.Properties',sender='com.Nvidia.PwrSeqMonitor',"
        "path_namespace='/xyz/openbmc_project/inventory/system/accelerator',"
        "arg0='xyz.openbmc_project.State.Decorator.OperationalStatus'",
        [this](sdbusplus::message_t& msg) {
            std::string interface;
            std::map<std::string, std::variant<std::string, bool>> changed;
            try
            {
                msg.read(interface, changed);
            }
            catch (const sdbusplus::exception_t& e)
            {
                lg2::error("Cannot parse LPU power-sequence state: {ERROR}",
                           "ERROR", e.what());
                return;
            }
            const auto state = changed.find("State");
            const char* objectPath = msg.get_path();
            if (state == changed.end() || objectPath == nullptr)
            {
                return;
            }
            const auto* value = std::get_if<std::string>(&state->second);
            const std::optional<size_t> slot = slotFromPath(objectPath);
            if (value != nullptr && *value == onlineState && slot)
            {
                restoreSlot(*slot, restoreAttempts);
            }
        });
    // pwrseq may have missed or not yet reported a reset while it was
    // restarting, so ask it again for every LPU.
    pwrSeqOwnerMatch = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*connection),
        "type='signal',sender='org.freedesktop.DBus',interface='org."
        "freedesktop.DBus',member='NameOwnerChanged',"
        "arg0='com.Nvidia.PwrSeqMonitor'",
        [this](sdbusplus::message_t& msg) {
            std::string name;
            std::string oldOwner;
            std::string newOwner;
            msg.read(name, oldOwner, newOwner);
            if (!newOwner.empty())
            {
                restoreAll();
            }
        });
    // Covers vHMC and BMC restarts, where no LPU state transition occurs.
    boost::asio::post(io, [this]() { restoreAll(); });
}

void Service::setPowerMode(boost::asio::yield_context yield,
                           const std::string& mode)
{
    try
    {
        const std::optional<Mode> target = modeFromName(mode);
        if (!target)
        {
            throw Error(Failure::invalidArgument, "Expected MaxQ or MaxP");
        }
        if (settingMode)
        {
            // Overlapping changes could interleave their saves and rollbacks.
            throw Error(Failure::unavailable,
                        "Another power mode change is in progress");
        }
        settingMode = true;
        try
        {
            setAndSave(controller, store, *target,
                       [this, yield = std::move(yield)](
                           const std::function<void()>& work) {
                           runOffLoop(io, yield, work);
                       });
        }
        catch (...)
        {
            settingMode = false;
            throw;
        }
        settingMode = false;
    }
    catch (const Error& error)
    {
        dbusError(error);
    }
}

void Service::restoreAll()
{
    if (!store.load())
    {
        return;
    }
    for (size_t slot = 0; slot < slotCount; ++slot)
    {
        sdbusplus::asio::getProperty<std::string>(
            *connection, std::string(pwrSeqService),
            std::string(lpuPathPrefix) + std::to_string(slot),
            std::string(operationalStatusInterface), "State",
            [this, slot](const boost::system::error_code& ec,
                         const std::string& state) {
                restoreIfOnline(slot, ec, state);
            });
    }
}

void Service::restoreIfOnline(size_t slot, const boost::system::error_code& ec,
                              const std::string& state)
{
    // A failed State query may be a transient D-Bus error on an online LPU, so
    // it gets the full retry budget too. An LPU that is really off fails every
    // attempt and is restored when pwrseq reports it online.
    if (ec || state == onlineState)
    {
        restoreSlot(slot, restoreAttempts);
    }
}

void Service::restoreSlot(size_t slot, size_t attemptsLeft)
{
    retryTimers[slot].cancel();
    const std::optional<Mode> saved = store.load();
    if (!saved)
    {
        return;
    }
    try
    {
        if (controller.restore(slot, *saved))
        {
            lg2::info("Restored LPU power mode {MODE} on LPU_{INDEX}", "MODE",
                      modeName(*saved), "INDEX", slot);
        }
        return;
    }
    catch (const Error& error)
    {
        if (attemptsLeft <= 1)
        {
            lg2::warning(
                "Cannot restore LPU power mode {MODE} on LPU_{INDEX}: {ERROR}",
                "MODE", modeName(*saved), "INDEX", slot, "ERROR", error.what());
            return;
        }
    }
    retryTimers[slot].expires_after(restoreRetryDelay);
    retryTimers[slot].async_wait(
        [this, slot, attemptsLeft](const boost::system::error_code& ec) {
            if (!ec)
            {
                restoreSlot(slot, attemptsLeft - 1);
            }
        });
}

void Service::updateProfileCatalog(const ManagedObjectType& objects)
{
    catalog = ProfileCatalog::fromManagedObjects(objects);
    if (!catalog)
    {
        lg2::error(
            "LPU power-profile Entity Manager configuration missing or invalid; watts unavailable");
        return;
    }
    if (catalog->status == "Tentative")
    {
        lg2::warning(
            "LPU power-profile watts are tentative, not final guaranteed limits");
    }
}

Service::~Service()
{
    server.remove_interface(associationInterface);
    server.remove_interface(modeInterface);
}
} // namespace lpu::power
