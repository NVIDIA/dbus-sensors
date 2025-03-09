// NOLINTBEGIN
#include "manager.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>

namespace gpuserver
{

Manager::Manager(
    sdeventplus::Event& event, const std::string& socketPath,
    requester::Handler<requester::Request>& reqHandler,
    mctp::EndpointManager& endpointManager, bool verbose) :
    event(event),
    socketPath(socketPath), reqHandler(reqHandler),
    endpointManager(endpointManager), verbose(verbose)
{
    cleanupSocket();
    initServerSocket();
}

Manager::~Manager()
{
    clientConnections.clear();
    if (serverFd >= 0)
    {
        close(serverFd);
    }
    cleanupSocket();
}

void Manager::cleanupSocket()
{
    if (std::filesystem::exists(socketPath))
    {
        try
        {
            std::filesystem::remove(socketPath);
            if (verbose)
            {
                lg2::info("Removed existing socket file at {PATH}", "PATH",
                          socketPath);
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            lg2::error("Failed to remove existing socket file: {ERROR}",
                       "ERROR", e.what());
            throw std::runtime_error("Unable to cleanup existing socket file");
        }
    }
}

void Manager::initServerSocket()
{
    serverFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (serverFd < 0)
    {
        throw std::runtime_error("Failed to create server socket");
    }

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(serverFd, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) < 0)
    {
        auto errnum = errno;
        close(serverFd);
        throw std::runtime_error("Failed to bind server socket: " +
                                 std::string(strerror(errnum)));
    }

    if (chmod(socketPath.c_str(), 0666) < 0)
    {
        close(serverFd);
        throw std::runtime_error("Failed to set socket permissions");
    }

    if (listen(serverFd, SOMAXCONN) < 0)
    {
        close(serverFd);
        throw std::runtime_error("Failed to listen on server socket");
    }

    serverIO = std::make_unique<IO>(
        event, serverFd, EPOLLIN,
        std::bind_front(&Manager::handleClientConnection, this));
    serverIO->set_priority(sdEventSourceMaxPriority);

    if (verbose)
    {
        lg2::info("Manager: Listening on Unix socket {PATH}", "PATH",
                  socketPath);
    }
}

void Manager::handleClientConnection(IO& io [[maybe_unused]], int fd,
                                              uint32_t revents)
{
    if (!(revents & EPOLLIN))
    {
        if (verbose)
            lg2::info("No new connections pending on server socket");
        return;
    }

    struct sockaddr_un clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);

    // Accept new client connection
    int clientFd = accept4(fd, reinterpret_cast<struct sockaddr*>(&clientAddr),
                           &clientAddrLen, SOCK_NONBLOCK);
    if (clientFd < 0)
    {
        lg2::error("Failed to accept client connection: {ERROR}", "ERROR",
                   strerror(errno));
        return;
    }

    if (verbose)
    {
        lg2::info("Accepted new client connection on fd {FD}", "FD", clientFd);
    }

    try
    {
        // Create new client connection handler
        auto client = std::make_unique<ClientConnection>(
            event, clientFd, reqHandler, endpointManager, verbose,
            [this](int fd) { removeClientConnection(fd); });

        // Store client connection
        clientConnections.emplace(clientFd, std::move(client));

        if (verbose)
        {
            lg2::info(
                "Successfully initialized new client connection handler for fd {FD}",
                "FD", clientFd);
            lg2::info("Total active connections: {COUNT}", "COUNT",
                      clientConnections.size());
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to create client connection: {ERROR}", "ERROR",
                   e.what());
        close(clientFd);
    }
}

void Manager::removeClientConnection(int fd)
{
    if (verbose)
    {
        lg2::info("Removing client connection for fd {FD}", "FD", fd);
    }
    clientConnections.erase(fd);
}

} // namespace gpuserver
// NOLINTEND
