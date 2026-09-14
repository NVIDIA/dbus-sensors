#ifndef DBUS_SENSORS_LPU_PROCESSOR_METRICS_HPP
#define DBUS_SENSORS_LPU_PROCESSOR_METRICS_HPP

#include "../Utils.hpp"

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace lpu_metrics_detail
{

struct MetricRow
{
    std::string displayName{};
    std::string auxiliaryName{};
    uint8_t busId{};
    uint8_t i2cAddr{};
    uint16_t offset{};
    uint8_t readWidthBytes{};
    /** Seconds between I2C reads; 0 means read once on config refresh only. */
    float pollRateSec{0.0F};
    bool initialized{false};
    uint8_t oneShotRetryCount{0};
    std::chrono::steady_clock::time_point nextPollDue{};
    /** Driver wall-clock timestamp for the source register's last poll. */
    uint64_t lastPolledNs{0};
};

} // namespace lpu_metrics_detail

void lpuProcessorMetricsApplyManagedObjects(
    boost::asio::io_context& io, sdbusplus::asio::object_server& objectServer,
    const std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const ManagedObjectType& resp);

bool lpuProcessorMetricsPublishersEmpty();

#endif /* DBUS_SENSORS_LPU_PROCESSOR_METRICS_HPP */
