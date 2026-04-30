#include "OpenOCDPortForwardPorts.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/write.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using error_code = boost::system::error_code;

constexpr const char* serviceName = "com.nvidia.openocdportforward";
constexpr const char* objectPath = "/com/nvidia/openocdportforward";
constexpr const char* interfaceName = "xyz.openbmc_project.Object.Enable";
constexpr const char* propertyName = "Enabled";

constexpr const char* defaultRemoteHost = "172.31.13.251";
constexpr const char* defaultPortList = "4444:6666:10000:10001:10002";
constexpr std::size_t maxAcceptedSessions = 16;
constexpr std::size_t transferBufferSize = 8192;

constexpr int keepIdleSeconds = 60;
constexpr int keepIntervalSeconds = 10;
constexpr int keepProbes = 3;

void enableTcpKeepalive(tcp::socket& sock)
{
    error_code ec;
    sock.set_option(asio::socket_base::keep_alive(true), ec);
    if (ec)
    {
        lg2::error("set_option(keep_alive) failed: {ERR}", "ERR", ec.message());
    }

    int fd = sock.native_handle();
    if (fd < 0)
    {
        return;
    }

    int idle = keepIdleSeconds;
    int interval = keepIntervalSeconds;
    int probes = keepProbes;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) != 0)
    {
        lg2::error("setsockopt(TCP_KEEPIDLE) failed: {ERRNO}", "ERRNO", errno);
    }
    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval,
                     sizeof(interval)) != 0)
    {
        lg2::error("setsockopt(TCP_KEEPINTVL) failed: {ERRNO}", "ERRNO", errno);
    }
    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof(probes)) !=
        0)
    {
        lg2::error("setsockopt(TCP_KEEPCNT) failed: {ERRNO}", "ERRNO", errno);
    }
}

class ProxySession : public std::enable_shared_from_this<ProxySession>
{
  public:
    using TrackCallback =
        std::function<void(const std::shared_ptr<ProxySession>&)>;
    using StopCallback =
        std::function<void(const std::shared_ptr<ProxySession>&)>;

    ProxySession(asio::io_context& io, std::string remoteHost,
                 uint16_t remotePort, TrackCallback onTrackIn,
                 StopCallback onStopIn) :
        clientSocket(io), remoteSocket(io), host(std::move(remoteHost)),
        port(remotePort), onTrackCb(std::move(onTrackIn)),
        onStopCb(std::move(onStopIn))
    {}

    tcp::socket& socket()
    {
        return clientSocket;
    }

    void start()
    {
        tracked = true;
        if (onTrackCb)
        {
            onTrackCb(shared_from_this());
        }

        enableTcpKeepalive(clientSocket);

        tcp::resolver resolver(clientSocket.get_executor());
        error_code resolveEc;
        auto results = resolver.resolve(host, std::to_string(port), resolveEc);
        if (resolveEc || results.empty())
        {
            lg2::error("resolve OpenOCD remote '{HOST}:{PORT}' failed: {ERR}",
                       "HOST", host, "PORT", port, "ERR",
                       resolveEc ? resolveEc.message()
                                 : std::string{"no addresses returned"});
            stop();
            return;
        }

        auto self = shared_from_this();
        asio::async_connect(remoteSocket, results,
                            [self](const error_code& ec, const tcp::endpoint&) {
                                self->onRemoteConnect(ec);
                            });
    }

    void stop()
    {
        if (stopped)
        {
            return;
        }
        stopped = true;

        closeSocket(clientSocket, "client");
        closeSocket(remoteSocket, "remote");

        if (tracked && onStopCb)
        {
            onStopCb(shared_from_this());
        }
    }

  private:
    void onRemoteConnect(const error_code& ec)
    {
        if (ec)
        {
            lg2::error("OpenOCD remote connect failed for port {PORT}: {ERR}",
                       "PORT", port, "ERR", ec.message());
            stop();
            return;
        }

        enableTcpKeepalive(remoteSocket);
        startClientRead();
        startRemoteRead();
    }

    void startClientRead()
    {
        auto self = shared_from_this();
        clientSocket.async_read_some(
            asio::buffer(clientToRemoteBuffer),
            [self](const error_code& ec, std::size_t bytes) {
                self->onClientRead(ec, bytes);
            });
    }

    void onClientRead(const error_code& ec, std::size_t bytes)
    {
        if (ec)
        {
            stop();
            return;
        }
        auto self = shared_from_this();
        asio::async_write(remoteSocket,
                          asio::buffer(clientToRemoteBuffer.data(), bytes),
                          [self](const error_code& writeEc, std::size_t) {
                              self->onClientWriteDone(writeEc);
                          });
    }

