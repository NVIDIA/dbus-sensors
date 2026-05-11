#include "USBRecovery.hpp"

#include "LibusbSystem.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

PHOSPHOR_LOG2_USING;

namespace
{
constexpr uint8_t mctpInterfaceClass = 0x14;

struct USBDeviceLocation
{
    uint8_t bus = 0;
    std::vector<uint8_t> ports;
    std::optional<uint8_t> deviceAddress;
};

struct BulkOutEndpoint
{
    uint8_t endpointAddress = 0;
    int interfaceNumber = -1;
};

class ScopedFd
{
  public:
    ScopedFd() = default;
    explicit ScopedFd(int fdValue) : fd(fdValue) {}

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept : fd(other.fd)
    {
        other.fd = -1;
    }

    ScopedFd& operator=(ScopedFd&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }

    ~ScopedFd()
    {
        reset();
    }

    int get() const
    {
        return fd;
    }

    void reset(int newFd = -1)
    {
        if (fd >= 0)
        {
            close(fd);
        }
        fd = newFd;
    }

  private:
    int fd = -1;
};

std::string formatLibusbError(const std::string& prefix, int rc)
{
    return prefix + ": " + std::string(libusb_error_name(rc)) + " (" +
           std::to_string(rc) + ")";
}

std::optional<std::string> readTrimmedLine(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    if (!stream.good())
    {
        return std::nullopt;
    }

    std::string value;
    std::getline(stream, value);
    value.erase(std::remove_if(value.begin(), value.end(),
                               [](unsigned char ch) {
                                   return ch == '\n' || ch == '\r' ||
                                          ch == ' ' || ch == '\t';
                               }),
                value.end());
    if (value.empty())
    {
        return std::nullopt;
    }

    return value;
}

bool parseUint8(std::string_view text, uint8_t& output)
{
    unsigned int value = 0;
    auto [ptr,
          ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || value > 255U)
    {
        return false;
    }
    output = static_cast<uint8_t>(value);
    return true;
}

std::optional<std::vector<uint8_t>> parsePortPath(const std::string& devpath)
{
    std::vector<uint8_t> ports;
    std::stringstream ss(devpath);
    std::string token;
    while (std::getline(ss, token, '.'))
    {
        if (token.empty())
        {
            return std::nullopt;
        }

        uint8_t port = 0;
        if (!parseUint8(token, port))
        {
            return std::nullopt;
        }
        ports.push_back(port);
    }

    if (ports.empty())
    {
        return std::nullopt;
    }
    return ports;
}

std::optional<USBDeviceLocation> resolveUSBDeviceLocationForInterface(
    const std::string& interface, std::string& status,
    const std::filesystem::path& netClassRoot = "/sys/class/net")
{
    info("Resolving USB location for interface {USB_INTERFACE}",
         "USB_INTERFACE", interface);
    std::error_code ec;
    auto interfacePath = std::filesystem::weakly_canonical(
        netClassRoot / interface / "device", ec);
    if (ec)
    {
        status = "failed to resolve sysfs path";
        return std::nullopt;
    }

    auto path = interfacePath;
    while (true)
    {
        auto busnumPath = path / "busnum";
        auto devpathPath = path / "devpath";
        if (std::filesystem::exists(busnumPath, ec) &&
            std::filesystem::exists(devpathPath, ec))
        {
            auto devnumPath = path / "devnum";
            auto busnumValue = readTrimmedLine(busnumPath);
            auto devpathValue = readTrimmedLine(devpathPath);
            if (!busnumValue || !devpathValue)
            {
                status = "busnum/devpath missing or unreadable";
                warning(
                    "Failed to read busnum/devpath for interface {USB_INTERFACE}",
                    "USB_INTERFACE", interface);
                return std::nullopt;
            }

            uint8_t bus = 0;
            if (!parseUint8(*busnumValue, bus))
            {
                status = "invalid busnum content";
                warning(
                    "Invalid busnum '{BUSNUM}' for interface {USB_INTERFACE}",
                    "BUSNUM", *busnumValue, "USB_INTERFACE", interface);
                return std::nullopt;
            }

            auto ports = parsePortPath(*devpathValue);
            if (!ports)
            {
                status = "invalid devpath content";
                warning(
                    "Invalid devpath '{DEVPATH}' for interface {USB_INTERFACE}",
                    "DEVPATH", *devpathValue, "USB_INTERFACE", interface);
                return std::nullopt;
            }

            std::optional<uint8_t> deviceAddress;
            if (std::filesystem::exists(devnumPath, ec))
            {
                auto devnumValue = readTrimmedLine(devnumPath);
                uint8_t parsedDevnum = 0;
                if (devnumValue && parseUint8(*devnumValue, parsedDevnum))
                {
                    deviceAddress = parsedDevnum;
                }
            }

            info(
                "Resolved USB interface {USB_INTERFACE}: bus={USB_BUS}, devpath={USB_DEVPATH}, devnum={USB_DEVNUM}",
                "USB_INTERFACE", interface, "USB_BUS",
                static_cast<unsigned int>(bus), "USB_DEVPATH", *devpathValue,
                "USB_DEVNUM",
                deviceAddress
                    ? std::to_string(static_cast<unsigned int>(*deviceAddress))
                    : std::string("unknown"));
            return USBDeviceLocation{bus, *ports, deviceAddress};
        }

        auto parent = path.parent_path();
        if (parent.empty() || parent == path)
        {
            break;
        }
        path = parent;
    }

    status = "no USB busnum/devpath found in sysfs ancestry";
    warning(
        "Could not resolve USB location for interface {USB_INTERFACE}: {RECOVERY_STATUS}",
        "USB_INTERFACE", interface, "RECOVERY_STATUS", status);
    return std::nullopt;
}

std::optional<BulkOutEndpoint> findBulkOutEndpoint(libusb_device* device)
{
    libusb_config_descriptor* config = nullptr;
    int rc = libusb_get_active_config_descriptor(device, &config);
    if (rc != LIBUSB_SUCCESS)
    {
        rc = libusb_get_config_descriptor(device, 0, &config);
        if (rc != LIBUSB_SUCCESS)
        {
            return std::nullopt;
        }
    }

    std::optional<BulkOutEndpoint> endpoint;
    for (uint8_t i = 0; i < config->bNumInterfaces && !endpoint; ++i)
    {
        const auto& iface = config->interface[i];
        for (int alt = 0; alt < iface.num_altsetting && !endpoint; ++alt)
        {
            const auto& altsetting = iface.altsetting[alt];
            if (altsetting.bInterfaceClass != mctpInterfaceClass)
            {
                continue;
            }
            for (uint8_t e = 0; e < altsetting.bNumEndpoints; ++e)
            {
                const auto& ep = altsetting.endpoint[e];
                const uint8_t transferType =
                    ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                const uint8_t direction =
                    ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK;
                if (transferType == LIBUSB_TRANSFER_TYPE_BULK &&
                    direction == LIBUSB_ENDPOINT_OUT)
                {
                    endpoint = BulkOutEndpoint{ep.bEndpointAddress,
                                               altsetting.bInterfaceNumber};
                    break;
                }
            }
        }
    }

    libusb_free_config_descriptor(config);
    return endpoint;
}

std::string buildLocationString(const USBDeviceLocation& location)
{
    std::ostringstream out;
    out << "bus " << static_cast<unsigned int>(location.bus) << ", ports ";
    for (size_t i = 0; i < location.ports.size(); ++i)
    {
        if (i > 0)
        {
            out << '.';
        }
        out << static_cast<unsigned int>(location.ports[i]);
    }
    if (location.deviceAddress)
    {
        out << ", devnum "
            << static_cast<unsigned int>(location.deviceAddress.value());
    }
    return out.str();
}

std::optional<std::string> buildUsbfsNodePath(
    const USBDeviceLocation& location,
    const std::filesystem::path& usbfsRoot = "/dev/bus/usb")
{
    if (!location.deviceAddress)
    {
        return std::nullopt;
    }

    std::ostringstream bus;
    bus << std::setw(3) << std::setfill('0')
        << static_cast<unsigned int>(location.bus);
    std::ostringstream device;
    device << std::setw(3) << std::setfill('0')
           << static_cast<unsigned int>(location.deviceAddress.value());
    return (usbfsRoot / bus.str() / device.str()).string();
}

bool openHandleDirectFromUsbfs(
    libusb_context* context, const USBDeviceLocation& location,
    libusb_device_handle** handle, ScopedFd& wrappedFd, std::string& status,
    const std::filesystem::path& usbfsRoot = "/dev/bus/usb")
{
    auto usbfsPath = buildUsbfsNodePath(location, usbfsRoot);
    if (!usbfsPath)
    {
        status = "devnum unavailable for direct usbfs open";
        return false;
    }

    info("Opening USB node {USBFS_NODE}", "USBFS_NODE", *usbfsPath);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    ScopedFd fd(open(usbfsPath->c_str(), O_RDWR | O_CLOEXEC));
    if (fd.get() < 0)
    {
        status = "open(" + *usbfsPath + ") failed: " + std::strerror(errno);
        return false;
    }

    int rc = libusb_wrap_sys_device(context, static_cast<intptr_t>(fd.get()),
                                    handle);
    if (rc != LIBUSB_SUCCESS)
    {
        status = "libusb_wrap_sys_device failed: " +
                 std::string(libusb_error_name(rc));
        return false;
    }

    wrappedFd = std::move(fd);

    info("Wrapped USB node {USBFS_NODE} into libusb handle", "USBFS_NODE",
         *usbfsPath);
    status.clear();
    return true;
}

// Performs the clear-halt sequence on an already-opened device handle:
// discover the MCTP bulk OUT endpoint, (optionally) detach the kernel driver,
// claim the interface, clear the halt, then release/re-attach. Ownership of
// the handle (and its libusb context) remains with the caller. Returns true
// on success, setting status to a human-readable result either way.
bool clearHaltOnHandle(libusb_device_handle* handle,
                       const USBDeviceLocation& location, std::string& status)
{
    libusb_device* device = libusb_get_device(handle);
    if (device == nullptr)
    {
        status = "libusb_get_device returned null";
        return false;
    }

    auto endpoint = findBulkOutEndpoint(device);
    if (!endpoint)
    {
        status = "No MCTP class bulk OUT endpoint on " +
                 buildLocationString(location);
        return false;
    }
    info(
        "Found MCTP bulk OUT endpoint 0x{USB_ENDPOINT} on interface number {USB_IFACE_NUM}",
        "USB_ENDPOINT", static_cast<unsigned int>(endpoint->endpointAddress),
        "USB_IFACE_NUM", endpoint->interfaceNumber);

    bool detachedKernelDriver = false;
    int kernelDriverActive =
        libusb_kernel_driver_active(handle, endpoint->interfaceNumber);
    if (kernelDriverActive == 1)
    {
        info("Kernel driver is active on interface {USB_IFACE_NUM}, detaching",
             "USB_IFACE_NUM", endpoint->interfaceNumber);
        int detachRc =
            libusb_detach_kernel_driver(handle, endpoint->interfaceNumber);
        if (detachRc != LIBUSB_SUCCESS)
        {
            status = formatLibusbError(
                "libusb_detach_kernel_driver failed on interface " +
                    std::to_string(endpoint->interfaceNumber),
                detachRc);
            return false;
        }
        detachedKernelDriver = true;
    }
    else if (kernelDriverActive < 0 &&
             kernelDriverActive != LIBUSB_ERROR_NOT_SUPPORTED)
    {
        warning(
            "Could not determine kernel driver state for interface {USB_IFACE_NUM}: {LIBUSB_ERROR}",
            "USB_IFACE_NUM", endpoint->interfaceNumber, "LIBUSB_ERROR",
            libusb_error_name(kernelDriverActive));
    }

    bool claimed = false;
    int rc = libusb_claim_interface(handle, endpoint->interfaceNumber);
    if (rc == LIBUSB_SUCCESS)
    {
        claimed = true;
    }
    else
    {
        warning(
            "Failed to claim USB interface {USB_IFACE_NUM} before clear-halt: {LIBUSB_ERROR}",
            "USB_IFACE_NUM", endpoint->interfaceNumber, "LIBUSB_ERROR",
            libusb_error_name(rc));
        status =
            formatLibusbError("libusb_claim_interface failed on interface " +
                                  std::to_string(endpoint->interfaceNumber),
                              rc);
        if (detachedKernelDriver)
        {
            int attachRc =
                libusb_attach_kernel_driver(handle, endpoint->interfaceNumber);
            if (attachRc != LIBUSB_SUCCESS)
            {
                warning(
                    "Failed to re-attach kernel driver on interface {USB_IFACE_NUM}: {LIBUSB_ERROR}",
                    "USB_IFACE_NUM", endpoint->interfaceNumber, "LIBUSB_ERROR",
                    libusb_error_name(attachRc));
            }
        }
        return false;
    }

    info("Issuing clear-halt on endpoint 0x{USB_ENDPOINT}", "USB_ENDPOINT",
         static_cast<unsigned int>(endpoint->endpointAddress));
    rc = libusb_clear_halt(handle, endpoint->endpointAddress);
    if (claimed)
    {
        libusb_release_interface(handle, endpoint->interfaceNumber);
    }
    if (detachedKernelDriver)
    {
        int attachRc =
            libusb_attach_kernel_driver(handle, endpoint->interfaceNumber);
        if (attachRc != LIBUSB_SUCCESS)
        {
            warning(
                "Failed to re-attach kernel driver on interface {USB_IFACE_NUM}: {LIBUSB_ERROR}",
                "USB_IFACE_NUM", endpoint->interfaceNumber, "LIBUSB_ERROR",
                libusb_error_name(attachRc));
        }
        else
        {
            info("Re-attached kernel driver on interface {USB_IFACE_NUM}",
                 "USB_IFACE_NUM", endpoint->interfaceNumber);
        }
    }

    if (rc != LIBUSB_SUCCESS)
    {
        status = formatLibusbError(
            "libusb_clear_halt failed on endpoint 0x" +
                [&]() {
                    std::ostringstream ep;
                    ep << std::hex
                       << static_cast<unsigned int>(endpoint->endpointAddress);
                    return ep.str();
                }() +
                " (interface " + std::to_string(endpoint->interfaceNumber) +
                ")",
            rc);
        return false;
    }

    status = "Cleared halt on endpoint 0x" +
             [&]() {
                 std::ostringstream ep;
                 ep << std::hex
                    << static_cast<unsigned int>(endpoint->endpointAddress);
                 return ep.str();
             }() +
             " for " + buildLocationString(location);
    return true;
}

} // namespace

