#ifndef DBUS_SENSORS_LPU_VR_SENSORS_HPP
#define DBUS_SENSORS_LPU_VR_SENSORS_HPP

#include "../Utils.hpp"

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <memory>

void lpuVrSensorsApplyManagedObjects(
    boost::asio::io_context& io, sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const ManagedObjectType& managedObjects);

void lpuVrSensorsClearPublishers();

bool lpuVrSensorsPublishersEmpty();

#endif /* DBUS_SENSORS_LPU_VR_SENSORS_HPP */
