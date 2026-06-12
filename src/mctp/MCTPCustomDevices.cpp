#include "MCTPCustomDevices.hpp"

#include "MCTPDefinitions.hpp"
#include "MCTPEndpoint.hpp"
#include "MCTPEndpointUtils.hpp"
#include "Utils.hpp"

#include <linux/mctp.h>
#include <net/if.h> // for if_nametoindex
#include <sys/socket.h>
#include <unistd.h>

#include <boost/system/error_code.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

PHOSPHOR_LOG2_USING;

namespace
{
bool isSafeGadgetName(const std::string& name)
{
    return !name.empty() && name.size() < IFNAMSIZ &&
           std::ranges::all_of(
               name,
               [](const unsigned char ch) {
                   return std::isalnum(ch) || ch == '_' || ch == '-' ||
                          ch == '.';
               });
}
} // namespace

/* MCTP USBGadget */

USBGadgetMCTPDevice::USBGadgetMCTPDevice(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    const std::string& gadgetName, uint8_t localEID, const std::string& name) :
    connection(connection), gadgetName(gadgetName), localEID(localEID),
    name(name)
{
    if (!isSafeGadgetName(gadgetName))
    {
        throw std::invalid_argument("Bad USB gadget interface name");
    }

    info("Creating USB Gadget MCTP Device: {GADGET_NAME}, EID: {EID}",
         "GADGET_NAME", gadgetName, "EID", static_cast<int>(localEID));
}