    void onClientWriteDone(const error_code& ec)
    {
        if (ec)
        {
            stop();
            return;
        }
        startClientRead();
    }

    void startRemoteRead()
    {
        auto self = shared_from_this();
        remoteSocket.async_read_some(
            asio::buffer(remoteToClientBuffer),
            [self](const error_code& ec, std::size_t bytes) {
                self->onRemoteRead(ec, bytes);
            });
    }

    void onRemoteRead(const error_code& ec, std::size_t bytes)
    {
        if (ec)
        {
            stop();
            return;
        }
        auto self = shared_from_this();
        asio::async_write(clientSocket,
                          asio::buffer(remoteToClientBuffer.data(), bytes),
                          [self](const error_code& writeEc, std::size_t) {
                              self->onRemoteWriteDone(writeEc);
                          });
    }

    void onRemoteWriteDone(const error_code& ec)
    {
        if (ec)
        {
            stop();
            return;
        }
        startRemoteRead();
    }

    void closeSocket(tcp::socket& sock, const char* label)
    {
        if (!sock.is_open())
        {
            return;
        }

        error_code ec;
        sock.shutdown(tcp::socket::shutdown_both, ec);
        if (ec && ec != asio::error::not_connected)
        {
            lg2::error("{LABEL} shutdown failed for port {PORT}: {ERR}",
                       "LABEL", label, "PORT", port, "ERR", ec.message());
        }
        sock.close(ec);
        if (ec)
        {
            lg2::error("{LABEL} close failed for port {PORT}: {ERR}", "LABEL",
                       label, "PORT", port, "ERR", ec.message());
        }
    }

    tcp::socket clientSocket;
    tcp::socket remoteSocket;
    std::string host;
    uint16_t port;
    TrackCallback onTrackCb;
    StopCallback onStopCb;
    std::array<char, transferBufferSize> clientToRemoteBuffer{};
    std::array<char, transferBufferSize> remoteToClientBuffer{};
    bool tracked = false;
    bool stopped = false;
};

class Listener : public std::enable_shared_from_this<Listener>
{
  public:
    using AcceptGate = std::function<bool()>;
    using SessionTracker =
        std::function<void(const std::shared_ptr<ProxySession>&)>;
    using SessionRemover =
        std::function<void(const std::shared_ptr<ProxySession>&)>;

    Listener(asio::io_context& ioIn, std::string remoteHost, uint16_t portIn,
             AcceptGate gate, SessionTracker tracker, SessionRemover remover) :
        io(ioIn), acceptor(ioIn), host(std::move(remoteHost)), port(portIn),
        acceptGate(std::move(gate)), trackSession(std::move(tracker)),
        removeSession(std::move(remover))
    {}

    void start()
    {
        tcp::endpoint endpoint(tcp::v6(), port);
        acceptor.open(endpoint.protocol());
        acceptor.set_option(asio::ip::v6_only(false));
        acceptor.set_option(tcp::acceptor::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen();
        doAccept();
    }

    void stop()
    {
        error_code ec;
        acceptor.cancel(ec);
        if (ec && ec != asio::error::operation_aborted)
        {
            lg2::error("acceptor.cancel failed for port {PORT}: {ERR}", "PORT",
                       port, "ERR", ec.message());
        }
        acceptor.close(ec);
        if (ec)
        {
            lg2::error("acceptor.close failed for port {PORT}: {ERR}", "PORT",
                       port, "ERR", ec.message());
        }
    }

  private:
    void doAccept()
    {
        auto session = std::make_shared<ProxySession>(
            io, host, port, trackSession, removeSession);
        auto self = shared_from_this();
        acceptor.async_accept(session->socket(),
                              [self, session](const error_code& ec) {
                                  self->onAccept(session, ec);
                              });
    }

    void onAccept(const std::shared_ptr<ProxySession>& session,
                  const error_code& ec)
    {
        if (!ec)
        {
            if (acceptGate())
            {
                session->start();
            }
            else
            {
                error_code closeEc;
                session->socket().close(closeEc);
                if (closeEc)
                {
                    lg2::error(
                        "rejected-session close failed for port {PORT}: {ERR}",
                        "PORT", port, "ERR", closeEc.message());
                }
                lg2::warning(
                    "OpenOCD port forward: max sessions ({MAX}) reached; "
                    "rejecting new connection on port {PORT}.",
                    "MAX", maxAcceptedSessions, "PORT", port);
            }
        }
        if (acceptor.is_open())
        {
            doAccept();
        }
    }

    asio::io_context& io;
    tcp::acceptor acceptor;
    std::string host;
    uint16_t port;
    AcceptGate acceptGate;
    SessionTracker trackSession;
    SessionRemover removeSession;
};

class OpenOCDPortForwardService
{
  public:
    OpenOCDPortForwardService(asio::io_context& ioIn,
                              sdbusplus::asio::object_server& objectServerIn) :
        io(ioIn), objectServer(objectServerIn)
    {
        iface = objectServer.add_interface(objectPath, interfaceName);

        iface->register_property(
            propertyName, enabled,
            [this](const bool& requested, bool& current) {
                return onEnabledSet(requested, current);
            },
            [this](bool& current) { return onEnabledGet(current); });

        iface->initialize();

        applyEnabled(enabled);
    }

