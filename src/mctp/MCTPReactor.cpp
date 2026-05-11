#include "MCTPReactor.hpp"

#include "MCTPDeviceRepository.hpp"
#include "MCTPEndpoint.hpp"
#include "Utils.hpp"

#include <boost/system/detail/error_code.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <charconv>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

PHOSPHOR_LOG2_USING;

namespace
{
constexpr const char* mctpdEndpointControlIface =
    "au.com.codeconstruct.MCTP.Endpoint1";

std::optional<uint8_t> eidFromMctpdEndpointPath(const std::string& path)
{
    constexpr std::string_view needle = "/endpoints/";
    const auto pos = path.find(needle);
    if (pos == std::string::npos)
    {
        return std::nullopt;
    }
    const char* begin = path.data() + pos + needle.size();
    const char* end = path.data() + path.size();
    unsigned long v = 0;
    auto [ptr, ec] = std::from_chars(begin, end, v);
    if (ec != std::errc{} || ptr != end || v > 255)
    {
        return std::nullopt;
    }
    return static_cast<uint8_t>(v);
}
} // namespace

void MCTPReactor::deferSetup(const std::shared_ptr<MCTPDevice>& dev)
{
    debug("Deferring setup for MCTP device at [ {MCTP_DEVICE} ]", "MCTP_DEVICE",
          dev->describe());

    // Track failures so we can suppress logs in the signal handler
    failureCounts[dev]++;
    trackUsbSetupFailure(dev);
    deferred.emplace(dev);
}

void MCTPReactor::trackUsbSetupFailure(const std::shared_ptr<MCTPDevice>& dev)
{
    auto usbDevice = std::dynamic_pointer_cast<USBMCTPDDevice>(dev);
    if (!usbDevice)
    {
        return;
    }

    const int recoveryThreshold = usbDevice->getRecoveryThreshold();
    if (recoveryThreshold == 0)
    {
        // RecoveryThreshold=0 disables auto USB recovery for this target.
        usbSetupFailureCounts.erase(dev);
        return;
    }

    int& failures = usbSetupFailureCounts[dev];
    failures++;
    info(
        "USB setup failure recorded for interface {USB_INTERFACE}: count {FAILURE_COUNT}/{FAILURE_THRESHOLD}",
        "USB_INTERFACE", usbDevice->getInterface(), "FAILURE_COUNT", failures,
        "FAILURE_THRESHOLD", recoveryThreshold);
    if (failures < recoveryThreshold)
    {
        return;
    }

    info(
        "USB setup failure threshold reached for interface {USB_INTERFACE}; triggering clear-halt recovery",
        "USB_INTERFACE", usbDevice->getInterface());
    failures = 0;
    if (!autoUSBRecoveryEnabled)
    {
        info(
            "USB clear-halt recovery is disabled at runtime; skipping recovery for interface {USB_INTERFACE}",
            "USB_INTERFACE", usbDevice->getInterface());
        return;
    }

    std::string recoveryStatus;
    if (usbRecovery && usbRecovery->clearBulkOutHalt(usbDevice->getInterface(),
                                                     recoveryStatus))
    {
        info(
            "USB recovery succeeded for interface {USB_INTERFACE} after setup failures: {RECOVERY_STATUS}",
            "USB_INTERFACE", usbDevice->getInterface(), "RECOVERY_STATUS",
            recoveryStatus);
        return;
    }

    warning(
        "USB recovery failed for interface {USB_INTERFACE} after setup failures: {RECOVERY_STATUS}",
        "USB_INTERFACE", usbDevice->getInterface(), "RECOVERY_STATUS",
        recoveryStatus);
}

void MCTPReactor::clearUsbSetupFailureTracking(
    const std::shared_ptr<MCTPDevice>& dev)
{
    usbSetupFailureCounts.erase(dev);
}

void MCTPReactor::untrackEndpoint(const std::shared_ptr<MCTPEndpoint>& ep)
{
    server.disassociate(MCTPDEndpoint::path(ep));
}