void USBGadgetMCTPDevice::setup(
    std::function<void(const std::error_code& ec,
                       const std::shared_ptr<MCTPEndpoint>& ep)>&& added)
{
    auto onSetupComplete = std::move(added);
    info("Setting up USB Gadget: {GADGET_NAME}", "GADGET_NAME", gadgetName);

    if (isSetup)
    {
        warning("USB Gadget already setup: {GADGET_NAME}", "GADGET_NAME",
                gadgetName);
        onSetupComplete(
            std::make_error_code(std::errc::device_or_resource_busy), nullptr);
        return;
    }

    // Load libcomposite kernel module
    // TODO: remove no lint next line once we come up with a better solution
    // NOLINTNEXTLINE(cert-env33-c)
    if (std::system("modprobe libcomposite") != 0)
    {
        error("Failed to load libcomposite module");
        createMCTPLogEntry(connection, name, hmcBridgeError,
                           "Failed to load libcomposite module",
                           "If problem persists, contact next level support.");
        onSetupComplete(std::make_error_code(std::errc::io_error), nullptr);
        return;
    }

    // Create USB gadget directory
    std::error_code ec;
    std::filesystem::create_directories("/sys/kernel/config/usb_gadget/g_multi",
                                        ec);
    if (ec)
    {
        error("Failed to create USB gadget directory: {ERROR}", "ERROR",
              ec.message());
        createMCTPLogEntry(
            connection, name, hmcBridgeError,
            "Failed to create USB gadget directory: " + ec.message(),
            "If problem persists, contact next level support.");
        onSetupComplete(ec, nullptr);
        return;
    }

    // Configure USB gadget parameters
    if (!writeSysfsFile("/sys/kernel/config/usb_gadget/g_multi/idVendor",
                        "0x1d6b"))
    {
        error("Failed to set idVendor");
        createMCTPLogEntry(connection, name, hmcBridgeError,
                           "Failed to set idVendor",
                           "If problem persists, contact next level support.");
        onSetupComplete(std::make_error_code(std::errc::io_error), nullptr);
        return;
    }

    if (!writeSysfsFile("/sys/kernel/config/usb_gadget/g_multi/idProduct",
                        "0x1040"))
    {
        error("Failed to set idProduct");
        createMCTPLogEntry(connection, name, hmcBridgeError,
                           "Failed to set idProduct",
                           "If problem persists, contact next level support.");
        onSetupComplete(std::make_error_code(std::errc::io_error), nullptr);
        return;
    }

    if (!writeSysfsFile("/sys/kernel/config/usb_gadget/g_multi/bcdDevice",
                        "0x0100"))
    {
        error("Failed to set bcdDevice");
        createMCTPLogEntry(connection, name, hmcBridgeError,
                           "Failed to set bcdDevice",
                           "If problem persists, contact next level support.");
        onSetupComplete(std::make_error_code(std::errc::io_error), nullptr);
        return;
    }

    if (!writeSysfsFile("/sys/kernel/config/usb_gadget/g_multi/bcdUSB",
                        "0x0200"))
    {
        error("Failed to set bcdUSB");
        createMCTPLogEntry(connection, name, hmcBridgeError,
                           "Failed to set bcdUSB",
                           "If problem persists, contact next level support.");
        onSetupComplete(std::make_error_code(std::errc::io_error), nullptr);
        return;
    }

    // Create strings directory
    std::filesystem::create_directories(
        "/sys/kernel/config/usb_gadget/g_multi/strings/0x409", ec);
    if (ec)
    {
        error("Failed to create strings directory: {ERROR}", "ERROR",
              ec.message());
        createMCTPLogEntry(
            connection, name, hmcBridgeError,
            "Failed to create strings directory: " + ec.message(),
            "If problem persists, contact next level support.");
        onSetupComplete(ec, nullptr);
        return;
    }

    if (!writeSysfsFile(
            "/sys/kernel/config/usb_gadget/g_multi/strings/0x409/manufacturer",
            "ASPEED"))
    {
        error("Failed to set manufacturer string");
        createMCTPLogEntry(connection, name, hmcBridgeError,
                           "Failed to set manufacturer string",
                           "If problem persists, contact next level support.");
        onSetupComplete(std::make_error_code(std::errc::io_error), nullptr);
        return;
    }

    if (!writeSysfsFile(
            "/sys/kernel/config/usb_gadget/g_multi/strings/0x409/product",
            "Gadget: MCTP"))
    {
        error("Failed to set product string");
        createMCTPLogEntry(connection, hmcBridgeError, name,
                           "Failed to set product string",
                           "If problem persists, contact next level support.");
        onSetupComplete(std::make_error_code(std::errc::io_error), nullptr);
        return;
    }

    if (!writeSysfsFile(
            "/sys/kernel/config/usb_gadget/g_multi/strings/0x409/serialnumber",
            "1234567890"))
    {
        error("Failed to set serial number");
        createMCTPLogEntry(connection, hmcBridgeError, name,
                           "Failed to set serial number",
                           "If problem persists, contact next level support.");
        onSetupComplete(std::make_error_code(std::errc::io_error), nullptr);
        return;
    }

    // Create configuration
    std::filesystem::create_directories(
        "/sys/kernel/config/usb_gadget/g_multi/configs/c.1", ec);
    if (ec)
    {
        error("Failed to create config directory: {ERROR}", "ERROR",
              ec.message());
        onSetupComplete(ec, nullptr);
        return;
    }

    std::filesystem::create_directories(
        "/sys/kernel/config/usb_gadget/g_multi/configs/c.1/strings/0x409", ec);
    if (ec)
    {
        error("Failed to create config strings directory: {ERROR}", "ERROR",
              ec.message());
        createMCTPLogEntry(
            connection, name, hmcBridgeError,
            "Failed to create config strings directory: " + ec.message(),
            "If problem persists, contact next level support.");
        onSetupComplete(ec, nullptr);
        return;
    }

    if (!writeSysfsFile(
            "/sys/kernel/config/usb_gadget/g_multi/configs/c.1/strings/0x409/configuration",
            "MCTP Config"))
    {
        error("Failed to set configuration string");
        createMCTPLogEntry(connection, name, hmcBridgeError,
                           "Failed to set configuration string",
                           "If problem persists, contact next level support.");
        onSetupComplete(std::make_error_code(std::errc::io_error), nullptr);
        return;
    }

    if (!writeSysfsFile(
            "/sys/kernel/config/usb_gadget/g_multi/configs/c.1/MaxPower",
            "250"))
    {
        error("Failed to set MaxPower");
        createMCTPLogEntry(connection, name, hmcBridgeError,
                           "Failed to set MaxPower",
                           "If problem persists, contact next level support.");
        onSetupComplete(std::make_error_code(std::errc::io_error), nullptr);
        return;
    }

    // Create MCTP function
    std::filesystem::create_directories(
        "/sys/kernel/config/usb_gadget/g_multi/functions/mctp.usb0", ec);
    if (ec)
    {
        error("Failed to create MCTP function directory: {ERROR}", "ERROR",
              ec.message());
        createMCTPLogEntry(
            connection, name, hmcBridgeError,
            "Failed to create MCTP function directory: " + ec.message(),
            "If problem persists, contact next level support.");
        onSetupComplete(ec, nullptr);
        return;
    }

    // Link function to configuration
    const char* symlinkSource =
        "/sys/kernel/config/usb_gadget/g_multi/functions/mctp.usb0";
    const char* symlinkDest =
        "/sys/kernel/config/usb_gadget/g_multi/configs/c.1/mctp.usb0";

    if (symlink(symlinkSource, symlinkDest) == -1)
    {
        // Ignore EEXIST (symlink already exists from previous run)
        if (errno != EEXIST)
        {
            error("Failed to link MCTP function to config: {ERROR}", "ERROR",
                  std::strerror(errno));
            createMCTPLogEntry(
                connection, name, hmcBridgeError,
                "Failed to link MCTP function to config",
                "If problem persists, contact next level support.");
            onSetupComplete(std::make_error_code(static_cast<std::errc>(errno)),
                            nullptr);
            return;
        }

        info("MCTP function symlink already exists, continuing...");
    }

    // Check if UDC is already set to avoid error on re-bind
    bool udcAlreadySet = false;
    {
        std::ifstream udcFile("/sys/kernel/config/usb_gadget/g_multi/UDC");
        if (udcFile)
        {
            std::string currentUdc;
            std::getline(udcFile, currentUdc);
            udcAlreadySet =
                (currentUdc.find("1e6a0000.usb-vhub:p2") != std::string::npos);
        }
    }

    if (!udcAlreadySet)
    {
        if (!writeSysfsFile("/sys/kernel/config/usb_gadget/g_multi/UDC",
                            "1e6a0000.usb-vhub:p2"))
        {
            error("Failed to set UDC and enable gadget");
            createMCTPLogEntry(
                connection, name, hmcBridgeError,
                "Failed to set UDC and enable gadget",
                "If problem persists, contact next level support.");
            onSetupComplete(std::make_error_code(std::errc::io_error), nullptr);
            return;
        }
    }
    else
    {
        info("UDC already set to 1e6a0000.usb-vhub:p2, skipping...");
    }

    // Set the link up
    // TODO: remove no lint next line once we come up with a better solution
    // NOLINTNEXTLINE(cert-env33-c)
    if (std::system(
            std::format("/usr/bin/mctp link set {} up", gadgetName).c_str()) !=
        0)
    {
        error("Failed to set link up for {GADGET_NAME}: {ERROR}", "GADGET_NAME",
              gadgetName, "ERROR", std::strerror(errno));
        createMCTPLogEntry(connection, name, hmcBridgeError,
                           "Failed to set link up for " + gadgetName + ": " +
                               std::strerror(errno),
                           "If problem persists, contact next level support.");
        onSetupComplete(std::make_error_code(static_cast<std::errc>(errno)),
                        nullptr);
        return;
    }

    // Apply netfilter rules to allow MCTP traffic only for type 0 and 5
    // messages. Clear the gadget-specific table first; ignore failure if none
    // exists.
    const std::string nftBinary = "/usr/sbin/nft";
    const std::string nftTableName = "mctp_" + gadgetName;
    const std::string nftChainName = "ingress_" + gadgetName;

    // TODO: remove no lint next line once we come up with a better solution
    // NOLINTNEXTLINE(cert-env33-c)
    const int deleteTableResult = std::system(
        (nftBinary + " delete table netdev " + nftTableName +
         " > /dev/null 2>&1")
            .c_str());
    info("netfilter delete table result for {GADGET_NAME}: {RESULT}",
         "GADGET_NAME", gadgetName, "RESULT", deleteTableResult);

    // MCTP control (type 0) state-mutating ("set") command codes per DSP0236
    // 1.3.3. These commands change endpoint/bridge state, so a request carrying
    // one is only honored when it targets our local EID; the same request aimed
    // at any other EID is dropped at ingress.
    struct CtrlSetCommand
    {
        uint8_t code;
        const char* name;
        // Set Endpoint ID legitimately targets the MCTP null EID (0): that is
        // how a bus owner assigns an EID to a not-yet-enumerated endpoint. Only
        // that command exempts dest EID 0 from the drop; the others never
        // legitimately target the null EID, so they stay strict.
        bool allowNullEid;
    };
    // Routing Information Update (0x09) is intentionally NOT filtered here: it
    // is handled at the mctpd layer, which ignores an update whose advertised
    // EID is already a known peer, so dropping it at ingress is redundant and
    // would block legitimate updates.
    static constexpr std::array<CtrlSetCommand, 5> ctrlSetCommands = {{
        {0x01, "Set Endpoint ID", true},
        {0x08, "Allocate Endpoint IDs", false},
        {0x0D, "Discovery Notify", false},
        {0x12, "Request TX rate limit", false},
        {0x13, "Update rate limit", false},
    }};

    std::vector<std::string> nftCommands = {
        nftBinary + " add table netdev " + nftTableName,
        std::format(
            "{} 'add chain netdev {} {} {{ type filter hook ingress device "
            "\"{}\" priority 0; }}'",
            nftBinary, nftTableName, nftChainName, gadgetName),
    };

    // Drop type-0 control "set" requests addressed to any EID other than ours.
    // Header bit offsets into the MCTP packet (@nh = start of MCTP header):
    //   @nh,32,8  IC|MsgType byte -> 0x0 means MCTP control with IC=0
    //   @nh,24,1  start-of-message bit -> 1 (command code is in first packet)
    //   @nh,40,1  Rq bit -> 1 (request, so responses are never matched)
    //   @nh,48,8  command code
    //   @nh,8,8   destination EID -> must differ from our local EID (and, for
    //             Set Endpoint ID, also from the null EID 0, which is the valid
    //             assignment target for a not-yet-enumerated endpoint)
    for (const auto& cmd : ctrlSetCommands)
    {
        std::string destGuard =
            std::format("@nh,8,8 != {}", static_cast<int>(localEID));
        if (cmd.allowNullEid)
        {
            destGuard += " @nh,8,8 != 0x00";
        }
        nftCommands.push_back(std::format(
            "{} add rule netdev {} {} @nh,32,8 0x0 @nh,24,1 1 @nh,40,1 1 "
            "@nh,48,8 {:#04x} {} drop",
            nftBinary, nftTableName, nftChainName, static_cast<int>(cmd.code),
            destGuard));
    }

    // Allow remaining MCTP control (type 0) and type 5 traffic; drop the rest.
    nftCommands.push_back(nftBinary + " add rule netdev " + nftTableName + " " +
                          nftChainName + " @nh,32,8 0x0 accept");
    nftCommands.push_back(nftBinary + " add rule netdev " + nftTableName + " " +
                          nftChainName + " @nh,32,8 0x5 accept");
    nftCommands.push_back(nftBinary + " add rule netdev " + nftTableName + " " +
                          nftChainName + " drop");

    for (const auto& command : nftCommands)
    {
        // TODO: remove no lint next line once we come up with a better solution
        // NOLINTNEXTLINE(cert-env33-c)
        if (std::system(command.c_str()) != 0)
        {
            warning(
                "Failed to apply netfilter rule for {GADGET_NAME}: {COMMAND}",
                "GADGET_NAME", gadgetName, "COMMAND", command);
            break;
        }
    }

    // Set local MCTP address (udev rule might get delayed)
    // TODO: remove no lint next line once we come up with a better solution
    // NOLINTNEXTLINE(cert-env33-c)
    if (std::system(std::format("/usr/bin/mctp addr add {} dev {}", localEID,
                                gadgetName)
                        .c_str()) != 0)
    {
        if (errno != EEXIST)
        {
            error("Failed to add MCTP address to {GADGET_NAME}: {ERROR}",
                  "GADGET_NAME", gadgetName, "ERROR", std::strerror(errno));
            createMCTPLogEntry(
                connection, name, hmcBridgeError,
                "Failed to add MCTP address to " + gadgetName + ": " +
                    std::strerror(errno),
                "If problem persists, contact next level support.");
            onSetupComplete(std::make_error_code(static_cast<std::errc>(errno)),
                            nullptr);
            return;
        }
        info("MCTP address already exists, continuing...");
    }

    // Set the role to Endpoint mode
    if (!setRoleEndpoint())
    {
        onSetupComplete(std::make_error_code(std::errc::io_error), nullptr);
        createMCTPLogEntry(connection, name, hmcBridgeError,
                           "Failed to set role to Endpoint mode",
                           "If problem persists, contact next level support.");
        warning("Failed to set role to Endpoint mode");
        return;
    }

    isSetup = true;
    info("USB gadget feature enabled successfully");
    createMCTPLogEntry(connection, name, hmcBridgeInfo,
                       "USB gadget feature enabled successfully",
                       "If problem persists, contact next level support.");

    onSetupComplete(std::error_code{}, shared_from_this());
}