    ~OpenOCDPortForwardService()
    {
        stopForwarding();
        objectServer.remove_interface(iface);
    }

    OpenOCDPortForwardService(const OpenOCDPortForwardService&) = delete;
    OpenOCDPortForwardService& operator=(const OpenOCDPortForwardService&) =
        delete;
    OpenOCDPortForwardService(OpenOCDPortForwardService&&) = delete;
    OpenOCDPortForwardService& operator=(OpenOCDPortForwardService&&) = delete;

  private:
    int onEnabledSet(const bool& requested, bool& current)
    {
        if (requested == current)
        {
            return 0;
        }
        if (!setEnabled(requested))
        {
            current = enabled;
            return 0;
        }
        current = enabled;
        return 1;
    }

    bool onEnabledGet(bool& current)
    {
        if (current != enabled)
        {
            current = enabled;
        }
        return current;
    }

    bool canAcceptNewSession() const
    {
        return sessions.size() < maxAcceptedSessions;
    }

    void trackSession(const std::shared_ptr<ProxySession>& session)
    {
        sessions.push_back(session);
    }

    void removeSession(const std::shared_ptr<ProxySession>& session)
    {
        sessions.remove_if(
            [&session](const std::shared_ptr<ProxySession>& candidate) {
                return candidate == session;
            });
    }

    bool startForwarding()
    {
        if (forwardingActive)
        {
            return true;
        }

        try
        {
            std::list<std::shared_ptr<Listener>> newListeners;
            for (uint16_t port : forwardedPorts)
            {
                auto listener = std::make_shared<Listener>(
                    io, remoteHost, port,
                    [this] { return canAcceptNewSession(); },
                    [this](const std::shared_ptr<ProxySession>& s) {
                        trackSession(s);
                    },
                    [this](const std::shared_ptr<ProxySession>& s) {
                        removeSession(s);
                    });
                listener->start();
                newListeners.push_back(listener);
            }
            listeners = std::move(newListeners);
            forwardingActive = true;
            return true;
        }
        catch (const std::exception& e)
        {
            lg2::error("OpenOCD port forward: failed to start: {ERR}", "ERR",
                       e.what());
            stopForwarding();
            return false;
        }
    }

    void stopForwarding()
    {
        for (const auto& listener : listeners)
        {
            listener->stop();
        }
        listeners.clear();

        while (!sessions.empty())
        {
            sessions.front()->stop();
        }
        forwardingActive = false;
    }

    bool applyEnabled(bool value)
    {
        if (!value)
        {
            stopForwarding();
            return true;
        }

        return startForwarding();
    }

    bool setEnabled(bool value)
    {
        if (!applyEnabled(value))
        {
            return false;
        }
        enabled = value;
        return true;
    }

    asio::io_context& io;
    sdbusplus::asio::object_server& objectServer;
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface;
    bool enabled = false;
    bool forwardingActive = false;
    std::string remoteHost = defaultRemoteHost;
    std::vector<uint16_t> forwardedPorts =
        openocd_port_forward::parsePorts(defaultPortList);
    std::list<std::shared_ptr<Listener>> listeners;
    std::list<std::shared_ptr<ProxySession>> sessions;
};

} // namespace

int main()
{
    try
    {
        asio::io_context io;
        auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
        systemBus->request_name(serviceName);

        sdbusplus::asio::object_server objectServer(systemBus);
        OpenOCDPortForwardService service(io, objectServer);

        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&io](const boost::system::error_code&, int) {
            io.stop();
        });

        io.run();
        return 0;
    }
    catch (const std::exception& ex)
    {
        lg2::error("openocdportforward fatal: {ERR}", "ERR", ex.what());
        return EXIT_FAILURE;
    }
}