void MCTPReactor::trackEndpoint(const std::shared_ptr<MCTPEndpoint>& ep)
{
    info("Added MCTP endpoint to device: [ {MCTP_ENDPOINT} ]", "MCTP_ENDPOINT",
         ep->describe());

    ep->subscribe(
        // Degraded
        [](const std::shared_ptr<MCTPEndpoint>& ep) {
            debug("Endpoint entered degraded state: [ {MCTP_ENDPOINT} ]",
                  "MCTP_ENDPOINT", ep->describe());
        },
        // Available
        [](const std::shared_ptr<MCTPEndpoint>& ep) {
            debug("Endpoint entered available state: [ {MCTP_ENDPOINT} ]",
                  "MCTP_ENDPOINT", ep->describe());
        },
        // Removed
        [weak{weak_from_this()}](const std::shared_ptr<MCTPEndpoint>& ep) {
            info("Removed MCTP endpoint from device: [ {MCTP_ENDPOINT} ]",
                 "MCTP_ENDPOINT", ep->describe());
            if (auto self = weak.lock())
            {
                self->untrackEndpoint(ep);
                // Only defer the setup if we know inventory is still present
                if (self->devices.contains(ep->device()))
                {
                    self->deferSetup(ep->device());
                }
            }
            else
            {
                info(
                    "The reactor object was destroyed concurrent to the removal of the remove match for the endpoint '{MCTP_ENDPOINT}'",
                    "MCTP_ENDPOINT", ep->describe());
            }
        });

    // Proxy-host the association back to the inventory at the same path as the
    // endpoint in mctpd.
    //
    // clang-format off
    // ```
    // # busctl call xyz.openbmc_project.ObjectMapper /xyz/openbmc_project/object_mapper xyz.openbmc_project.ObjectMapper GetAssociatedSubTree ooias /au/com/codeconstruct/mctp1/networks/1/endpoints/9/configured_by / 0 1 xyz.openbmc_project.Configuration.MCTPDevice
    // a{sa{sas}} 1 "/xyz/openbmc_project/inventory/system/nvme/NVMe_1/NVMe_1_Temp" 1 "xyz.openbmc_project.EntityManager" 1 "xyz.openbmc_project.Configuration.MCTPDevice"
    // ```
    // clang-format on
    std::optional<std::string> item = devices.inventoryFor(ep->device());
    if (!item)
    {
        error("Inventory missing for endpoint: [ {MCTP_ENDPOINT} ]",
              "MCTP_ENDPOINT", ep->describe());
        return;
    }
    std::vector<Association> associations{
        {"configured_by", "configures", *item}};
    server.associate(MCTPDEndpoint::path(ep), associations);
}

void MCTPReactor::setupEndpoint(const std::shared_ptr<MCTPDevice>& dev)
{
    debug(
        "Attempting to setup up MCTP endpoint for device at [ {MCTP_DEVICE} ]",
        "MCTP_DEVICE", dev->describe());
    dev->setup([weak{weak_from_this()},
                dev](const std::error_code& ec,
                     const std::shared_ptr<MCTPEndpoint>& ep) mutable {
        auto self = weak.lock();
        if (!self)
        {
            info(
                "The reactor object was destroyed concurrent to the completion of the endpoint setup for '{MCTP_ENDPOINT}'",
                "MCTP_ENDPOINT", ep->describe());
            return;
        }

        if (ec)
        {
            debug(
                "Setup failed for MCTP device at [ {MCTP_DEVICE} ]: {ERROR_MESSAGE}",
                "MCTP_DEVICE", dev->describe(), "ERROR_MESSAGE", ec.message());

            self->deferSetup(dev);
            return;
        }

        // Clear failure count on success
        self->failureCounts.erase(dev);
        self->clearUsbSetupFailureTracking(dev);

        if (!ep)
        {
            info(
                "Ignoring already discovered endpoint for MCTP device at [ {MCTP_DEVICE} ]",
                "MCTP_DEVICE", dev->describe());
            return;
        }

        try
        {
            self->trackEndpoint(ep);
        }
        catch (const MCTPException& e)
        {
            error("Failed to track endpoint '{MCTP_ENDPOINT}': {EXCEPTION}",
                  "MCTP_ENDPOINT", ep->describe(), "EXCEPTION", e);
            self->deferSetup(dev);
        }
    });
}

bool MCTPReactor::isRetrying(uint8_t eid) const
{
    // For broadcast/unknown EID (0), check if any device is retrying
    if (eid == 0)
    {
        return !failureCounts.empty();
    }

    // Check if this specific EID's device is in failureCounts
    for (const auto& [device, count] : failureCounts)
    {
        auto mctpDevice = std::dynamic_pointer_cast<MCTPDDevice>(device);
        if (mctpDevice)
        {
            if (mctpDevice->managesEid(eid))
            {
                return true;
            }
        }
    }
    return false;
}

void MCTPReactor::tick()
{
    auto toSetup = std::exchange(deferred, {});
    for (const auto& entry : toSetup)
    {
        setupEndpoint(entry);
    }
}

