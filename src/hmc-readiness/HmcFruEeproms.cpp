#include "HmcFruEeproms.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <unistd.h>

extern "C"
{
#include <i2c/smbus.h>
#include <linux/i2c-dev.h>
}

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <gpiod.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace
{

namespace fs = std::filesystem;

constexpr const char* bootUnitPath =
    "/org/freedesktop/systemd1/unit/bmc_2dboot_2dcomplete_2eservice";
// Units that follow the HMC (nvidia-usb-monitor.service is PartOf the first
// and Conflicts with the second); started as phosphor-gpio-monitor did.
constexpr const char* readyTarget = "hmc-ready.target";
constexpr const char* notReadyTarget = "hmc-not-ready.target";

enum class Result
{
    ok,
    skipped,
    failed
};

using Device = HmcFruEeproms::Device;

std::string busDir(const Device& d)
{
    return std::format("/sys/bus/i2c/devices/i2c-{}/", d.bus);
}

std::string devDir(const Device& d)
{
    return busDir(d) + std::format("{}-{:04x}", d.bus, d.addr);
}

bool writeSysfs(const std::string& path, const std::string& value)
{
    std::ofstream f(path);
    f << value << '\n';
    f.flush();
    bool ok = f.good();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return ok;
}

bool responds(const Device& d)
{
    const std::string dev = std::format("/dev/i2c-{}", d.bus);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    int fd = open(dev.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        return false;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    bool ok = ioctl(fd, I2C_SLAVE_FORCE, d.addr) >= 0 &&
              i2c_smbus_read_byte_data(fd, 0) >= 0;
    close(fd);
    return ok;
}

// A stale client without an eeprom file is deleted before binding again.
void deleteDevice(const Device& d)
{
    if (!writeSysfs(busDir(d) + "delete_device",
                    std::format("{:#04x}", d.addr)) ||
        fs::exists(devDir(d)))
    {
        lg2::error("Failed to delete stale {NAME}", "NAME", d.name);
    }
}

Result bind(const Device& d)
{
    auto miss = [&d](const char* why) {
        if (d.optional)
        {
            lg2::warning("{NAME} (bus {BUS}, {ADDR}) {WHY}, skipped", "NAME",
                         d.name, "BUS", d.bus, "ADDR", d.address, "WHY", why);
            return Result::skipped;
        }
        lg2::error("{NAME} (bus {BUS}, {ADDR}) {WHY}", "NAME", d.name, "BUS",
                   d.bus, "ADDR", d.address, "WHY", why);
        return Result::failed;
    };
    if (!responds(d))
    {
        return miss("not present on the bus");
    }
    const std::string eeprom = devDir(d) + "/eeprom";
    if (fs::exists(eeprom))
    {
        lg2::info("{NAME} already bound", "NAME", d.name);
        return Result::ok;
    }
    if (fs::exists(devDir(d)))
    {
        lg2::warning("{NAME} has no eeprom, rebinding", "NAME", d.name);
        deleteDevice(d);
    }
    if (!writeSysfs(busDir(d) + "new_device", d.driver + " " + d.address) ||
        !fs::exists(eeprom))
    {
        return miss("failed to bind");
    }
    lg2::info("{NAME} bound", "NAME", d.name);
    return Result::ok;
}

template <typename T>
const T* field(const nlohmann::json& j, const char* key)
{
    auto it = j.find(key);
    return it == j.end() ? nullptr : it->template get_ptr<const T*>();
}

} // namespace

std::unique_ptr<HmcFruEeproms> HmcFruEeproms::create(
    boost::asio::io_context& io,
    std::shared_ptr<sdbusplus::asio::connection> conn,
    const std::string& configPath)
{
    std::ifstream f(configPath);
    auto j = nlohmann::json::parse(f, nullptr, false);
    const auto* gpio = field<std::string>(j, "ReadyGpio");
    const auto* rescan = field<uint64_t>(j, "RescanBus");
    const auto* list = field<nlohmann::json::array_t>(j, "Devices");
    std::vector<Device> devices;
    for (const auto& e : list != nullptr ? *list : nlohmann::json::array_t{})
    {
        const auto* bus = field<uint64_t>(e, "Bus");
        const auto* addr = field<std::string>(e, "Address");
        const auto* drv = field<std::string>(e, "Driver");
        const auto* name = field<std::string>(e, "Name");
        const auto* opt = field<bool>(e, "Optional");
        char* end = nullptr;
        unsigned long a =
            addr != nullptr ? std::strtoul(addr->c_str(), &end, 0) : 0;
        if (bus == nullptr || drv == nullptr || name == nullptr || a == 0 ||
            a > 0x7f || *end != '\0')
        {
            list = nullptr;
            break;
        }
        devices.push_back(
            {static_cast<uint16_t>(*bus), *addr, static_cast<unsigned>(a), *drv,
             *name, opt != nullptr && *opt});
    }
    if (gpio == nullptr || rescan == nullptr || list == nullptr)
    {
        lg2::error("Invalid {PATH}", "PATH", configPath);
        return nullptr;
    }
    return std::make_unique<HmcFruEeproms>(
        io, std::move(conn), *gpio, static_cast<uint16_t>(*rescan),
        std::move(devices));
}

HmcFruEeproms::HmcFruEeproms(boost::asio::io_context& io,
                             std::shared_ptr<sdbusplus::asio::connection> conn,
                             const std::string& gpioName, uint16_t rescanBus,
                             std::vector<Device> devices) :
    conn(std::move(conn)), rescanBus(rescanBus), devices(std::move(devices)),
    gpioFd(io), retry(io)
{
    try
    {
        line = gpiod::find_line(gpioName);
        line.request(
            {"hmcreadiness", gpiod::line_request::EVENT_BOTH_EDGES, 0});
        // The line keeps its own fd; the descriptor owns a copy.
        gpioFd.assign(dup(line.event_get_fd()));
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to request {GPIO}: {ERR}", "GPIO", gpioName, "ERR",
                   e);
        return;
    }

    // Wait for bmc-boot-complete.service before the first bind.
    bootMatch = std::make_unique<sdbusplus::bus::match_t>(
        *this->conn,
        sdbusplus::bus::match::rules::propertiesChanged(
            bootUnitPath, "org.freedesktop.systemd1.Unit"),
        [this](sdbusplus::message_t& msg) {
            std::string iface;
            std::map<std::string, std::variant<std::string>> props;
            msg.read(iface, props);
            auto it = props.find("ActiveState");
            if (it != props.end())
            {
                onActiveState(std::get<std::string>(it->second));
            }
        });
    this->conn->async_method_call(
        [](const boost::system::error_code&) {}, "org.freedesktop.systemd1",
        "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager",
        "Subscribe");
    this->conn->async_method_call(
        [this](const boost::system::error_code& ec,
               const std::variant<std::string>& state) {
            if (!ec)
            {
                onActiveState(std::get<std::string>(state));
            }
        },
        "org.freedesktop.systemd1", bootUnitPath,
        "org.freedesktop.DBus.Properties", "Get",
        "org.freedesktop.systemd1.Unit", "ActiveState");
}

