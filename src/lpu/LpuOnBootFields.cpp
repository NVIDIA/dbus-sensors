#include "LpuOnBootFields.hpp"

#include "LpuCommon.hpp"
#include "LpuProcessorMetrics.hpp"

#include <boost/asio/post.hpp>

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/message.hpp>

#include <array>
#include <chrono>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lpu_detail
{

constexpr const char* softwareDbusRoot = "/xyz/openbmc_project/software";
constexpr const char* softwareVersionInterface =
    "xyz.openbmc_project.Software.Version";
constexpr const char* softwareAssetInterface =
    "xyz.openbmc_project.Software.Asset";
constexpr const char* softwareSettingsInterface =
    "xyz.openbmc_project.Software.Settings";
constexpr const char* softwareVersionPurposeOther =
    "xyz.openbmc_project.Software.Version.VersionPurpose.Other";
/** bmcweb RelatedItem: Properties.Get on .../inventory for Association.endpoints. */
constexpr const char* associationEndpointsInterface =
    "xyz.openbmc_project.Association";

/** Stable backing for dbus_interface properties (not updated after ctor). */
const std::string kEmptyPropertyValue;
const std::string kVersionPurposeValue(softwareVersionPurposeOther);

constexpr auto failedVersionReadRetryDelay = std::chrono::minutes(5);
std::mutex failedVersionReadMutex;
std::unordered_map<uint16_t, std::chrono::steady_clock::time_point>
    failedVersionReadRetryAfter;

uint16_t versionReadEndpointKey(uint8_t bus, uint8_t addr)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(bus) << 8U) | addr);
}

bool versionReadEndpointRetryDeferred(uint8_t bus, uint8_t addr)
{
    std::lock_guard<std::mutex> guard(failedVersionReadMutex);
    auto failure = failedVersionReadRetryAfter.find(
        versionReadEndpointKey(bus, addr));
    return failure != failedVersionReadRetryAfter.end() &&
           std::chrono::steady_clock::now() < failure->second;
}

bool rememberFailedVersionReadEndpoint(uint8_t bus, uint8_t addr)
{
    std::lock_guard<std::mutex> guard(failedVersionReadMutex);
    auto [failure, inserted] = failedVersionReadRetryAfter.try_emplace(
        versionReadEndpointKey(bus, addr));
    failure->second =
        std::chrono::steady_clock::now() + failedVersionReadRetryDelay;
    return inserted;
}

void forgetFailedVersionReadEndpoint(uint8_t bus, uint8_t addr)
{
    std::lock_guard<std::mutex> guard(failedVersionReadMutex);
    failedVersionReadRetryAfter.erase(versionReadEndpointKey(bus, addr));
}

std::optional<std::string> optionalSoftwareId(
    const lpu::PropertyBaseConfigMap& cfg)
{
    auto it = cfg.find(lpu_em::kSoftwareId);
    if (it == cfg.end())
    {
        return std::nullopt;
    }
    if (const auto* value = std::get_if<std::string>(&it->second);
        value != nullptr)
    {
        if (value->empty())
        {
            return std::nullopt;
        }
        return *value;
    }

    try
    {
        int softwareId = std::visit(VariantToIntVisitor(), it->second);
        if (softwareId < 0)
        {
            lg2::error("SoftwareId must be non-negative");
            return std::nullopt;
        }

        std::ostringstream stream;
        stream << "0x" << std::uppercase << std::hex << std::setfill('0')
               << std::setw(4) << softwareId;
        return stream.str();
    }
    catch (const std::invalid_argument& e)
    {
        lg2::error("Malformed LPU SoftwareId: {ERROR}", "ERROR", e.what());
        return std::nullopt;
    }
}

