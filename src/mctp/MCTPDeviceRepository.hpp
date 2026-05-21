#pragma once

#include "MCTPEndpoint.hpp"

#include <cstddef>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

class MCTPDeviceRepository
{
  private:
    struct DevicePtrLess
    {
        bool operator()(const std::shared_ptr<MCTPDevice>& lhs,
                        const std::shared_ptr<MCTPDevice>& rhs) const
        {
            return std::less<const MCTPDevice*>{}(lhs.get(), rhs.get());
        }
    };

    std::map<std::string, std::shared_ptr<MCTPDevice>> devices;
    std::map<std::shared_ptr<MCTPDevice>, std::string, DevicePtrLess>
        inventoryByDevice;
    std::map<std::shared_ptr<MCTPDevice>, std::size_t, DevicePtrLess>
        inventoryCountByDevice;

    auto lookup(const std::shared_ptr<MCTPDevice>& device)
    {
        return inventoryByDevice.find(device);
    }

    void addReverseLookup(const std::string& inventory,
                          const std::shared_ptr<MCTPDevice>& device)
    {
        auto [entry, fresh] = inventoryByDevice.emplace(device, inventory);
        if (!fresh && inventory < entry->second)
        {
            entry->second = inventory;
        }
        ++inventoryCountByDevice[device];
    }

    void refreshReverseLookup(const std::shared_ptr<MCTPDevice>& device)
    {
        for (const auto& [inventory, candidate] : devices)
        {
            if (candidate == device)
            {
                inventoryByDevice[device] = inventory;
                return;
            }
        }
        inventoryByDevice.erase(device);
        inventoryCountByDevice.erase(device);
    }

    void removeReverseLookup(const std::shared_ptr<MCTPDevice>& device)
    {
        auto count = inventoryCountByDevice.find(device);
        if (count == inventoryCountByDevice.end() || count->second <= 1)
        {
            inventoryCountByDevice.erase(device);
            inventoryByDevice.erase(device);
            return;
        }

        --count->second;
        refreshReverseLookup(device);
    }

  public:
    MCTPDeviceRepository() = default;
    MCTPDeviceRepository(const MCTPDeviceRepository&) = delete;
    MCTPDeviceRepository(MCTPDeviceRepository&&) = delete;
    ~MCTPDeviceRepository() = default;

    MCTPDeviceRepository& operator=(const MCTPDeviceRepository&) = delete;
    MCTPDeviceRepository& operator=(MCTPDeviceRepository&&) = delete;

    void add(const std::string& inventory,
             const std::shared_ptr<MCTPDevice>& device)
    {
        auto [entry, fresh] = devices.emplace(inventory, device);
        if (!fresh && entry->second.get() != device.get())
        {
            throw std::system_error(
                std::make_error_code(std::errc::device_or_resource_busy),
                std::format("Tried to add entry for existing device: {}",
                            device->describe()));
        }
        if (fresh)
        {
            addReverseLookup(inventory, device);
        }
    }

    void remove(const std::shared_ptr<MCTPDevice>& device)
    {
        auto entry = lookup(device);
        if (entry == inventoryByDevice.end())
        {
            throw std::system_error(
                std::make_error_code(std::errc::no_such_device),
                std::format("Trying to remove unknown device: {}",
                            device->describe()));
        }
        const auto inventory = entry->second;
        devices.erase(inventory);
        removeReverseLookup(device);
    }

    bool contains(const std::shared_ptr<MCTPDevice>& device)
    {
        return lookup(device) != inventoryByDevice.end();
    }

    std::optional<std::string> inventoryFor(
        const std::shared_ptr<MCTPDevice>& device)
    {
        auto entry = lookup(device);
        if (entry == inventoryByDevice.end())
        {
            return {};
        }
        return entry->second;
    }

    std::shared_ptr<MCTPDevice> deviceFor(const std::string& inventory)
    {
        auto entry = devices.find(inventory);
        if (entry == devices.end())
        {
            return {};
        }
        return entry->second;
    }

    std::optional<std::string> getNameForEid(uint8_t eid)
    {
        for (const auto& [path, device] : devices)
        {
            auto name = device->getNameForEid(eid);
            if (name)
            {
                return name;
            }
        }
        return std::nullopt;
    }

    std::optional<uint8_t> getStaticEidFromInterface(
        const std::string& interface)
    {
        for (const auto& [path, device] : devices)
        {
            auto mctpDevice = std::dynamic_pointer_cast<MCTPDDevice>(device);
            if (mctpDevice && mctpDevice->getInterface() == interface)
            {
                return mctpDevice->getEid();
            }
        }
        return std::nullopt;
    }

    /** Notify devices that may manage @p eid that mctpd published that
     * endpoint. */
    void markDiscoveredMctpEndpointEid(uint8_t eid)
    {
        for (const auto& [path, device] : devices)
        {
            if (auto mctp = std::dynamic_pointer_cast<MCTPDDevice>(device))
            {
                mctp->markDiscoveredMctpEid(eid);
            }
        }
    }

    auto begin()
    {
        return devices.begin();
    }

    auto end()
    {
        return devices.end();
    }
};
