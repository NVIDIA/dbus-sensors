// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA CORPORATION & AFFILIATES

// hmc-usb-monitor: keeps the BMC-HMC USB network link working, as
// usb_status_monitor.sh did; nvidia-usb-monitor.service runs it while the HMC
// is ready. The link and its host controller are the network interface and
// ehci-platform controller of the HMC's RNDIS gadget (USB 0525:a4a2) in sysfs.
//
// Every 5 s: if phosphor-networkd shows the link offline, or the HMC
// does not answer 18 pings 5 s apart, re-authorize the HMC's RNDIS USB
// device; if the HMC still does not answer, reset usb1 and rebind its host
// controller.

#include "hmc_usb_monitor.hpp"

#include "Utils.hpp"

#include <unistd.h>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp> // NOLINT(misc-include-cleaner)
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/icmp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <algorithm>
#include <array>
#include <bitset>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace hmc_usb_monitor
{

namespace
{

namespace fs = std::filesystem;
using std::chrono::seconds;

constexpr auto hmcAddress = "172.31.13.251";
constexpr std::string_view usbVendor = "0525";  // Netchip
constexpr std::string_view usbProduct = "a4a2"; // RNDIS ethernet device
constexpr int pingTries = 18;
constexpr seconds pingRetryDelay{5};
constexpr seconds resetDelay{2};
constexpr seconds driverInitDelay{5};
constexpr seconds loopDelay{5};

constexpr auto networkService = "xyz.openbmc_project.Network";
constexpr auto networkRoot = "/xyz/openbmc_project/network";

std::string readLine(const fs::path& path)
{
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);
    return line;
}

// Like the script's echo, a failed write is reported and the step goes on.
void writeSysfs(const fs::path& path, std::string_view value)
{
    std::ofstream f(path);
    f << value;
    f.close();
    if (f.fail())
    {
        lg2::error("Cannot write {VALUE} to {PATH}: {ERROR}", "VALUE", value,
                   "PATH", path.string(), "ERROR", std::strerror(errno));
    }
}

boost::asio::awaitable<bool> waitForHmcPing(const Ping& ping,
                                            const Sleep& sleep)
{
    for (int tries = 1;; ++tries)
    {
        if (co_await ping())
        {
            co_return true;
        }
        if (tries >= pingTries)
        {
            lg2::error("HMC not responding to ping");
            co_return false;
        }
        co_await sleep(pingRetryDelay);
    }
}

boost::asio::awaitable<void> sleepFor(boost::asio::steady_timer& timer,
                                      seconds duration)
{
    timer.expires_after(duration);
    co_await timer.async_wait(boost::asio::use_awaitable);
}

// ping -c 5 -W 1 on a raw ICMP socket: five echo requests a second apart,
// then 1 s for a late reply; true when any reply came. Ends early once all
// five are answered.
boost::asio::awaitable<bool> echoHmc(boost::asio::ip::icmp::socket& socket,
                                     boost::asio::steady_timer& timer,
                                     uint16_t& sequence)
{
    using namespace boost::asio::experimental::awaitable_operators;
    constexpr std::size_t count = 5;
    constexpr std::size_t receiveBufferSize = 1500;
    const boost::asio::ip::icmp::endpoint peer(
        boost::asio::ip::make_address_v4(hmcAddress), 0);
    const auto id = static_cast<uint16_t>(::getpid());
    const uint16_t first = sequence;
    const auto start = std::chrono::steady_clock::now();
    std::bitset<count> answered;
    std::array<uint8_t, receiveBufferSize> in{};
    for (std::size_t i = 0; i < count && !answered.all(); ++i)
    {
        // Echo request: type 8, checksum, identifier, sequence, 56 bytes.
        std::array<uint8_t, 64> out{8};
        out[4] = static_cast<uint8_t>(id >> 8);
        out[5] = static_cast<uint8_t>(id);
        out[6] = static_cast<uint8_t>(sequence >> 8);
        out[7] = static_cast<uint8_t>(sequence);
        ++sequence;
        uint32_t sum = 0;
        for (std::size_t j = 0; j < out.size(); j += 2)
        {
            sum += static_cast<uint32_t>(out[j] << 8 | out[j + 1]);
        }
        while ((sum >> 16) != 0)
        {
            sum = (sum & 0xffff) + (sum >> 16);
        }
        sum = ~sum;
        out[2] = static_cast<uint8_t>(sum >> 8);
        out[3] = static_cast<uint8_t>(sum);
        boost::system::error_code ec;
        co_await socket.async_send_to(
            boost::asio::buffer(out), peer,
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec && i == 0)
        {
            // ping fails at once too when there is no route (hmcusb0 gone).
            sequence = static_cast<uint16_t>(first + count);
            co_return false;
        }

        timer.expires_at(start + seconds(static_cast<seconds::rep>(i + 1)));
        while (!answered.all())
        {
            auto got = co_await (
                socket.async_receive(boost::asio::buffer(in),
                                     boost::asio::redirect_error(
                                         boost::asio::use_awaitable, ec)) ||
                timer.async_wait(boost::asio::use_awaitable));
            if (got.index() != 0 || ec)
            {
                break;
            }
            // An IPv4 header, then an echo reply (type 0) from the HMC to one
            // of this round's requests.
            const std::size_t size = std::get<0>(got);
            const std::size_t ihl = static_cast<std::size_t>(in[0] & 0x0f) * 4;
            if (size < ihl + 8 || in[9] != 1 ||
                boost::asio::ip::address_v4({in[12], in[13], in[14], in[15]}) !=
                    peer.address().to_v4())
            {
                continue;
            }
            const std::span<const uint8_t> icmp(&in.at(ihl), 8);
            const auto n = static_cast<uint16_t>(
                static_cast<uint16_t>(icmp[6] << 8 | icmp[7]) - first);
            if (icmp[0] == 0 && (icmp[4] << 8 | icmp[5]) == id && n <= i)
            {
                answered.set(n);
            }
        }
    }
    sequence = static_cast<uint16_t>(first + count);
    co_return answered.any();
}

} // namespace

