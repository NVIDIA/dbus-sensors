#include "MCTPReactor.hpp"

#include "MCTPDeviceRepository.hpp"
#include "MCTPEndpoint.hpp"
#include "Utils.hpp"

#include <boost/system/detail/error_code.hpp>
#include <phosphor-logging/lg2.hpp>
#include <phosphor-logging/lg2/flags.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <charconv>
#include <cstdint>
#include <exception>
#include <functional>
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

void MCTPReactor::trackUsbSetupFailure(const std::shared_ptr<MCTPDevice>& dev)
{
    auto usbDevice = std::dynamic_pointer_cast<USBMCTPDDevice>(dev);
    if (!usbDevice)
    {
        return;
    }

    if (usbDevice->getInterface().empty())
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

void MCTPReactor::trackEndpoint(const std::shared_ptr<MCTPEndpoint>& ep)
{
    info("Added MCTP endpoint to device: [ {MCTP_ENDPOINT} ]", "MCTP_ENDPOINT",
         ep->describe());

    const auto endpointPath = MCTPDEndpoint::path(ep);
    const auto generation = std::make_shared<EndpointGeneration>();
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
        [weak{weak_from_this()}, endpointPath,
         generation](const std::shared_ptr<MCTPEndpoint>& ep) {
            info("Removed MCTP endpoint from device: [ {MCTP_ENDPOINT} ]",
                 "MCTP_ENDPOINT", ep->describe());
            if (auto self = weak.lock())
            {
                auto current =
                    self->trackedEndpointGenerations.find(endpointPath);
                if (current == self->trackedEndpointGenerations.end() ||
                    current->second != generation)
                {
                    info(
                        "Ignoring stale endpoint removal for superseded endpoint: [ {MCTP_ENDPOINT} ]",
                        "MCTP_ENDPOINT", ep->describe());
                    return;
                }

                self->trackedEndpointGenerations.erase(current);
                self->server.disassociate(endpointPath);
                auto dev = ep->device();
                if (!dev || !self->devices.contains(dev))
                {
                    return;
                }

                switch (self->states[dev->id()])
                {
                    case MCTPDeviceState::Unmanaged:
                    case MCTPDeviceState::Assigning:
                    case MCTPDeviceState::Unassigned:
                        break;
                    case MCTPDeviceState::Assigned:
                        self->next(dev, MCTPDeviceState::Lost);
                        break;
                    case MCTPDeviceState::Quarantine:
                        if (self->devices.contains(dev))
                        {
                            self->terminate(dev);
                        }
                        break;
                    case MCTPDeviceState::Lost:
                    case MCTPDeviceState::Recovering:
                        break;
                    case MCTPDeviceState::Recovered:
                        self->next(dev, MCTPDeviceState::Lost);
                        break;
                    case MCTPDeviceState::Removing:
                        // If the configuration has been replaced then we've
                        // already terminated the state tracking
                        if (self->devices.contains(dev))
                        {
                            self->terminate(dev);
                        }
                        break;
                    case MCTPDeviceState::Pending:
                        self->next(dev, MCTPDeviceState::Unassigned);
                        break;
                }
            }
            else
            {
                info(
                    "The reactor object was destroyed concurrent to the removal of the remove match for the endpoint '{MCTP_ENDPOINT}'",
                    "MCTP_ENDPOINT", ep->describe());
            }
        });

    switch (states[ep->device()->id()])
    {
        case MCTPDeviceState::Unmanaged:
            return;
        case MCTPDeviceState::Assigning:
            next(ep->device(), MCTPDeviceState::Assigned);
            break;
        case MCTPDeviceState::Unassigned:
        case MCTPDeviceState::Assigned:
        case MCTPDeviceState::Quarantine:
            next(ep->device(), MCTPDeviceState::Recovered);
            break;
        case MCTPDeviceState::Lost:
            return;
        case MCTPDeviceState::Recovering:
            next(ep->device(), MCTPDeviceState::Recovered);
            break;
        case MCTPDeviceState::Recovered:
        case MCTPDeviceState::Removing:
        case MCTPDeviceState::Pending:
            return;
    }

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
    server.associate(endpointPath, associations);
    trackedEndpointGenerations[endpointPath] = generation;
}