void MCTPReactor::manageMCTPDevice(const std::string& path,
                                   const std::shared_ptr<MCTPDevice>& device)
{
    if (!device)
    {
        return;
    }

    try
    {
        devices.add(path, device);
        info("MCTP device inventory added at '{INVENTORY_PATH}'",
             "INVENTORY_PATH", path);
        if (auto mctpDevice = std::dynamic_pointer_cast<MCTPDDevice>(device))
        {
            // There could be case where Discovery Notify is expected to do
            // device discovery thus setup match rule before hand and setup
            // callback for the same
            mctpDevice->onDiscoveryMatchRule();
            mctpDevice->setRequestSetupCallback(
                [weak{weak_from_this()}](
                    const std::shared_ptr<MCTPDDevice>& requestingDevice) {
                    auto self = weak.lock();
                    if (!self)
                    {
                        return;
                    }
                    self->setupEndpoint(requestingDevice);
                });
        }

        setupEndpoint(device);
    }
    catch (const std::system_error& e)
    {
        if (e.code() != std::errc::device_or_resource_busy)
        {
            throw e;
        }

        auto current = devices.deviceFor(path);
        if (!current)
        {
            warning(
                "Invalid state: Failed to manage device for inventory at '{INVENTORY_PATH}', but the inventory item is unrecognised",
                "INVENTORY_PATH", path);
            return;
        }

        // TODO: Ensure remove completion happens-before add. For now this
        // happens unsynchronised. Make some noise about it.
        warning(
            "Unsynchronised endpoint reinitialsation due to configuration change at '{INVENTORY_PATH}': Removing '{MCTP_DEVICE}'",
            "INVENTORY_PATH", path, "MCTP_DEVICE", current->describe());

        unmanageMCTPDevice(path);

        devices.add(path, device);

        // Pray (this is the unsynchronised bit)
        deferSetup(device);
    }
}

void MCTPReactor::unmanageMCTPDevice(const std::string& path)
{
    auto device = devices.deviceFor(path);
    if (!device)
    {
        info("Unrecognised inventory item: {INVENTORY_PATH}", "INVENTORY_PATH",
             path);
        return;
    }

    info("MCTP device inventory removed at '{INVENTORY_PATH}'",
         "INVENTORY_PATH", path);
    clearUsbSetupFailureTracking(device);

    deferred.erase(device);
    failureCounts.erase(device);

    // Remove the device from the repository before notifying the device itself
    // of removal so we don't defer its setup
    devices.remove(device);

    debug("Stopping management of MCTP device at [ {MCTP_DEVICE} ]",
          "MCTP_DEVICE", device->describe());

    device->remove();
}

std::optional<std::string> MCTPReactor::getDeviceName(uint8_t eid)
{
    return devices.getNameForEid(eid);
}

std::optional<uint8_t> MCTPReactor::getStaticEidFromInterface(
    const std::string& interface)
{
    return devices.getStaticEidFromInterface(interface);
}

bool MCTPReactor::forceUSBRecovery(const std::string& interface,
                                   std::string& status)
{
    info("ForceUSBRecovery requested for interface {USB_INTERFACE}",
         "USB_INTERFACE", interface);
    if (interface.empty())
    {
        status = "Interface is empty";
        warning("ForceUSBRecovery rejected: empty interface");
        return false;
    }

    if (!usbRecovery)
    {
        status = "USB recovery backend unavailable";
        warning(
            "ForceUSBRecovery failed for interface {USB_INTERFACE}: backend unavailable",
            "USB_INTERFACE", interface);
        return false;
    }
    bool success = usbRecovery->clearBulkOutHalt(interface, status);
    if (success)
    {
        info(
            "ForceUSBRecovery succeeded for interface {USB_INTERFACE}: {STATUS}",
            "USB_INTERFACE", interface, "STATUS", status);
    }
    else
    {
        warning(
            "ForceUSBRecovery failed for interface {USB_INTERFACE}: {STATUS}",
            "USB_INTERFACE", interface, "STATUS", status);
    }
    return success;
}

void MCTPReactor::onMctpdEndpointInterfacesAdded(sdbusplus::message_t& msg)
{
    try
    {
        auto [objPath, interfaces] =
            msg.unpack<sdbusplus::message::object_path, SensorData>();
        if (interfaces.find(mctpdEndpointControlIface) == interfaces.end())
        {
            return;
        }
        const auto eidOpt = eidFromMctpdEndpointPath(objPath.str);
        if (!eidOpt)
        {
            return;
        }
        devices.markDiscoveredMctpEndpointEid(*eidOpt);
    }
    catch (const std::exception& e)
    {
        error("Failed to handle mctpd InterfacesAdded: {ERROR}", "ERROR",
              e.what());
    }
}

bool MCTPReactor::setAutoUSBRecoveryEnabled(bool enabled)
{
    autoUSBRecoveryEnabled = enabled;
    info("Auto USB clear-halt recovery runtime switch set to {ENABLED}",
         "ENABLED", autoUSBRecoveryEnabled);
    return autoUSBRecoveryEnabled;
}

bool MCTPReactor::isAutoUSBRecoveryEnabled() const
{
    return autoUSBRecoveryEnabled;
}