void HmcFruEeproms::onActiveState(const std::string& state)
{
    if (started || state != "active")
    {
        return;
    }
    started = true;
    lg2::info("bmc-boot-complete.service active, following the HMC ready GPIO");
    watchGpio();
}

// Drains queued edges and acts on the current level, so edges that arrive
// during a run collapse into one more run: starts hmc-ready.target or
// hmc-not-ready.target, and binds while high. A falling edge leaves the
// EEPROMs bound: an HMC reboot does not remove its FRUs.
void HmcFruEeproms::watchGpio()
{
    bool ready = false;
    try
    {
        pollfd p{gpioFd.native_handle(), POLLIN, 0};
        while (poll(&p, 1, 0) > 0)
        {
            line.event_read();
        }
        ready = line.get_value() != 0;
    }
    catch (const std::exception& e)
    {
        lg2::error("HMC ready GPIO read failed: {ERR}", "ERR", e);
        return;
    }
    retry.cancel();
    startUnit(ready ? readyTarget : notReadyTarget);
    if (ready)
    {
        bindAll(1);
    }
    gpioFd.async_wait(boost::asio::posix::stream_descriptor::wait_read,
                      [this](const boost::system::error_code& ec) {
                          if (ec)
                          {
                              lg2::error("HMC ready GPIO wait failed: {ERR}",
                                         "ERR", ec.message());
                              return;
                          }
                          watchGpio();
                      });
}

void HmcFruEeproms::startUnit(const char* unit)
{
    conn->async_method_call(
        [unit](const boost::system::error_code& ec,
               const sdbusplus::object_path&) {
            if (ec)
            {
                lg2::error("Failed to start {UNIT}: {ERR}", "UNIT", unit, "ERR",
                           ec.message());
            }
        },
        "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
        "org.freedesktop.systemd1.Manager", "StartUnit", unit, "replace");
}

// Binds what is missing; devices that do not answer yet are retried up to
// six times, 2 s apart, as the HMC brings its EEPROMs up after READY. Then
// FruDevice rescans the bus.
void HmcFruEeproms::bindAll(int attempt)
{
    int ok = 0;
    int failed = 0;
    int skipped = 0;
    for (const auto& d : devices)
    {
        Result r = bind(d);
        ok += r == Result::ok ? 1 : 0;
        failed += r == Result::failed ? 1 : 0;
        skipped += r == Result::skipped ? 1 : 0;
    }
    lg2::info(
        "HMC FRU EEPROM bind {N}: {OK} succeeded, {FAILED} failed, {SKIPPED} skipped",
        "N", attempt, "OK", ok, "FAILED", failed, "SKIPPED", skipped);
    if (failed != 0 && attempt < maxAttempts)
    {
        retry.expires_after(std::chrono::seconds(2));
        retry.async_wait([this, attempt](const boost::system::error_code& ec) {
            if (!ec)
            {
                bindAll(attempt + 1);
            }
        });
        return;
    }
    conn->async_method_call(
        [bus = rescanBus](const boost::system::error_code& ec) {
            if (ec)
            {
                lg2::warning("FruDevice ReScanBus {BUS} failed: {ERR}", "BUS",
                             bus, "ERR", ec.message());
                return;
            }
            lg2::info("FruDevice ReScanBus {BUS} done", "BUS", bus);
        },
        "xyz.openbmc_project.FruDevice", "/xyz/openbmc_project/FruDevice",
        "xyz.openbmc_project.FruDeviceManager", "ReScanBus", rescanBus);
}