bool USBGadgetMCTPDevice::setRoleEndpoint()
{
    std::string interfacePath =
        std::format("{}/interfaces/{}", mctpdControlPath, gadgetName);
    try
    {
        auto method = connection->new_method_call(
            mctpdBusName, interfacePath.c_str(),
            "org.freedesktop.DBus.Properties", "Set");
        method.append("au.com.codeconstruct.MCTP.Interface1", "Role",
                      std::variant<std::string>("Endpoint"));
        connection->call(method);
    }
    catch (const std::exception& e)
    {
        warning("Exception while setting Role property: {ERROR}", "ERROR",
                e.what());
        return false;
    }
    return true;
}

void USBGadgetMCTPDevice::remove()
{
    info("Removing USB Gadget: {GADGET_NAME}", "GADGET_NAME", gadgetName);

    if (notifyRemoved)
    {
        notifyRemoved(shared_from_this());
    }

    // TODO: Implement gadget cleanup
    isSetup = false;
}

std::string USBGadgetMCTPDevice::describe() const
{
    return std::format("USBGadget[{}, EID={}]", gadgetName,
                       static_cast<int>(localEID));
}

std::optional<std::string> USBGadgetMCTPDevice::getNameForEid(uint8_t eid) const
{
    if (eid == localEID)
    {
        return name.empty() ? gadgetName : name;
    }
    return std::nullopt;
}