void MCTPReactor::setupEndpoint(const std::shared_ptr<MCTPDevice>& dev)
{
    debug(
        "Attempting to setup up MCTP endpoint for device at [ {MCTP_DEVICE} ]",
        "MCTP_DEVICE", dev->describe());
    dev->setup([weak{weak_from_this()}, wdev = std::weak_ptr<MCTPDevice>(dev)](
                   const std::error_code& ec,
                   const std::shared_ptr<MCTPEndpoint>& ep) mutable {
        auto self = weak.lock();
        if (!self)
        {
            info(
                "The reactor object was destroyed concurrent to the completion of the endpoint setup");
            return;
        }

        auto dev = wdev.lock();
        if (!dev)
        {
            info(
                "The device was destroyed concurrent to the completion of endpoint setup");
            return;
        }

        if (ec)
        {
            debug(
                "Setup failed for MCTP device at [ {MCTP_DEVICE} ], deferring: {ERROR_MESSAGE}",
                "MCTP_DEVICE", dev->describe(), "ERROR_MESSAGE", ec.message());

            switch (self->states[dev->id()])
            {
                case MCTPDeviceState::Unmanaged:
                    break;
                case MCTPDeviceState::Assigning:
                    self->next(dev, MCTPDeviceState::Unassigned);
                    break;
                case MCTPDeviceState::Unassigned:
                case MCTPDeviceState::Assigned:
                    break;
                case MCTPDeviceState::Quarantine:
                    self->terminate(dev);
                    break;
                case MCTPDeviceState::Lost:
                    break;
                case MCTPDeviceState::Recovering:
                    self->next(dev, MCTPDeviceState::Lost);
                    break;
                case MCTPDeviceState::Recovered:
                case MCTPDeviceState::Removing:
                case MCTPDeviceState::Pending:
                    break;
            }
            // Track failed setup (isRetrying / log suppression; fork addition)
            self->failureCounts[dev]++;
            self->trackUsbSetupFailure(dev);
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
            self->next(dev, MCTPDeviceState::Quarantine);
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
    for (const auto& entry : devices)
    {
        switch (states[entry.second->id()])
        {
            case MCTPDeviceState::Unmanaged:
            case MCTPDeviceState::Assigning:
                break;
            case MCTPDeviceState::Unassigned:
                next(entry.second, MCTPDeviceState::Assigning);
                setupEndpoint(entry.second);
                break;
            case MCTPDeviceState::Assigned:
            case MCTPDeviceState::Quarantine:
                break;
            case MCTPDeviceState::Lost:
                next(entry.second, MCTPDeviceState::Recovering);
                setupEndpoint(entry.second);
                break;
            case MCTPDeviceState::Recovering:
            case MCTPDeviceState::Recovered:
            case MCTPDeviceState::Removing:
            case MCTPDeviceState::Pending:
                break;
        }
    }
}

void MCTPReactor::addDevice(const std::string& path,
                            const std::shared_ptr<MCTPDevice>& device)
{
    devices.add(path, device);
    info("MCTP device inventory added at '{INVENTORY_PATH}'", "INVENTORY_PATH",
         path);
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
}

void MCTPReactor::manageMCTPDevice(const std::string& path,
                                   const std::shared_ptr<MCTPDevice>& device)
{
    if (!device)
    {
        return;
    }

    debug("MCTP device inventory added at '{INVENTORY_PATH}'", "INVENTORY_PATH",
          path);

    if (!states.contains(device->id()))
    {
        debug(
            "Initialising state for device {DEVICE_ID} ([ {DEVICE_DESCRIPTION} ])) as {INITIAL_STATE}",
            "DEVICE_ID", lg2::hex, device->id(), "DEVICE_DESCRIPTION",
            device->describe(), "INITIAL_STATE", MCTPDeviceState::Unmanaged);
        states[device->id()] = MCTPDeviceState::Unmanaged;
    }

    switch (states[device->id()])
    {
        case MCTPDeviceState::Unmanaged:
            addDevice(path, device);
            next(device, MCTPDeviceState::Assigning);
            setupEndpoint(device);
            break;
        case MCTPDeviceState::Assigning:
        case MCTPDeviceState::Unassigned:
            break;
        case MCTPDeviceState::Assigned:
        {
            // EM may publish property changes without removal. Replace the
            // device so its state reflects EM's configuration.
            auto current = devices.deviceFor(path);
            if (!current)
            {
                warning(
                    "Invalid state: Failed to manage device for inventory at '{INVENTORY_PATH}', but the inventory item is unrecognised",
                    "INVENTORY_PATH", path);
                return;
            }

            warning(
                "Endpoint reinitialisation due to configuration change at '{INVENTORY_PATH}': Removing '{MCTP_DEVICE}'",
                "INVENTORY_PATH", path, "MCTP_DEVICE", current->describe());

            auto removed = std::make_shared<bool>(false);
            terminate(current);
            current->remove([weak{weak_from_this()}, path, device, removed]() {
                *removed = true;
                auto self = weak.lock();
                if (!self || self->devices.deviceFor(path) != device)
                {
                    return;
                }

                auto state = self->states.find(device->id());
                if (state != self->states.end() &&
                    state->second == MCTPDeviceState::Pending)
                {
                    self->next(device, MCTPDeviceState::Unassigned);
                }
            });

            addDevice(path, device);
            next(device, MCTPDeviceState::Pending);
            if (*removed)
            {
                next(device, MCTPDeviceState::Unassigned);
            }
            break;
        }
        case MCTPDeviceState::Quarantine:
            next(device, MCTPDeviceState::Assigning);
            break;
        case MCTPDeviceState::Lost:
        case MCTPDeviceState::Recovering:
            break;
        case MCTPDeviceState::Recovered:
            next(device, MCTPDeviceState::Assigned);
            break;
        case MCTPDeviceState::Removing:
            addDevice(path, device);
            next(device, MCTPDeviceState::Pending);
            break;
        case MCTPDeviceState::Pending:
            break;
    }
}

void MCTPReactor::unmanageMCTPDevice(const std::string& path)
{
    unmanageMCTPDevice(path, {});
}

void MCTPReactor::unmanageMCTPDevice(const std::string& path,
                                     std::function<void()>&& removed)
{
    auto device = devices.deviceFor(path);
    if (!device)
    {
        info("Unrecognised inventory item: {INVENTORY_PATH}", "INVENTORY_PATH",
             path);
        if (removed)
        {
            removed();
        }
        return;
    }

    info("MCTP device inventory removed at '{INVENTORY_PATH}'",
         "INVENTORY_PATH", path);
    clearUsbSetupFailureTracking(device);

    switch (states[device->id()])
    {
        case MCTPDeviceState::Unmanaged:
            break;
        case MCTPDeviceState::Assigning:
            next(device, MCTPDeviceState::Quarantine);
            break;
        case MCTPDeviceState::Unassigned:
            terminate(device);
            break;
        case MCTPDeviceState::Assigned:
            failureCounts.erase(device);
            // Remove the device from the repository before notifying the device
            // itself of removal so we don't defer its setup
            devices.remove(device);
            info("Stopping management of MCTP device at [ {MCTP_DEVICE} ]",
                 "MCTP_DEVICE", device->describe());
            next(device, MCTPDeviceState::Removing);
            device->remove(std::move(removed));
            break;
        case MCTPDeviceState::Quarantine:
            break;
        case MCTPDeviceState::Lost:
            terminate(device);
            break;
        case MCTPDeviceState::Recovering:
            next(device, MCTPDeviceState::Quarantine);
            break;
        case MCTPDeviceState::Recovered:
        case MCTPDeviceState::Removing:
            break;
        case MCTPDeviceState::Pending:
            failureCounts.erase(device);
            devices.remove(device);
            debug("Stopping management of MCTP device at [ {MCTP_DEVICE} ]",
                  "MCTP_DEVICE", device->describe());
            next(device, MCTPDeviceState::Removing);
            device->remove(std::move(removed));
            break;
    }
}

void MCTPReactor::next(const std::shared_ptr<MCTPDevice>& dev,
                       const MCTPDeviceState next)
{
    debug(
        "Device {DEVICE_ID} ([ {DEVICE_DESCRIPTION} ]) transitioning from {CURRENT_STATE} to {NEXT_STATE}",
        "DEVICE_ID", lg2::hex, dev->id(), "DEVICE_DESCRIPTION", dev->describe(),
        "CURRENT_STATE", states[dev->id()], "NEXT_STATE", next);
    states[dev->id()] = next;
}

void MCTPReactor::terminate(const std::shared_ptr<MCTPDevice>& dev)
{
    debug(
        "Device {DEVICE_ID} ([ {DEVICE_DESCRIPTION} ]) terminated from {CURRENT_STATE}",
        "DEVICE_ID", lg2::hex, dev->id(), "DEVICE_DESCRIPTION", dev->describe(),
        "CURRENT_STATE", states[dev->id()]);
    devices.remove(dev);
    failureCounts.erase(dev);
    states.erase(dev->id());
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
        auto [objPath,
              interfaces] = msg.unpack<sdbusplus::object_path, SensorData>();
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