struct I2cSoftwareVersionPublisher
{
    static std::shared_ptr<I2cSoftwareVersionPublisher> create(
        boost::asio::io_context& ioctx,
        sdbusplus::asio::object_server& objServer, std::string objectPath,
        std::string inventoryActivationEndpoint,
        std::string assetManufacturer, std::string assetModel,
        std::string assetPartNumber, std::string assetSerialNumber,
        std::string assetName, std::string softwareId,
        bool hasSoftwareWriteProtected, bool softwareWriteProtected,
        uint8_t bus, uint8_t address, uint16_t regOffset, uint8_t readWidth,
        std::string logContext)
    {
        auto p = std::make_shared<I2cSoftwareVersionPublisher>(
            ioctx, objServer, std::move(objectPath),
            std::move(inventoryActivationEndpoint), std::move(assetManufacturer),
            std::move(assetModel), std::move(assetPartNumber),
            std::move(assetSerialNumber), std::move(assetName),
            std::move(softwareId), hasSoftwareWriteProtected,
            softwareWriteProtected, bus, address, regOffset, readWidth,
            std::move(logContext));
        boost::asio::post(ioctx,
                          [weak = std::weak_ptr<I2cSoftwareVersionPublisher>(p)]() {
                              if (auto self = weak.lock())
                              {
                                  self->readVersionOnce();
                              }
                          });
        return p;
    }

    I2cSoftwareVersionPublisher(const I2cSoftwareVersionPublisher&) = delete;
    I2cSoftwareVersionPublisher&
        operator=(const I2cSoftwareVersionPublisher&) = delete;

    ~I2cSoftwareVersionPublisher()
    {
        if (inventoryEndpointsInterface)
        {
            objectServer.remove_interface(inventoryEndpointsInterface);
        }
        if (assocDefinitionsInterface)
        {
            objectServer.remove_interface(assocDefinitionsInterface);
        }
        if (assetInterface)
        {
            objectServer.remove_interface(assetInterface);
        }
        if (settingsInterface)
        {
            objectServer.remove_interface(settingsInterface);
        }
        if (versionInterface)
        {
            objectServer.remove_interface(versionInterface);
        }
    }

    I2cSoftwareVersionPublisher(
        boost::asio::io_context& ioctx,
        sdbusplus::asio::object_server& objServer, std::string objectPath,
        std::string inventoryActivationEndpoint,
        std::string assetManufacturer, std::string assetModel,
        std::string assetPartNumber, std::string assetSerialNumber,
        std::string assetName, std::string softwareId,
        bool hasSoftwareWriteProtected, bool softwareWriteProtected,
        uint8_t bus, uint8_t address, uint16_t regOffset, uint8_t readWidth,
        std::string logContext) :
        ioCtx(ioctx),
        objectServer(objServer),
        softwareRootPath(std::move(objectPath)),
        busId(bus), addr(address),
        offset(regOffset), readWidthBytes(readWidth),
        logContext(std::move(logContext)),
        assetManufacturer(std::move(assetManufacturer)),
        assetModel(std::move(assetModel)),
        assetPartNumber(std::move(assetPartNumber)),
        assetSerialNumber(std::move(assetSerialNumber)),
        assetName(std::move(assetName)),
        softwareId(std::move(softwareId)),
        hasSoftwareWriteProtected(hasSoftwareWriteProtected),
        softwareWriteProtected(softwareWriteProtected)
    {
        versionInterface = objectServer.add_interface(softwareRootPath,
                                                      softwareVersionInterface);
        versionInterface->register_property("Version", versionString);
        versionInterface->register_property("Purpose", kVersionPurposeValue);
        versionInterface->register_property("SoftwareId", this->softwareId);
        versionInterface->register_property("PrettyName", this->assetName);
        versionInterface->initialize();

        assetInterface =
            objectServer.add_interface(softwareRootPath, softwareAssetInterface);
        assetInterface->register_property("Manufacturer", this->assetManufacturer);
        assetInterface->register_property("Model", this->assetModel);
        assetInterface->register_property("PartNumber", this->assetPartNumber);
        assetInterface->register_property("SerialNumber", this->assetSerialNumber);
        assetInterface->register_property("Name", this->assetName);
        assetInterface->register_property("BuildDate", kEmptyPropertyValue);
        assetInterface->register_property("SKU", kEmptyPropertyValue);
        assetInterface->register_property("SparePartNumber", kEmptyPropertyValue);
        assetInterface->register_property("SubModel", kEmptyPropertyValue);
        assetInterface->initialize();

        if (this->hasSoftwareWriteProtected)
        {
            settingsInterface = objectServer.add_interface(
                softwareRootPath, softwareSettingsInterface);
            settingsInterface->register_property("WriteProtected",
                                                 this->softwareWriteProtected);
            settingsInterface->initialize();
        }

        assocDefinitionsInterface = objectServer.add_interface(
            softwareRootPath, association::interface);
        std::vector<Association> assocs;
        assocs.emplace_back("inventory", "activation",
                            inventoryActivationEndpoint);
        assocs.emplace_back("software_version", "updateable", softwareDbusRoot);
        assocDefinitionsInterface->register_property("Associations",
                                                     std::move(assocs));
        assocDefinitionsInterface->initialize();

        inventoryEndpointsInterface = objectServer.add_interface(
            softwareRootPath + "/inventory", associationEndpointsInterface);
        inventoryEndpointsInterface->register_property(
            "endpoints",
            std::vector<std::string>{inventoryActivationEndpoint});
        inventoryEndpointsInterface->initialize();
    }