int USBGadgetMCTPDevice::network() const
{
    // USB gadget's network ID, typically 1
    return 1;
}

uint8_t USBGadgetMCTPDevice::eid() const
{
    return localEID;
}

std::size_t USBGadgetMCTPDevice::id() const
{
    std::size_t h1 = std::hash<std::string>{}(name);
    std::size_t h2 = std::hash<std::string>{}(gadgetName);
    return h1 ^ (h2 << 1) ^ (static_cast<std::size_t>(localEID) << 2);
}

void USBGadgetMCTPDevice::subscribe([[maybe_unused]] Event&& degraded,
                                    [[maybe_unused]] Event&& available,
                                    Event&& removed)
{
    if (!isSetup)
    {
        warning("USB Gadget not setup, skipping subscription");
        return;
    }

    notifyRemoved = std::move(removed);
    using namespace sdbusplus::bus::match;

    info("Setting up MCTP endpoint monitoring for USB Gadget");

    const std::string addedMatchSpec =
        rules::sender(mctpdBusName) +
        rules::interfacesAddedAtPath(mctpdEndpointPath);

    endpointAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*connection), addedMatchSpec,
        [weak{weak_from_this()}](sdbusplus::message_t& msg) {
            if (auto self = weak.lock())
            {
                self->onEndpointAdded(msg);
            }
        });

    const std::string removedMatchSpec =
        rules::sender(mctpdBusName) +
        rules::interfacesRemovedAtPath(mctpdEndpointPath);

    endpointRemovedMatch = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*connection), removedMatchSpec,
        [weak{weak_from_this()}](sdbusplus::message_t& msg) {
            if (auto self = weak.lock())
            {
                self->onEndpointRemoved(msg);
            }
        });

    connection->async_method_call(
        [weak{weak_from_this()}](const boost::system::error_code& ec,
                                 const ManagedObjectType& objects) {
            if (auto self = weak.lock())
            {
                if (ec)
                {
                    error("Failed to get managed objects from mctpd: {ERROR}",
                          "ERROR", ec.message());
                    return;
                }

                auto networkIt =
                    objects.find(sdbusplus::object_path(mctpdNetworkPath));
                if (networkIt != objects.end())
                {
                    const auto& interfaces = networkIt->second;
                    auto ifaceIt = interfaces.find(mctpdNetworkInterface);
                    if (ifaceIt != interfaces.end())
                    {
                        const auto& properties = ifaceIt->second;
                        auto propIt = properties.find("LocalEIDs");
                        if (propIt != properties.end())
                        {
                            const auto* eids =
                                std::get_if<std::vector<uint8_t>>(
                                    &propIt->second);
                            if (eids)
                            {
                                self->netLocalEIDs.clear();
                                for (auto eid : *eids)
                                {
                                    std::string endpointPath = std::format(
                                        "{}{}", mctpdEndpointPath, eid);
                                    self->netLocalEIDs.insert(endpointPath);
                                }
                            }
                        }
                    }
                }
            }
        },
        mctpdBusName, mctpdControlPath, mctpdObjectManagerInterface,
        "GetManagedObjects");

    info("Subscribed to MCTP endpoint Added/Removed signals");
}

