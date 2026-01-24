#include "GpioInputConfig.hpp"

#include <phosphor-logging/lg2.hpp>

#include <exception>

PHOSPHOR_LOG2_USING;

namespace nvidia::write_protect
{

auto GpioInputConfig::tryFrom(sdbusplus::async::context& ctx,
                              std::string_view service, std::string_view path,
                              std::string_view interface)
    -> sdbusplus::async::task<std::optional<GpioInputConfig>>
{
    try
    {
        GpioInputConfig config{};

        info("Reading config service={SVC} path={PATH} intf={INTF}", "SVC",
             service, "PATH", path, "INTF", interface);

        auto props = co_await sdbusplus::async::proxy()
                         .service(service)
                         .interface(interface)
                         .path(path)
                         .get_all_properties<PropertiesVariant>(ctx);

        debug("Got {COUNT} properties from {PATH}", "COUNT", props.size(),
              "PATH", path);

        if (auto it = props.find("Name"); it != props.end())
        {
            config.name = std::get<std::string>(it->second);
        }
        if (auto it = props.find("LineName"); it != props.end())
        {
            config.lineName = std::get<std::string>(it->second);
        }
        if (auto it = props.find("ActiveLow"); it != props.end())
        {
            config.activeLow = std::get<bool>(it->second);
        }
        if (auto it = props.find("FlashProtectedComponents"); it != props.end())
        {
            config.flashProtectedComponents =
                std::get<std::vector<std::string>>(it->second);
        }
        if (auto it = props.find("PollInterval"); it != props.end())
        {
            config.pollInterval =
                std::chrono::milliseconds(std::get<uint64_t>(it->second));
        }

        info(
            "Parsed config name={NAME} line={LINE} activeLow={ACTIVE_LOW} flashProtectedComponents={NUM_PROT_COMPONENTS}",
            "NAME", config.name, "LINE", config.lineName, "ACTIVE_LOW",
            config.activeLow, "NUM_PROT_COMPONENTS",
            config.flashProtectedComponents.size());

        co_return config;
    }
    catch (const std::exception& e)
    {
        error("Failed to parse group config at {PATH}: {ERROR}", "PATH", path,
              "ERROR", e.what());
        co_return std::nullopt;
    }
}

} // namespace nvidia::write_protect
