#pragma once

#include "MCTPEndpoint.hpp"
#include "Utils.hpp"

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <cstdint>
#include <memory>
#include <set>
#include <string>

/**
 * @brief USB Gadget MCTP Device - creates and manages a USB gadget device
 *        for MCTP communication.
 *
 * This device is self-contained - it acts as both device and endpoint.
 * The gadget creates a local USB function interface (not discovering remote
 * endpoints). Multiple instances can be created for multiple gadgets.
 *
 * The setup() method configures the USB gadget using configfs and sets the
 * local EID via mctp command.
 */
class USBGadgetMCTPDevice :
    public MCTPDevice,
    public MCTPEndpoint,
    public std::enable_shared_from_this<USBGadgetMCTPDevice>
{
  public:
    USBGadgetMCTPDevice() = delete;
    static bool match(const std::set<std::string>& interfaces);
    static std::optional<SensorBaseConfigMap> match(const SensorData& config);
    static std::shared_ptr<USBGadgetMCTPDevice> from(
        const std::shared_ptr<sdbusplus::asio::connection>& connection,
        const SensorBaseConfigMap& iface);
    USBGadgetMCTPDevice(
        const std::shared_ptr<sdbusplus::asio::connection>& connection,
        const std::string& gadgetName, uint8_t localEID,
        const std::string& name = "");
    ~USBGadgetMCTPDevice() override = default;

    // MCTPDevice interface
    void setup(std::function<void(const std::error_code& ec,
                                  const std::shared_ptr<MCTPEndpoint>& ep)>&&
                   added) override;
    void remove() override;
    std::string describe() const override;
    std::optional<std::string> getNameForEid(uint8_t eid) const override;

    // MCTPEndpoint interface
    int network() const override;
    uint8_t eid() const override;
    void subscribe(Event&& degraded, Event&& available,
                   Event&& removed) override;

  private:
    static constexpr const char* configType = "MCTPUSBGadgetTarget";
    std::shared_ptr<sdbusplus::asio::connection> connection;
    std::string gadgetName;
    uint8_t localEID;
    std::string name;
    bool isSetup = false;
    std::set<std::string> netLocalEIDs;
    MCTPEndpoint::Event notifyRemoved;
    std::unique_ptr<sdbusplus::bus::match_t> endpointAddedMatch;
    std::unique_ptr<sdbusplus::bus::match_t> endpointRemovedMatch;

    void sendDiscoveryNotify();
    void onEndpointAdded(sdbusplus::message_t& msg);
    void onEndpointRemoved(sdbusplus::message_t& msg);
    bool setRoleEndpoint();

    // MCTPEndpoint::device() implementation
    std::shared_ptr<MCTPDevice> device() const override
    {
        return std::const_pointer_cast<USBGadgetMCTPDevice>(shared_from_this());
    }
};
