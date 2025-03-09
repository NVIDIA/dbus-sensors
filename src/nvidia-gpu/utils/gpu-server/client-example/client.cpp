// NOLINTBEGIN
#include "gpuserver.h"

#include <errno.h>
#include <poll.h>
#include <string.h> // for strerror
#include <sys/epoll.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

class GpuClient
{
  public:
    GpuClient(const std::string& socketPath, uint8_t eid) : eid(eid)
    {
        ctx = gpuserver_connect(socketPath.c_str());
        if (!ctx)
        {
            throw std::runtime_error("Failed to connect to gpuserver daemon");
        }

        epollFd = epoll_create1(0);
        if (epollFd < 0)
        {
            gpuserver_close(ctx);
            throw std::runtime_error("Failed to create epoll instance");
        }

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = gpuserver_get_fd(ctx);

        if (epoll_ctl(epollFd, EPOLL_CTL_ADD, ev.data.fd, &ev) < 0)
        {
            close(epollFd);
            gpuserver_close(ctx);
            throw std::runtime_error("Failed to add fd to epoll");
        }
    }

    ~GpuClient()
    {
        if (epollFd >= 0)
        {
            close(epollFd);
        }
        if (ctx)
        {
            gpuserver_close(ctx);
        }
    }

    void run()
    {
        while (true)
        {
            std::vector<uint8_t> msg({0x10, 0xde, 0x81, 0x89, 0x03, 0x00, 0x01,
                                      0xff}); // Type-3: Get Temperature

            // Send message with API headers using the provided EID
            ssize_t sent =
                gpuserver_send_msg(ctx, GPUSERVER_API_PASSTHROUGH_EID,
                                       eid, // Use EID from constructor
                                       msg.data(), msg.size());
            if (sent < 0)
            {
                std::cerr << "Error sending message: " << strerror(-sent)
                          << std::endl;
                break;
            }

            // Wait for response with infinite timeout
            struct epoll_event events[1];
            int nfds = epoll_wait(epollFd, events, 1, -1);
            if (nfds < 0)
            {
                std::cerr << "Error in epoll_wait" << std::endl;
                break;
            }

            // Receive response
            std::vector<uint8_t> response(1024);
            ssize_t respLen = gpuserver_recv(ctx, response.data(),
                                                 response.size());
            if (respLen < 0)
            {
                std::cerr << "Error receiving response: " << strerror(-respLen)
                          << std::endl;
                break;
            }

            std::cout << "Received response: ";
            for (size_t i = 0; i < static_cast<size_t>(respLen); i++)
            {
                std::cout << static_cast<int>(response[i]) << " ";
            }
            std::cout << std::endl;

            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

  private:
    gpuserver_ctx* ctx{nullptr};
    int epollFd{-1};
    uint8_t eid; // Store EID as member variable
};

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <EID> [socket_path]"
                  << std::endl;
        return 1;
    }

    // Parse EID from command line
    char* endptr;
    unsigned long eid_val = strtoul(argv[1], &endptr, 0);
    if (*endptr != '\0' || eid_val > 0xFF)
    {
        std::cerr << "Invalid EID value. Must be between 0x00 and 0xFF"
                  << std::endl;
        return 1;
    }
    uint8_t eid = static_cast<uint8_t>(eid_val);

    // Get socket path from command line or use default
    std::string socketPath = "/run/gpuserverd.sock";
    if (argc > 2)
    {
        socketPath = argv[2];
    }

    try
    {
        GpuClient client(socketPath, eid);
        client.run();
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
// NOLINTEND
