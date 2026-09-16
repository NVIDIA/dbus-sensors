#ifndef DBUS_SENSORS_LPU_COMMON_HPP
#define DBUS_SENSORS_LPU_COMMON_HPP

#include "../Utils.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

/** Type aliases for LPU rows in entity-manager configuration maps. */
namespace lpu
{

/** Property map for one entity-manager configuration interface (LPU subtree
 * only). */
using PropertyBaseConfigMap = ::SensorBaseConfigMap;

/** Same underlying type as elsewhere in dbus-sensors; LPU-local name for
 * interface rows. */
using SensorBaseConfigMap = ::SensorBaseConfigMap;

} // namespace lpu

/** PMBus layout exposed by LPU SMBus function 0x67. */
namespace lpu_pmbus
{

inline constexpr uint16_t vddCoreBaseOffset = 0x0100U;
inline constexpr uint16_t vddC2cBaseOffset = 0x0120U;
inline constexpr uint16_t vddmBaseOffset = 0x0140U;
inline constexpr uint16_t vddhC2cBaseOffset = 0x0160U;
inline constexpr uint16_t vddlC2cBaseOffset = 0x0180U;

inline constexpr uint16_t voutSubOffset = 0x00U;
inline constexpr uint16_t ioutSubOffset = 0x01U;
inline constexpr uint16_t vinSubOffset = 0x0AU;
inline constexpr uint16_t temperatureSubOffset = 0x0BU;
inline constexpr uint16_t poutSubOffset = 0x0CU;

// Each rail reserves PMBus sub-offsets +0x00 through +0x0F.
inline constexpr uint16_t lastRegisterSubOffset = 0x0FU;

} // namespace lpu_pmbus

/** I2C access and EM field parsing helpers for LPU telemetry. */
namespace lpu_common
{
void createInventoryAssoc(
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::shared_ptr<sdbusplus::asio::dbus_interface>& association,
    const std::string& path, AssociationList&& additionalAssociations);

bool parseLpuInstanceToInventoryPath(const lpu::PropertyBaseConfigMap& cfg,
                                     std::string& outPath,
                                     const std::string& displayName);
std::optional<uint64_t> parseLpuInstance(const lpu::PropertyBaseConfigMap& cfg,
                                         const std::string& displayName);

std::optional<uint8_t> parseI2cAddress(const lpu::PropertyBaseConfigMap& cfg);
std::optional<uint8_t> parseReadWidthBytes(
    const lpu::PropertyBaseConfigMap& cfg);
std::optional<uint16_t> parseOffsetValue(const lpu::PropertyBaseConfigMap& cfg);
std::optional<uint8_t> parseBusId(const lpu::PropertyBaseConfigMap& cfg);
std::optional<std::string> optionalConfigString(
    const lpu::PropertyBaseConfigMap& cfg, const char* key);

int i2cReadBytes(
    uint64_t bus, uint8_t addr, uint16_t offset, uint8_t* out, uint8_t len,
    bool rejectIfResponseAllOnes = true,
    std::chrono::nanoseconds maxSampleAge = std::chrono::nanoseconds::zero(),
    uint64_t* lastPolledNs = nullptr);

bool isI2cEndpointBlocked(uint8_t bus, uint8_t addr);

std::string formatRegisterVersionString(const uint8_t* buf, uint8_t len);
/* ASCII serial from I2C (NUL-terminated or fixed width); trim spaces; empty ->
 * "NA". */
std::string formatLpuAssetSerialString(const uint8_t* buf, uint8_t len);

} // namespace lpu_common

/** String keys in flattened entity-manager JSON for LPU firmware configuration.
 */
namespace lpu_em
{

inline constexpr const char* kRecordTypeLpu = "LPU";
inline constexpr const char* kRecordTypeLpuMetrics = "LPU_Metrics";
inline constexpr const char* kRecordTypeLpuVrMetrics = "LPU_VR_Metrics";
inline constexpr const char* kRecordTypeLpuPowerProfile = "LPU_Power_Profile";

inline constexpr const char* kType = "Type";
inline constexpr const char* kName = "Name";

inline constexpr const char* kFirmwareSoftwareObjectName =
    "FirmwareSoftwareObjectName";
inline constexpr const char* kFirmwareInventoryAssociation =
    "FirmwareInventoryAssociation";
inline constexpr const char* kSoftwareId = "SoftwareId";
inline constexpr const char* kSoftwareWriteProtected = "SoftwareWriteProtected";

inline constexpr const char* kSoftwareAssetManufacturer =
    "SoftwareAssetManufacturer";
inline constexpr const char* kSoftwareAssetModel = "SoftwareAssetModel";
inline constexpr const char* kSoftwareAssetPartNumber =
    "SoftwareAssetPartNumber";
inline constexpr const char* kSoftwareAssetSerialNumber =
    "SoftwareAssetSerialNumber";
inline constexpr const char* kSoftwareAssetName = "SoftwareAssetName";
inline constexpr const char* kAuxiliaryName = "AuxiliaryName";
inline constexpr const char* kLPUInstance = "LPUInstance";

} // namespace lpu_em

#endif /* DBUS_SENSORS_LPU_COMMON_HPP */