bool LibusbUSBRecovery::clearBulkOutHalt(const std::string& interface,
                                         std::string& status)
{
    info("Starting USB clear-halt recovery for interface {USB_INTERFACE}",
         "USB_INTERFACE", interface);
    auto location = resolveUSBDeviceLocationForInterface(interface, status);
    if (!location)
    {
        return false;
    }

    libusb_context* context = nullptr;
    int rc = libusb_init(&context);
    if (rc != LIBUSB_SUCCESS)
    {
        status = "libusb_init failed: " + std::string(libusb_error_name(rc));
        return false;
    }

    libusb_device_handle* handle = nullptr;
    ScopedFd wrappedFd;
    if (!openHandleDirectFromUsbfs(context, *location, &handle, wrappedFd,
                                   status))
    {
        status = "Direct USB open failed for " +
                 buildLocationString(*location) + ": " + status;
        libusb_exit(context);
        return false;
    }

    bool cleared = clearHaltOnHandle(handle, *location, status);

    libusb_close(handle);
    libusb_exit(context);

    if (cleared)
    {
        info(
            "Completed USB clear-halt recovery for interface {USB_INTERFACE}: {RECOVERY_STATUS}",
            "USB_INTERFACE", interface, "RECOVERY_STATUS", status);
    }
    return cleared;
}