void USBGadgetMCTPDevice::sendDiscoveryNotify()
{
    int sd = socket(AF_MCTP, SOCK_DGRAM, 0);
    if (sd < 0)
    {
        error("Failed to create MCTP socket for Discovery Notify: {ERROR}",
              "ERROR", strerror(errno));
        return;
    }

    int opt = 1;
    if (setsockopt(sd, SOL_MCTP, MCTP_OPT_ADDR_EXT, &opt, sizeof(opt)) < 0)
    {
        error("Failed to enable extended addressing: {ERROR}", "ERROR",
              strerror(errno));
        close(sd);
        return;
    }

    unsigned int ifindex = if_nametoindex(gadgetName.c_str());
    if (ifindex == 0)
    {
        error("Failed to get interface index for {GADGET_NAME}: {ERROR}",
              "GADGET_NAME", gadgetName, "ERROR", strerror(errno));
        close(sd);
        return;
    }

    // Construct Discovery Notify message
    // 0x80 = Request, non-datagram, instance ID 0
    // 0x0D = MCTP_CTRL_CMD_DISCOVERY_NOTIFY
    std::array<uint8_t, 2> discoveryNotifyMsg = {0x80, 0x0D};
    struct sockaddr_mctp_ext addr = {};
    addr.smctp_base.smctp_family = AF_MCTP;
    addr.smctp_base.smctp_network = 1;
    addr.smctp_base.smctp_addr.s_addr = 0;
    addr.smctp_base.smctp_type = 0;
    addr.smctp_base.smctp_tag = MCTP_TAG_OWNER;
    addr.smctp_ifindex = ifindex;
    addr.smctp_halen = 0;

    // TODO: remove no lint next line once we come up with a better solution
    ssize_t len =
        sendto(sd, discoveryNotifyMsg.data(), discoveryNotifyMsg.size(), 0,
               // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
               reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

    if (len < 0)
    {
        error("Failed to send Discovery Notify to {GADGET_NAME}: {ERROR}",
              "GADGET_NAME", gadgetName, "ERROR", strerror(errno));
    }
    else
    {
        info("Successfully sent Discovery Notify to {GADGET_NAME}",
             "GADGET_NAME", gadgetName);
    }

    close(sd);
}

void USBGadgetMCTPDevice::onEndpointAdded(sdbusplus::message_t& msg)
{
    auto [path, interfaces] = msg.unpack<sdbusplus::object_path, SensorData>();

    if (interfaces.find(mctpdEndpointControlInterface) == interfaces.end())
    {
        return;
    }

    if (netLocalEIDs.contains(path.str))
    {
        return;
    }

    sendDiscoveryNotify();
}

void USBGadgetMCTPDevice::onEndpointRemoved(sdbusplus::message_t& msg)
{
    auto [path, interfaces] =
        msg.unpack<sdbusplus::object_path, std::set<std::string>>();

    if (!interfaces.contains(mctpdEndpointControlInterface))
    {
        return;
    }

    if (netLocalEIDs.contains(path.str))
    {
        return;
    }

    sendDiscoveryNotify();
}

bool USBGadgetMCTPDevice::match(const std::set<std::string>& interfaces)
{
    return interfaces.contains(configInterfaceName(configType));
}

std::optional<SensorBaseConfigMap> USBGadgetMCTPDevice::match(
    const SensorData& config)
{
    auto iface = config.find(configInterfaceName(configType));
    if (iface == config.end())
    {
        return std::nullopt;
    }
    return iface->second;
}

std::shared_ptr<USBGadgetMCTPDevice> USBGadgetMCTPDevice::from(
    const std::shared_ptr<sdbusplus::asio::connection>& connection,
    const SensorBaseConfigMap& iface)
{
    auto mType = iface.find("Type");
    if (mType == iface.end())
    {
        throw std::invalid_argument(
            "No 'Type' member found for provided configuration object");
    }

    auto type = std::visit(VariantToStringVisitor(), mType->second);
    if (type != configType)
    {
        throw std::invalid_argument("Not an USB Gadget MCTP device");
    }

    auto mName = iface.find("Name");
    auto mLocalEID = iface.find("LocalEID");
    auto mInterface = iface.find("Interface");

    if (mName == iface.end() || mInterface == iface.end() ||
        mLocalEID == iface.end())
    {
        throw std::invalid_argument(
            "Configuration object violates MCTPUSBGadgetTarget schema");
    }

    std::string name = std::visit(VariantToStringVisitor(), mName->second);
    std::string interface =
        std::visit(VariantToStringVisitor(), mInterface->second);
    auto sLocalEID = std::visit(VariantToStringVisitor(), mLocalEID->second);
    std::uint8_t parsedLocalEID{};

    auto [cptr, cec] = std::from_chars(
        sLocalEID.data(), sLocalEID.data() + sLocalEID.size(), parsedLocalEID);
    if (cec != std::errc{} || parsedLocalEID > 0xfe || parsedLocalEID < 0x08)
    {
        throw std::invalid_argument("Bad local EID");
    }

    try
    {
        return std::make_shared<USBGadgetMCTPDevice>(connection, interface,
                                                     parsedLocalEID, name);
    }
    catch (const MCTPException& ex)
    {
        warning(
            "Failed to create USBGadgetMCTPDevice at [ name: {GADGET_NAME} interface: {INTERFACE} ]: {EXCEPTION}",
            "GADGET_NAME", name, "INTERFACE", interface, "EXCEPTION", ex);
        return {};
    }
}
