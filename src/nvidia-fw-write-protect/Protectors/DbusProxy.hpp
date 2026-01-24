#pragma once

#include "Protector.hpp"

#include <sdbusplus/async.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace nvidia
{
namespace write_protect
{

/**
 * @brief Protector implementation that delegates to a remote D-Bus object.
 *
 * Detects the interface supported by the target object (SoftwareSettings,
 * AsyncSoftwareSettings, or NvidiaWriteProtect) and proxies get/set/changed
 * operations through it.
 */
class DbusProxy : public Protector
{
    enum class Kind
    {
        SoftwareSettings,
        AsyncSoftwareSettings,
        NvidiaWriteProtect,
    };

    sdbusplus::async::context* ctx;
    std::string service;
    Kind kind;
    sdbusplus::message::object_path path;
    std::unique_ptr<sdbusplus::async::match> match{nullptr};

    explicit DbusProxy(sdbusplus::async::context& ctx,
                       const std::string& service, Kind kind,
                       const sdbusplus::message::object_path& path) :
        ctx(&ctx), service(service), kind(kind), path(path), match()
    {}

    auto init() -> sdbusplus::async::task<>;

    static auto detect(sdbusplus::async::context& ctx,
                       const sdbusplus::message::object_path& path)
        -> sdbusplus::async::task<std::optional<std::pair<Kind, std::string>>>;

  public:
    DbusProxy(const DbusProxy&) = delete;
    DbusProxy& operator=(const DbusProxy&) = delete;
    DbusProxy(DbusProxy&&) = default;
    DbusProxy& operator=(DbusProxy&&) = default;

    /**
     * @brief Asynchronously create a DbusProxy for a D-Bus object path.
     *
     * Detects which write-protection interface the target implements. If the
     * target is not yet available on D-Bus, waits for it to appear.
     *
     * @param ctx  The sdbusplus async context.
     * @param path D-Bus object path of the target write-protection object.
     * @return The constructed DbusProxy, or std::nullopt on failure.
     */
    static auto create(sdbusplus::async::context& ctx,
                       const sdbusplus::message::object_path& path)
        -> sdbusplus::async::task<std::optional<DbusProxy>>;

    /** @copydoc Protector::set */
    auto set(bool value) -> sdbusplus::async::task<Result<void>> override;
    /** @copydoc Protector::get */
    auto get() -> sdbusplus::async::task<Result<bool>> override;
    /** @copydoc Protector::changed */
    auto changed() -> sdbusplus::async::task<Result<bool>> override;
};

} // namespace write_protect
} // namespace nvidia
