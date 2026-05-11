#pragma once

#include "MCTPDeviceRepository.hpp"
#include "MCTPEndpoint.hpp"
#include "USBRecovery.hpp"
#include "Utils.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct AssociationServer
{
    virtual ~AssociationServer() = default;

    virtual void associate(const std::string& path,
                           const std::vector<Association>& associations) = 0;
    virtual void disassociate(const std::string& path) = 0;
};

class MCTPReactor : public std::enable_shared_from_this<MCTPReactor>
{
    using MCTPDeviceFactory = std::function<std::shared_ptr<MCTPDevice>(
        const std::string& interface, const std::vector<std::uint8_t>& physaddr,
        std::optional<std::uint8_t> eid)>;

  public:
    MCTPReactor() = delete;
    MCTPReactor(const MCTPReactor&) = delete;
    MCTPReactor(MCTPReactor&&) = delete;
    explicit MCTPReactor(AssociationServer& server,
                         std::unique_ptr<USBRecovery> usbRecovery =
                             std::make_unique<LibusbUSBRecovery>()) :
        server(server), usbRecovery(std::move(usbRecovery))
    {}
    ~MCTPReactor() = default;
    MCTPReactor& operator=(const MCTPReactor&) = delete;
    MCTPReactor& operator=(MCTPReactor&&) = delete;

    void tick();
    bool isRetrying(uint8_t eid) const;

    void manageMCTPDevice(const std::string& path,
                          const std::shared_ptr<MCTPDevice>& device);
    void unmanageMCTPDevice(const std::string& path);

    std::optional<std::string> getDeviceName(uint8_t eid);
    std::optional<uint8_t> getStaticEidFromInterface(
        const std::string& interface);
    bool forceUSBRecovery(const std::string& interface, std::string& status);
    bool setAutoUSBRecoveryEnabled(bool enabled);
    bool isAutoUSBRecoveryEnabled() const;

    /** mctpd ObjectManager InterfacesAdded on an endpoint object path. */
    void onMctpdEndpointInterfacesAdded(sdbusplus::message_t& msg);

  private:
    static std::optional<std::string> findSMBusInterface(int bus);

    AssociationServer& server;
    MCTPDeviceRepository devices;

    // Tracks MCTP devices that have failed their setup
    std::set<std::shared_ptr<MCTPDevice>> deferred;

    // Map to track failure counts for each device
    std::map<std::shared_ptr<MCTPDevice>, int> failureCounts;
    std::map<std::shared_ptr<MCTPDevice>, int> usbSetupFailureCounts;

    void deferSetup(const std::shared_ptr<MCTPDevice>& dev);
    void trackUsbSetupFailure(const std::shared_ptr<MCTPDevice>& dev);
    void clearUsbSetupFailureTracking(const std::shared_ptr<MCTPDevice>& dev);
    void setupEndpoint(const std::shared_ptr<MCTPDevice>& dev);
    void trackEndpoint(const std::shared_ptr<MCTPEndpoint>& ep);
    void untrackEndpoint(const std::shared_ptr<MCTPEndpoint>& ep);
    bool autoUSBRecoveryEnabled = false;
    std::unique_ptr<USBRecovery> usbRecovery;
};