  private:
    void readVersionOnce()
    {
        std::array<uint8_t, 8> buf{};
        if (readWidthBytes == 0 || readWidthBytes > buf.size())
        {
            lg2::warning(
                "Invalid readWidthBytes ({WIDTH}) for {CTX} (buf {SZ}), skipping read",
                "WIDTH", readWidthBytes, "CTX", logContext, "SZ", buf.size());
            return;
        }
        if (lpu_common::isI2cEndpointBlocked(busId, addr) ||
            versionReadEndpointRetryDeferred(busId, addr))
        {
            return;
        }
        if (lpu_common::i2cReadBytes(busId, addr, offset, buf.data(),
                                     readWidthBytes, false) == 0)
        {
            versionString = lpu_common::formatRegisterVersionString(
                buf.data(), readWidthBytes);
            if (versionInterface)
            {
                versionInterface->set_property("Version", versionString);
            }
            forgetFailedVersionReadEndpoint(busId, addr);
        }
        else
        {
            if (rememberFailedVersionReadEndpoint(busId, addr))
            {
                lg2::warning(
                    "Register version read failed ({CTX}), keeping Unknown",
                    "CTX", logContext);
            }
        }
    }

    boost::asio::io_context& ioCtx;
    sdbusplus::asio::object_server& objectServer;
    std::string softwareRootPath;
    uint8_t busId;
    uint8_t addr;
    uint16_t offset;
    uint8_t readWidthBytes;
    std::string logContext;
    std::string assetManufacturer;
    std::string assetModel;
    std::string assetPartNumber;
    std::string assetSerialNumber;
    std::string assetName;
    std::string softwareId;
    bool hasSoftwareWriteProtected{false};
    bool softwareWriteProtected{false};
    std::string versionString{"Unknown"};
    std::shared_ptr<sdbusplus::asio::dbus_interface> versionInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> assetInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> settingsInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> assocDefinitionsInterface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> inventoryEndpointsInterface;
};

std::vector<std::shared_ptr<I2cSoftwareVersionPublisher>> softwarePublishers;