// Whether phosphor-networkd shows the link online: read once, then again
// whenever networkd signals a change to the link or its addresses.
class LinkState
{
  public:
    LinkState(std::shared_ptr<sdbusplus::asio::connection> conn,
              const std::string& name, std::function<void()> ready) :
        conn(std::move(conn)), path(std::string(networkRoot) + "/" + name),
        ready(std::move(ready)),
        changed(*this->conn,
                sdbusplus::bus::match::rules::propertiesChanged(
                    path, "xyz.openbmc_project.Network.EthernetInterface"),
                [this](sdbusplus::message_t&) { refresh(); }),
        objects(*this->conn,
                sdbusplus::bus::match::rules::type::signal() +
                    sdbusplus::bus::match::rules::sender(networkService) +
                    sdbusplus::bus::match::rules::path(networkRoot) +
                    sdbusplus::bus::match::rules::interface(
                        "org.freedesktop.DBus.ObjectManager"),
                std::bind_front(&LinkState::objectsChanged, this))
    {
        refresh();
    }

    bool online() const
    {
        return up;
    }

  private:
    void refresh()
    {
        unsigned attempt = ++generation;
        conn->async_method_call(
            [this, attempt](const boost::system::error_code& ec,
                            const ManagedObjectType& objects) {
                if (attempt != generation)
                {
                    return;
                }
                // Without phosphor-networkd the link state is unknown: leave
                // it to the ping check instead of resetting a working link.
                if (ec)
                {
                    lg2::warning("Cannot read {PATH} from phosphor-networkd: "
                                 "{ERROR}",
                                 "PATH", path, "ERROR", ec.message());
                }
                up = ec || isOnline(objects, path);
                if (ready)
                {
                    std::exchange(ready, nullptr)();
                }
            },
            networkService, networkRoot, "org.freedesktop.DBus.ObjectManager",
            "GetManagedObjects");
    }

    // InterfacesAdded or InterfacesRemoved for the link or one of its
    // addresses.
    void objectsChanged(sdbusplus::message_t& msg)
    {
        sdbusplus::object_path object;
        try
        {
            msg.read(object);
        }
        catch (const sdbusplus::exception_t&)
        {
            return;
        }
        if (object.str == path || object.str.starts_with(path + "/"))
        {
            refresh();
        }
    }

    std::shared_ptr<sdbusplus::asio::connection> conn;
    std::string path;
    std::function<void()> ready;
    bool up = false;
    unsigned generation = 0;
    sdbusplus::bus::match_t changed;
    sdbusplus::bus::match_t objects;
};

namespace
{

boost::asio::awaitable<void> monitor(const LinkState& link, const HmcUsb& usb,
                                     const Ping& ping, const Sleep& sleep)
{
    while (true)
    {
        if (!link.online())
        {
            lg2::error("USB connection is down, attempting to recover");
            co_await recoverUsbConnection("/sys", usb, ping, sleep);
        }
        // The link can stay online while the HMC's RNDIS gadget or network
        // stack hangs; only an answer from the HMC shows it is reachable.
        if (!co_await waitForHmcPing(ping, sleep))
        {
            lg2::error("Cannot ping HMC, attempting to recover");
            co_await recoverUsbConnection("/sys", usb, ping, sleep);
        }
        co_await sleep(loopDelay);
    }
}

} // namespace

bool isOnline(const ManagedObjectType& objects, const std::string& path)
{
    auto link = objects.find(sdbusplus::object_path(path));
    if (link == objects.end())
    {
        return false;
    }
    auto eth =
        link->second.find("xyz.openbmc_project.Network.EthernetInterface");
    if (eth == link->second.end())
    {
        return false;
    }
    auto up = eth->second.find("LinkUp");
    const bool* linkUp =
        up == eth->second.end() ? nullptr : std::get_if<bool>(&up->second);
    if (linkUp == nullptr || !*linkUp)
    {
        return false;
    }
    return std::ranges::any_of(objects, [&path](const auto& object) {
        auto ip = object.second.find("xyz.openbmc_project.Network.IP");
        if (!object.first.str.starts_with(path + "/") ||
            ip == object.second.end())
        {
            return false;
        }
        auto type = ip->second.find("Type");
        const auto* name = type == ip->second.end()
                               ? nullptr
                               : std::get_if<std::string>(&type->second);
        return name != nullptr && name->ends_with(".IPv4");
    });
}

