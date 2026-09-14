#ifndef DBUS_SENSORS_LPU_ON_BOOT_FIELDS_HPP
#define DBUS_SENSORS_LPU_ON_BOOT_FIELDS_HPP

#include "../Utils.hpp"

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <memory>

/** Rebuild on-boot D-Bus fields from entity-manager GetManagedObjects response. */
void lpuOnBootFieldsApplyManagedObjects(
    boost::asio::io_context& io, sdbusplus::asio::object_server& objectServer,
    const std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const ManagedObjectType& resp);

bool lpuOnBootFieldsPublishersEmpty();

#endif /* DBUS_SENSORS_LPU_ON_BOOT_FIELDS_HPP */