void applyManagedObjects(
    boost::asio::io_context& io, sdbusplus::asio::object_server& objectServer,
    const std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const ManagedObjectType& resp)
{
    // Free previous /software/HGX_FW_* objects before registering replacements
    // (GetManagedObjects refresh reuses the same paths on this connection).
    softwarePublishers.clear();
    std::vector<std::shared_ptr<I2cSoftwareVersionPublisher>> nextPublishers;
    for (const auto& pathPair : resp)
    {
        for (const auto& entry : pathPair.second)
        {
            if (!entry.first.starts_with(configInterfacePrefix))
            {
                continue;
            }
            const lpu::SensorBaseConfigMap& cfg = entry.second;
            if (cfg.find(lpu_em::kType) == cfg.end())
            {
                continue;
            }
            std::string recordType =
                lpu_common::optionalConfigString(cfg, lpu_em::kType).value_or("");
            if (entry.first != configInterfaceName(recordType))
            {
                continue;
            }

            if (recordType != lpu_em::kRecordTypeLpu)
            {
                continue;
            }

            // This field opts a row into publishing a /software object. LP30
            // module firmware owns that inventory, so per-LPU rows omit it.
            // Absence is intentional and must not produce a warning.
            if (cfg.find(lpu_em::kFirmwareSoftwareObjectName) == cfg.end())
            {
                continue;
            }

            std::string displayName =
                lpu_common::optionalConfigString(cfg, lpu_em::kName).value_or("");
            if (displayName.empty())
            {
                lg2::warning(
                    "LPU firmware row has missing or invalid Name at {PATH}; skipping",
                    "PATH", std::string(pathPair.first));
                continue;
            }

            auto firmwareSoftwareBasename =
                lpu_common::optionalConfigString(cfg,
                                                 lpu_em::kFirmwareSoftwareObjectName);
            auto inventoryActivation = lpu_common::optionalConfigString(
                cfg, lpu_em::kFirmwareInventoryAssociation);

            if (!firmwareSoftwareBasename)
            {
                lg2::warning(
                    "LPU row has invalid FirmwareSoftwareObjectName for {NAME}; skipping",
                    "NAME", displayName);
                continue;
            }

            if (!inventoryActivation || inventoryActivation->empty())
            {
                lg2::warning(
                    "FirmwareInventoryAssociation required for firmware software object {NAME}; skipping",
                    "NAME", displayName);
                continue;
            }

            const std::string inventoryActivationPath(*inventoryActivation);

            auto busId = lpu_common::parseBusId(cfg);
            if (!busId)
            {
                continue;
            }
            auto addr = lpu_common::parseI2cAddress(cfg);
            if (!addr)
            {
                continue;
            }
            auto off = lpu_common::parseOffsetValue(cfg);
            if (!off)
            {
                continue;
            }
            auto width = lpu_common::parseReadWidthBytes(cfg);
            if (!width)
            {
                continue;
            }
            if (firmwareSoftwareBasename->find('/') != std::string::npos)
            {
                lg2::error(
                    "FirmwareSoftwareObjectName must be a basename for {NAME} path {PATH}; skipping",
                    "NAME", displayName, "PATH", std::string(pathPair.first));
                continue;
            }
            std::string dbusPath = std::string(softwareDbusRoot) + "/" +
                                   *firmwareSoftwareBasename;
            const std::string dbusPathLogged = dbusPath;
            std::string mfg =
                lpu_common::optionalConfigString(
                    cfg, lpu_em::kSoftwareAssetManufacturer)
                    .value_or("");
            std::string mdl =
                lpu_common::optionalConfigString(cfg, lpu_em::kSoftwareAssetModel)
                    .value_or("");
            std::string pn =
                lpu_common::optionalConfigString(
                    cfg, lpu_em::kSoftwareAssetPartNumber)
                    .value_or("");
            std::string sn =
                lpu_common::optionalConfigString(
                    cfg, lpu_em::kSoftwareAssetSerialNumber)
                    .value_or("");
            std::string nm =
                lpu_common::optionalConfigString(cfg, lpu_em::kSoftwareAssetName)
                    .value_or("");
            std::string swId = optionalSoftwareId(cfg).value_or("");
            bool hasSoftwareWriteProtected = false;
            bool softwareWriteProtected = false;
            if (auto it = cfg.find(lpu_em::kSoftwareWriteProtected);
                it != cfg.end())
            {
                if (const bool* value = std::get_if<bool>(&it->second))
                {
                    hasSoftwareWriteProtected = true;
                    softwareWriteProtected = *value;
                }
            }
            try
            {
                nextPublishers.push_back(I2cSoftwareVersionPublisher::create(
                    io, objectServer, std::move(dbusPath), inventoryActivationPath,
                    std::move(mfg), std::move(mdl), std::move(pn), std::move(sn),
                    std::move(nm), std::move(swId), hasSoftwareWriteProtected,
                    softwareWriteProtected, *busId, *addr, *off, *width,
                    displayName));
            }
            catch (const std::exception& e)
            {
                lg2::error(
                    "LPU firmware software D-Bus publish failed for {NAME} path {PATH}: {ERR} "
                    "(often FileExists if another service owns the path)",
                    "NAME", displayName, "PATH", dbusPathLogged, "ERR",
                    e.what());
            }
        }
    }
    // Old publishers were cleared at start; swap in the freshly built list.
    // Posted readVersionOnce handlers use weak_ptr so they no-op if dropped.
    nextPublishers.swap(softwarePublishers);
    lpuProcessorMetricsApplyManagedObjects(io, objectServer, dbusConnection,
                                           resp);
}

} // namespace lpu_detail

void lpuOnBootFieldsApplyManagedObjects(
    boost::asio::io_context& io, sdbusplus::asio::object_server& objectServer,
    const std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const ManagedObjectType& resp)
{
    lpu_detail::applyManagedObjects(io, objectServer, dbusConnection, resp);
}

bool lpuOnBootFieldsPublishersEmpty()
{
    return lpu_detail::softwarePublishers.empty() &&
           lpuProcessorMetricsPublishersEmpty();
}
