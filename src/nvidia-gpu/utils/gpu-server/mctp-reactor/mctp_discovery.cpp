// NOLINTBEGIN
#include "mctp_discovery.hpp"

#include <phosphor-logging/lg2.hpp>

namespace gpuserver
{

MctpDiscovery::MctpDiscovery(sdbusplus::bus::bus& bus,
                             const std::string& socketPath) :
    bus(bus)
{
    lg2::info("Initializing MCTP Discovery service");

    // Connect to gpuserver
    ctx = gpuserver_connect(socketPath.c_str());
    if (!ctx)
    {
        lg2::error("Failed to connect to gpuserver at {PATH}", "PATH",
                   socketPath);
        throw std::runtime_error("Failed to connect to gpuserver");
    }
    lg2::info("Connected to gpuserver successfully");

    // Watch for new MCTP endpoints
    endpointAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
        bus, sdbusplus::bus::match::rules::interfacesAdded(mctpBasePath),
        std::bind(&MctpDiscovery::handleEndpointAdded, this,
                  std::placeholders::_1));

    // Scan for existing endpoints
    scanExistingEndpoints();
}

MctpDiscovery::~MctpDiscovery()
{
    if (ctx)
    {
        gpuserver_close(ctx);
    }
}

void MctpDiscovery::handleEndpointAdded(sdbusplus::message::message& msg)
{
    lg2::info("New MCTP endpoint detected");
    sdbusplus::message::object_path objPath;
    std::map<std::string, std::map<std::string, PropertyValue>> interfaces;

    msg.read(objPath, interfaces);
    lg2::debug("Processing endpoint at {PATH}", "PATH", objPath.str);

    // Set up enable property monitoring
    if (enableMatches.find(objPath) == enableMatches.end())
    {
        lg2::debug("Setting up enable property monitoring for {PATH}", "PATH",
                   objPath.str);
        enableMatches.emplace(
            objPath.str, std::make_unique<sdbusplus::bus::match_t>(
                             bus,
                             sdbusplus::bus::match::rules::propertiesChanged(
                                 objPath.str, enableIntf),
                             std::bind(&MctpDiscovery::handlePropertiesChanged,
                                       this, std::placeholders::_1)));
    }

    processEndpoint(objPath.str, msg.get_sender());
}

void MctpDiscovery::handlePropertiesChanged(sdbusplus::message::message& msg)
{
    std::string interface;
    std::map<std::string, std::variant<bool>> changed;
    std::vector<std::string> invalidated;
    msg.read(interface, changed, invalidated);

    auto enabledProp = changed.find("Enabled");
    if (enabledProp != changed.end())
    {
        bool enabled = std::get<bool>(enabledProp->second);
        processEndpoint(msg.get_path(), msg.get_sender(), enabled);
    }
}

void MctpDiscovery::processEndpoint(const std::string& path,
                                    const std::string& service, bool enabled)
{
    lg2::debug(
        "Processing endpoint {PATH} from service {SERVICE}, enabled={ENABLED}",
        "PATH", path, "SERVICE", service, "ENABLED", enabled);
    try
    {
        auto method = bus.new_method_call(service.c_str(), path.c_str(),
                                          "org.freedesktop.DBus.Properties",
                                          "GetAll");
        method.append(mctpEndpointIntf);
        auto reply = bus.call(method);

        Properties properties;
        reply.read(properties);

        uint8_t eid = std::get<uint32_t>(properties.at("EID"));
        auto types = std::get<std::vector<uint8_t>>(
            properties.at("SupportedMessageTypes"));

        // Check if endpoint supports VDM (type 0x7E)
        if (std::find(types.begin(), types.end(), 0x7E) == types.end())
        {
            lg2::info(
                "Endpoint {PATH} does not support VDM messaging, skipping",
                "PATH", path);
            return;
        }
        lg2::debug("Endpoint {PATH} supports VDM messaging", "PATH", path);

        // Get socket info
        method = bus.new_method_call(service.c_str(), path.c_str(),
                                     "org.freedesktop.DBus.Properties",
                                     "GetAll");
        method.append(unixSocketIntf);
        reply = bus.call(method);

        Properties socketProps;
        reply.read(socketProps);

        uint8_t socketType = std::get<uint32_t>(socketProps.at("Type"));
        uint8_t socketProtocol = std::get<uint32_t>(socketProps.at("Protocol"));
        std::vector<uint8_t> address =
            std::get<std::vector<uint8_t>>(socketProps.at("Address"));

        if (enabled)
        {
            lg2::info(
                "Registering endpoint EID {EID} Type={TYPE} Protocol={PROTO}",
                "EID", eid, "TYPE", socketType, "PROTO", socketProtocol);
            if (gpuserver_mctp_add_endpoint(
                    ctx, MCTP_ENDPOINT_ADDED, eid, socketType, socketProtocol,
                    address.data(), address.size()) < 0)
            {
                lg2::error("Failed to register endpoint {EID} at {PATH}", "EID",
                           eid, "PATH", path);
                return;
            }
            lg2::info("Successfully registered endpoint EID {EID}", "EID", eid);
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to process endpoint {PATH}: {ERROR}", "PATH", path,
                   "ERROR", e.what());
    }
}

void MctpDiscovery::scanExistingEndpoints()
{
    lg2::info("Scanning for existing MCTP endpoints");
    try
    {
        auto method = bus.new_method_call("xyz.openbmc_project.ObjectMapper",
                                          "/xyz/openbmc_project/object_mapper",
                                          "xyz.openbmc_project.ObjectMapper",
                                          "GetSubTree");
        method.append(mctpBasePath, 0,
                      std::vector<std::string>{mctpEndpointIntf});

        auto reply = bus.call(method);
        std::map<std::string, std::map<std::string, std::vector<std::string>>>
            objects;
        reply.read(objects);
        lg2::info("Found {COUNT} existing endpoints", "COUNT", objects.size());

        for (const auto& [path, services] : objects)
        {
            for (const auto& [service, interfaces] : services)
            {
                // Set up property monitoring
                if (enableMatches.find(path) == enableMatches.end())
                {
                    enableMatches.emplace(
                        path,
                        std::make_unique<sdbusplus::bus::match_t>(
                            bus,
                            sdbusplus::bus::match::rules::propertiesChanged(
                                path, enableIntf),
                            std::bind(&MctpDiscovery::handlePropertiesChanged,
                                      this, std::placeholders::_1)));
                }

                processEndpoint(path, service);
            }
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to scan existing endpoints: {ERROR}", "ERROR",
                   e.what());
    }
}

} // namespace gpuserver
// NOLINTEND