std::optional<HmcUsb> findHmcUsb(const fs::path& sysfs)
{
    std::error_code ec;
    for (const auto& net : fs::directory_iterator(sysfs / "class/net", ec))
    {
        // device is the USB interface; the USB device is its parent.
        const auto device =
            fs::canonical(net.path() / "device", ec).parent_path();
        if (ec || readLine(device / "idVendor") != usbVendor ||
            readLine(device / "idProduct") != usbProduct)
        {
            ec.clear();
            continue;
        }
        HmcUsb usb{net.path().filename().string(), {}, {}};
        for (auto dir = device; dir != dir.parent_path();
             dir = dir.parent_path())
        {
            const auto name = dir.filename().string();
            if (usb.rootHub.empty() && name.starts_with("usb"))
            {
                usb.rootHub = name;
            }
            if (fs::read_symlink(dir / "driver", ec).filename() ==
                "ehci-platform")
            {
                usb.controller = name;
                return usb;
            }
            ec.clear();
        }
    }
    return std::nullopt;
}

boost::asio::awaitable<void> recoverUsbConnection(
    const fs::path& sysfs, const HmcUsb& usb, const Ping& ping,
    const Sleep& sleep)
{
    std::vector<fs::path> rndis;
    std::error_code ec;
    for (const auto& d : fs::directory_iterator(sysfs / "bus/usb/devices", ec))
    {
        if (readLine(d.path() / "idVendor") == usbVendor &&
            readLine(d.path() / "idProduct") == usbProduct)
        {
            rndis.push_back(d.path());
        }
    }
    std::ranges::sort(rndis);
    for (const auto& d : rndis)
    {
        lg2::info("Resetting USB device: {DEVICE}", "DEVICE",
                  d.filename().string());
        writeSysfs(d / "authorized", "0");
        co_await sleep(resetDelay);
        writeSysfs(d / "authorized", "1");
    }

    if (!co_await waitForHmcPing(ping, sleep))
    {
        lg2::info("Resetting USB1");
        const fs::path driver = sysfs / "bus/platform/drivers/ehci-platform";
        writeSysfs(sysfs / "bus/usb/devices" / usb.rootHub / "authorized", "0");
        co_await sleep(resetDelay);
        writeSysfs(driver / "unbind", usb.controller);
        co_await sleep(resetDelay);
        writeSysfs(driver / "bind", usb.controller);
        co_await sleep(driverInitDelay);
        // Unlike the script, no address check: systemd-networkd applies the
        // link's .network, static address included, whenever hmcusb0 comes
        // back.
    }

    if (co_await waitForHmcPing(ping, sleep))
    {
        lg2::info("BMC/HMC USB Connection restored");
    }
    else
    {
        lg2::error("Failed to restore BMC/HMC USB Connection");
    }
}

Monitor::Monitor(boost::asio::io_context& io,
                 std::shared_ptr<sdbusplus::asio::connection> conn) :
    io(io), conn(std::move(conn)), timer(io), findTimer(io),
    sleep(std::bind_front(sleepFor, std::ref(timer)))
{}

Monitor::~Monitor() = default;

void Monitor::start()
{
    auto found = findHmcUsb("/sys");
    if (!found)
    {
        if (!std::exchange(waiting, true))
        {
            lg2::info("No HMC RNDIS device (USB 0525:a4a2) yet; waiting");
        }
        findTimer.expires_after(loopDelay);
        findTimer.async_wait(std::bind_front(&Monitor::retry, this));
        return;
    }
    usb = *found;
    lg2::info("HMC link {LINK} on {CONTROLLER} ({HUB})", "LINK", usb.link,
              "CONTROLLER", usb.controller, "HUB", usb.rootHub);
    // The daemon runs as root.
    socket.emplace(io, boost::asio::ip::icmp::v4());
    ping = std::bind_front(echoHmc, std::ref(*socket), std::ref(timer),
                           std::ref(sequence));
    link = std::make_unique<LinkState>(
        conn, usb.link, std::bind_front(&Monitor::linkKnown, this));
}

void Monitor::retry(const boost::system::error_code& ec)
{
    if (!ec)
    {
        start();
    }
}

void Monitor::linkKnown()
{
    // NOLINTNEXTLINE(misc-include-cleaner)
    boost::asio::co_spawn(io, monitor(*link, usb, ping, sleep),
                          boost::asio::detached);
}

} // namespace hmc_usb_monitor

int main()
{
    boost::asio::io_context io;
    // Lives until the process exits: its callbacks capture it.
    hmc_usb_monitor::Monitor monitor(
        io, std::make_shared<sdbusplus::asio::connection>(io));
    monitor.start();
    io.run();
    return 0;
}
