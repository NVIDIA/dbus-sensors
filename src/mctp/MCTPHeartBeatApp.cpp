#include "MCTPHeartBeatApp.hpp"

#include <systemd/sd-event.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>

#include <linux/mctp.h>
#include <csignal>  // NOLINT(modernize-deprecated-headers)
#include <sys/socket.h>
#include <sys/time.h>  // NOLINT(misc-include-cleaner)
#include <sys/types.h>
#include <ctime>  // NOLINT(modernize-deprecated-headers)
#include <time.h>  // NOLINT(modernize-deprecated-headers)
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

// Global flag for signal handling
static std::atomic<bool> gRunning{true};
// Global flag for enabling TX/RX debug logging
static constexpr bool gDebugTxRx = false;
static constexpr const char* mctpdBusName = "au.com.codeconstruct.MCTP1";
static constexpr const char* mctpdEndpointControlInterface =
    "xyz.openbmc_project.MCTP.Endpoint";
// Global io_context
static boost::asio::io_context gIo;
// Forward declaration of MCTPHeartbeatService
class MCTPHeartbeatService;

// Now we can declare our global pointer
static std::shared_ptr<MCTPHeartbeatService> gHeartbeatService = nullptr;

// Retry configuration
static constexpr int maxRetries = 5;
static constexpr int retryDelaySec = 1;

// Signal handler forward declaration
void signalHandler(int signum);

static void printHex(const char* msg, const uint8_t* data, int len)
{
    if (!gDebugTxRx)
    {
        return;
    }
    
    std::string output = std::string(msg) + " : ";
    if (data != nullptr)
    {
        for (int i = 0; i < len; ++i)
        {
            std::stringstream ss;
            ss << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<int>(data[i]);
            output += ss.str();
            output += " ";
        }
    }
    lg2::info("{MSG}", "MSG", output);
}

static int cbExitLoopIo(sd_event_source* s, int fd, uint32_t revents,
                        void* userdata)
{
    (void)fd;
    (void)revents;
    (void)userdata;
    sd_event_exit(sd_event_source_get_event(s), 0);
    return 0;
}

static int cbExitLoopTimeout(sd_event_source* s, uint64_t usec, void* userdata)
{
    (void)usec;
    (void)userdata;
    sd_event_exit(sd_event_source_get_event(s), -ETIMEDOUT);
    return 0;
}

/* Events are EPOLLIN, EPOLLOUT etc.
   Returns 0 on ready, negative on error. -ETIMEDOUT on timeout */
static int waitFdTimeout(int fd, short events, uint64_t timeoutUsec)
{
    int rc = 0;
    sd_event* ev = nullptr;

    rc = sd_event_new(&ev);
    if (rc < 0)
    {
        return rc;
    }

    rc = sd_event_add_time_relative(ev, nullptr, CLOCK_MONOTONIC, timeoutUsec, 0,  // NOLINT(misc-include-cleaner)
                                    cbExitLoopTimeout, nullptr);
    if (rc < 0)
    {
        sd_event_unref(ev);
        return rc;
    }

    rc = sd_event_add_io(ev, nullptr, fd, events, cbExitLoopIo, nullptr);
    if (rc < 0)
    {
        sd_event_unref(ev);
        return rc;
    }

    rc = sd_event_loop(ev);

    if (ev != nullptr)
    {
        sd_event_unref(ev);
    }
    return rc;
}

/* Returns the message from a socket. */
static int readMessage(int sd, std::vector<uint8_t>& retBuf,
                       sockaddr_mctp* retAddr)
{
    ssize_t len =
        recvfrom(sd, nullptr, 0, MSG_PEEK | MSG_TRUNC, nullptr, nullptr);
    if (len < 0)
    {
        lg2::error("readMessage returned error: {ERROR}", "ERROR",
                   strerror(errno));
        retBuf.clear();
        return -errno;
    }

    if (len == 0)
    {
        retBuf.clear();
        return 0;
    }

    retBuf.resize(len);

    if (retAddr != nullptr)
    {
        socklen_t addrlen = sizeof(sockaddr_mctp);
        memset(retAddr, 0x0, addrlen);
        len = recvfrom(sd, retBuf.data(), retBuf.size(), MSG_TRUNC,
                       reinterpret_cast<struct sockaddr*>(retAddr), &addrlen);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    }
    else
    {
        len = recvfrom(sd, retBuf.data(), retBuf.size(), MSG_TRUNC, nullptr,
                       nullptr);
    }

    if (len < 0)
    {
        lg2::error("readMessage returned error: {ERROR}", "ERROR",
                   strerror(errno));
        retBuf.clear();
        return -errno;
    }
    if (static_cast<size_t>(len) != retBuf.size())
    {
        lg2::error("BUG: incorrect recvfrom {ACTUAL}, expected {EXPECTED}",
                   "ACTUAL", len, "EXPECTED", retBuf.size());
        retBuf.clear();
        return -EPROTO;
    }

    return 0;
}

/* Use mctpQueryVdmCommand()
 *
 * Extended addressing is used optionally, depending on extAddr arg. */

static int mctpQueryVdmCommand(int sd, const struct sockaddr_mctp* reqAddr,
                               bool extAddr, const void* req, size_t reqLen,
                               std::vector<uint8_t>& resp,
                               struct sockaddr_mctp* respAddr)
{
    resp.clear();

    if (sd < 0)
    {
        lg2::error("socket creation failed");
        return -errno;
    }

    size_t reqAddrLen = 0;
    if (extAddr)
    {
        reqAddrLen = sizeof(struct sockaddr_mctp_ext);
    }
    else
    {
        reqAddrLen = sizeof(struct sockaddr_mctp);
    }

    if (reqLen == 0)
    {
        lg2::error("BUG: zero length request");
        return -EPROTO;
    }

    printHex("TX", static_cast<const uint8_t*>(req), reqLen);
    ssize_t rc =
        sendto(sd, req, reqLen, 0,
              reinterpret_cast<const struct sockaddr*>(reqAddr), reqAddrLen);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (rc < 0)
    {
        lg2::error("sendto failed: {ERROR}", "ERROR", strerror(errno));
        return -errno;
    }
    if (static_cast<size_t>(rc) != reqLen)
    {
        lg2::error("BUG: incorrect sendto {ACTUAL}, expected {EXPECTED}",
                   "ACTUAL", rc, "EXPECTED", reqLen);
        return -EPROTO;
    }

    /* Wait 2.5 seconds for response - this timeout value could be adjusted
     * based on*/
    int waitRc = waitFdTimeout(sd, EPOLLIN, 2500000);
    if (waitRc < 0)
    {
        if (waitRc == -ETIMEDOUT)
        {
            lg2::error("received time out from EID {EID}", "EID", reqAddr->smctp_addr.s_addr);
        }
        return waitRc;
    }

    int readRc = readMessage(sd, resp, respAddr);
    if (readRc < 0)
    {
        return readRc;
    }

    if (respAddr->smctp_type != reqAddr->smctp_type)
    {
        lg2::error(
            "Mismatching response type {RESPONSE_TYPE} for request type {REQUEST_TYPE}",
            "RESPONSE_TYPE", respAddr->smctp_type, "REQUEST_TYPE",
            reqAddr->smctp_type);
        resp.clear();
        return -ENOMSG;
    }

    return 0;
}

/* Wrapper for mctpQueryVdmCommand with retry logic */
static int mctpQueryVdmCommandWithRetry(int sd, const struct sockaddr_mctp* reqAddr,
                                        bool extAddr, const void* req, size_t reqLen,
                                        std::vector<uint8_t>& resp,
                                        struct sockaddr_mctp* respAddr)
{
    for (int attempt = 1; attempt <= maxRetries; ++attempt)
    {
        int rc = mctpQueryVdmCommand(sd, reqAddr, extAddr, req, reqLen, resp, respAddr);
        if (rc == 0)
        {
            return 0;
        }
        
        if (attempt < maxRetries)
        {
            lg2::info("Retrying command (attempt {ATTEMPT}/{MAX})", 
                      "ATTEMPT", attempt + 1, "MAX", maxRetries);
            sleep(retryDelaySec);
        }
    }
    
    lg2::error("Command failed after {MAX} attempts", "MAX", maxRetries);
    return -1;
}

/*
 * Send Restart Notification:
 * AP firmware sends this message to indicate restart notification.
 * Uses mctpQueryVdmCommand() to send and receive the command.
 */
int vdmRestartNotification(int fd, uint8_t tid)
{
    std::vector<uint8_t> resp;
    int rc = -1;
    MctpVendorCmdRestartnoti cmd{};
    struct sockaddr_mctp reqAddr {};
    struct sockaddr_mctp respAddr {};

    /* Encode the VDM headers for Restart notification */
    mctpEncodeVendorCmdRestartnoti(&cmd);

    /* Setup request address */
    reqAddr.smctp_family = AF_MCTP;
    reqAddr.smctp_network = MCTP_NET_ANY;
    reqAddr.smctp_addr.s_addr = tid;
    reqAddr.smctp_type = mctpVendorMsgType;
    reqAddr.smctp_tag = MCTP_TAG_OWNER;

    /* Send and Receive the MCTP-VDM command using mctpQueryVdmCommand */
    rc = mctpQueryVdmCommandWithRetry(fd, &reqAddr,
                             false, /* don't use extended addressing */
                             &cmd, sizeof(cmd), resp, &respAddr);

    if (rc == 0 && !resp.empty())
    {
        /* Decode the response (skip the message type byte) */
        printHex("RX", resp.data() + 1, resp.size() - 1);
    }

    return rc;
}

/*
 * Send Boot Complete V2:
 * AP firmware sends this message to indicate boot completion status.
 * Uses mctpQueryVdmCommand() to send and receive the command.
 */
int vdmBootCompleteV2(int fd, uint8_t tid, uint8_t valid, uint8_t slot)
{
    std::vector<uint8_t> resp;
    int rc = -1;
    MctpVendorCmdBootcompleteV2 cmd{};
    struct sockaddr_mctp reqAddr {};
    struct sockaddr_mctp respAddr {};

    /* Encode the VDM headers for Boot Complete V2 */
    mctpEncodeVendorCmdBootcmpltV2(&cmd);
    cmd.valid = valid;
    cmd.slot = slot;

    /* Setup request address */
    reqAddr.smctp_family = AF_MCTP;
    reqAddr.smctp_network = MCTP_NET_ANY;
    reqAddr.smctp_addr.s_addr = tid;
    reqAddr.smctp_type = mctpVendorMsgType;
    reqAddr.smctp_tag = MCTP_TAG_OWNER;

    /* Send and Receive the MCTP-VDM command using mctpQueryVdmCommand */
    rc = mctpQueryVdmCommandWithRetry(fd, &reqAddr,
                             false, /* don't use extended addressing */
                             &cmd, sizeof(cmd), resp, &respAddr);

    if (rc == 0 && !resp.empty())
    {
        /* Decode the response (skip the message type byte) */
        printHex("RX", resp.data() + 1, resp.size() - 1);
    }

    return rc;
}

/*
 * Set Heartbeat Enable/Disable:
 * Sometimes AP firmware might not be able to do heartbeat reporting,
 * and in this case, AP firmware can send this message to disable
 * heartbeat reporting, and enable it later.
 * Uses mctpQueryVdmCommand() to send and receive the command.
 */
int vdmSetHeartbeatEnable(int fd, uint8_t tid, int enable)
{
    std::vector<uint8_t> resp;
    int rc = -1;
    MctpVendorCmdHbenable cmd{};
    struct sockaddr_mctp reqAddr {};
    struct sockaddr_mctp respAddr {};

    /* Encode the VDM headers for Heartbeat enable/disable */
    mctpEncodeVendorCmdHbenable(&cmd);

    /* Update enable field */
    cmd.enable = enable;

    /* Setup request address */
    reqAddr.smctp_family = AF_MCTP;
    reqAddr.smctp_network = MCTP_NET_ANY;
    reqAddr.smctp_addr.s_addr = tid;
    reqAddr.smctp_type = mctpVendorMsgType;
    reqAddr.smctp_tag = MCTP_TAG_OWNER;

    /* Send and Receive the MCTP-VDM command using mctpQueryVdmCommand */
    rc = mctpQueryVdmCommandWithRetry(fd, &reqAddr,
                             false, /* don't use extended addressing */
                             &cmd, sizeof(cmd), resp, &respAddr);

    if (rc == 0 && !resp.empty())
    {
        /* Decode the response (skip the message type byte) */
        printHex("RX", resp.data() + 1, resp.size() - 1);
    }
    else
    {
        lg2::error("failed to send/receive command, rc={RC}", "RC", rc);
    }

    return rc;
}

/*
 * Send Heartbeat VDM command:
 * Sends a heartbeat message to the target EID and gets the response.
 * Uses mctpQueryVdmCommand() to send and receive the command.
 */
int vdmSendHeartbeat(int fd, uint8_t tid)
{
    std::vector<uint8_t> resp;
    int rc = -1;
    struct sockaddr_mctp reqAddr {};
    struct sockaddr_mctp respAddr {};
    MctpVendorCmdHbenvent cmd{};

    /* Encode the VDM headers for Heartbeat command */
    mctpEncodeVendorCmdHbenvent(&cmd);

    /* Setup request address */
    reqAddr.smctp_family = AF_MCTP;
    reqAddr.smctp_network = MCTP_NET_ANY;
    reqAddr.smctp_addr.s_addr = tid;
    reqAddr.smctp_type = mctpVendorMsgType;
    reqAddr.smctp_tag = MCTP_TAG_OWNER;

    /* Send and Receive the MCTP-VDM command using mctpQueryVdmCommand */
    rc = mctpQueryVdmCommandWithRetry(fd, &reqAddr,
                             false, /* don't use extended addressing */
                             &cmd, sizeof(cmd), resp, &respAddr);

    if (rc == 0 && !resp.empty())
    {
        /* Print the raw response buffer */
        printHex("RX", resp.data() + 1, resp.size() - 1);
    }
    else
    {
        lg2::error("failed to send/receive command, rc={RC}", "RC", rc);
    }

    return rc;
}

class MCTPHeartbeatService
{
  private:
    int fd;
    uint8_t targetEid;
    boost::asio::io_context& io;
    std::shared_ptr<boost::asio::steady_timer> heartbeatTimer;
    static constexpr int heartbeatIntervalSec = 30;

    static int initializeMctpSocket()
    {
        /* Setup AF_MCTP socket*/
        int sockFd = socket(AF_MCTP, SOCK_DGRAM, 0);
        if (sockFd < 0)
        {
            lg2::error("AF_MCTP socket bind failed: {ERROR}", "ERROR",
                       strerror(errno));
            return -1;
        }

        struct sockaddr_mctp addr {};
        addr.smctp_family = AF_MCTP;
        addr.smctp_network = MCTP_NET_ANY;
        addr.smctp_addr.s_addr = MCTP_ADDR_ANY;
        addr.smctp_type = mctpVendorMsgType;
        addr.smctp_tag = MCTP_TAG_OWNER;

        if (bind(sockFd, reinterpret_cast<struct sockaddr*>(&addr),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                 sizeof(addr)) < 0)
        {
            lg2::error("AF_MCTP socket bind failed: {ERROR}", "ERROR",
                       strerror(errno));
            close(sockFd);
            return -1;
        }

        /* Set socket timeout */
        struct timeval timeout {};  // NOLINT(misc-include-cleaner)
        timeout.tv_sec = 5; // 5 seconds timeout
        timeout.tv_usec = 0;

        if (setsockopt(sockFd, SOL_SOCKET, SO_RCVTIMEO,  // NOLINT(misc-include-cleaner)
                       reinterpret_cast<char*>(&timeout), sizeof(timeout)) < 0)  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        {
            lg2::error("AF_MCTP socket setsockopt failed: {ERROR}", "ERROR",
                       strerror(errno));
            close(sockFd);
            return -1;
        }

        /* Enable extended addressing */
        int val = 1;
        if (setsockopt(sockFd, SOL_MCTP, MCTP_OPT_ADDR_EXT, &val,
                       sizeof(val)) < 0)
        {
            lg2::error(
                "Kernel does not support MCTP extended addressing: {ERROR}",
                "ERROR", strerror(errno));
            close(sockFd);
            return -1;
        }

        return sockFd;
    }

  public:
    MCTPHeartbeatService(boost::asio::io_context& ioContext, uint8_t eid) :
        fd(initializeMctpSocket()), targetEid(eid), io(ioContext),
        heartbeatTimer(std::make_shared<boost::asio::steady_timer>(io))
    {
        if (fd < 0)
        {
            throw std::runtime_error("Failed to initialize MCTP socket");
        }
    }

    ~MCTPHeartbeatService()
    {
        if (fd >= 0)
        {
            close(fd);
        }
    }

    void run()
    {
        // Setup signal handler
        struct sigaction sa {};  // NOLINT(misc-include-cleaner)
        sa.sa_handler = signalHandler;  // NOLINT(misc-include-cleaner)
        sigemptyset(&sa.sa_mask);  // NOLINT(misc-include-cleaner)
        sa.sa_flags = 0;
        sigaction(SIGTERM, &sa, nullptr);  // NOLINT(misc-include-cleaner)

        // Send boot complete v2 command first
        if (vdmBootCompleteV2(fd, targetEid, 1, 0) != 0)
        {
            lg2::error("Failed to send boot complete v2");
            return;
        }

        lg2::info("Boot complete v2 command successful");

        // Enable heartbeat first
        if (vdmSetHeartbeatEnable(fd, targetEid, 1) != 0)
        {
            lg2::error("Failed to enable heartbeat");
            return;
        }

        lg2::info("Heartbeat enabled");

        // Send heartbeat message
        if (vdmSendHeartbeat(fd, targetEid) < 0)
        {
            lg2::error("Failed to send heartbeat VDM");
            return;
        }

        lg2::info("Heartbeat VDM successful");
        // Start the heartbeat timer
        scheduleNextHeartbeat();
    }

    void stop(bool sendRestart)
    {
        // Cancel any pending heartbeat
        if (heartbeatTimer)
        {
            heartbeatTimer->cancel();
            lg2::info("Heartbeat cancelled");
        }
        if (!sendRestart)
        {
            return;
        }
        // Send restart notification before exiting
        if (vdmRestartNotification(fd, targetEid) != 0)
        {
            lg2::error("Failed to send restart notification");
        }
        else
        {
            lg2::info("Restart notification sent");
        }

        lg2::info("Heartbeat service stopped");
    }

  private:
    void scheduleNextHeartbeat()
    {
        if (!gRunning)
        {
            return;
        }

        heartbeatTimer->expires_after(
            std::chrono::seconds(heartbeatIntervalSec));
        heartbeatTimer->async_wait([this](const boost::system::error_code& ec) {
            if (ec)
            {
                // Timer was cancelled or error occurred
                return;
            }

            if (!gRunning)
            {
                return;
            }

            // Send heartbeat message
            if (vdmSendHeartbeat(fd, targetEid) < 0)
            {
                lg2::error("Failed to send heartbeat VDM");
            }
            else
            {
                lg2::info("Heartbeat VDM successful");
            }

            // Schedule the next heartbeat
            scheduleNextHeartbeat();
        });
    }
};

// Now implement the signal handler after MCTPHeartbeatService is fully defined
void signalHandler(int signum)
{
    if (signum == SIGTERM)
    {
        gRunning = false;
        lg2::info("Termination signal received");
        // Stop the io_context to break out of io.run()
        gIo.stop();
        if (gHeartbeatService)
        {
            gHeartbeatService->stop(true);
        }
    }
}

static void
    addedSPIEndpoint(const std::shared_ptr<sdbusplus::asio::connection>&
                         systemBus,
                     uint8_t eid, boost::asio::io_context& io)
{
    // Add this line to mark systemBus as used and avoid the unused parameter
    // warning
    (void)systemBus;

    lg2::info("SPI endpoint added");

    try
    {
        // Don't create a new service if one already exists
        if (!gHeartbeatService)
        {
            lg2::info("Creating new MCTPHeartbeatService for EID {EID}", "EID",
                      eid);
            gHeartbeatService =
                std::make_shared<MCTPHeartbeatService>(io, eid);
        }
        else
        {
            lg2::info("MCTPHeartbeatService already exists for EID {EID}",
                      "EID", eid);
        }
        gHeartbeatService->run();
    }
    catch (const std::logic_error& e)
    {
        lg2::error(
            "Addition of inventory at caused an invalid program state: {EXCEPTION}",
            "EXCEPTION", e);
    }
    catch (const std::system_error& e)
    {
        lg2::error(
            "Failed to manage device described by inventory at {EXCEPTION}",
            "EXCEPTION", e);
    }
}

// Function to check if an endpoint exists and initialize heartbeat if it does
static void checkExistingEndpoint(
    const std::shared_ptr<sdbusplus::asio::connection>& systemBus,
    uint8_t eid, boost::asio::io_context& io)
{
    // Create the object path string properly
    std::string objpath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/" +
        std::to_string(eid);
    systemBus->async_method_call(
        [systemBus, eid, &io,
         objpath](const boost::system::error_code& ec,
                  const std::variant<std::string>& value) {
        (void)value;
        if (ec)
        {
            lg2::info("Failed to get EID assuing missing: {ERROR_MESSAGE}",
                      "ERROR_MESSAGE", ec.message(), "ERROR_CATEGORY",
                      ec.category().name(), "ERROR_CODE", ec.value());
            return;
        }

        lg2::info("Found existing SPI endpoint {EID}", "EID", eid);
        addedSPIEndpoint(systemBus, eid, io);
    },
        mctpdBusName, objpath, "org.freedesktop.DBus.Properties", "Get",
        mctpdEndpointControlInterface, "EID");
}

int main()
{
    try
    {
        // Single EID configuration
        uint8_t eid = 10; // Example target SPI HMC EROT
        // Use global io_context instead of local
        auto systemBus = std::make_shared<sdbusplus::asio::connection>(gIo);
        using namespace sdbusplus::bus::match;
        systemBus->request_name("xyz.openbmc_project.MCTPHeartBeat");

        // Check if the endpoint already exists (handle service restart case)
        checkExistingEndpoint(systemBus, eid, gIo);

        // Match for when the endpoint is removed
        const std::string path =
            "/au/com/codeconstruct/mctp1/networks/1/endpoints/" +
            std::to_string(eid);
        lg2::info("path to match: {PATH}", "PATH", path);

        const std::string interfacesRemovedMatchSpec =
            rules::sender("au.com.codeconstruct.MCTP1") +
            rules::interfacesRemovedAtPath(path);

        const std::string interfacesAddedMatchSpec =
            rules::sender("au.com.codeconstruct.MCTP1") +
            rules::interfacesAddedAtPath(path);

        // Fix the match object creation - update to use gIo instead of io
        auto interfacesAddedMatch =
            match(*systemBus, interfacesAddedMatchSpec,
                  [systemBus, eid](auto&) {
            lg2::info("interfacesAddedMatch for SPI endpoint {EID}", "EID",
                      eid);
            addedSPIEndpoint(systemBus, eid, gIo);
        });
        auto interfacesRemovedMatch = match(
            *systemBus, interfacesRemovedMatchSpec, [](auto&) {
            lg2::info("interfacesRemovedMatch for SPI endpoint");
            if (gHeartbeatService)
            {
                gHeartbeatService->stop(false);
            }
        });

        // Setup signal handler for process termination
        struct sigaction sa {};  // NOLINT(misc-include-cleaner)
        sa.sa_handler = signalHandler;  // NOLINT(misc-include-cleaner)
        sigemptyset(&sa.sa_mask);  // NOLINT(misc-include-cleaner)
        sa.sa_flags = 0;
        sigaction(SIGTERM, &sa, nullptr);  // NOLINT(misc-include-cleaner)

        // to keep service running
        // Need to keep the service alive unless SIGTERM is received
        constexpr std::chrono::seconds period(5);
        boost::asio::steady_timer clock(gIo);
        std::function<void(const boost::system::error_code&)> alarm =
            [&](const boost::system::error_code& ec) {
            if (ec)
            {
                return;
            }

            // We still check gRunning as a backup to avoid race condition
            if (!gRunning)
            {
                lg2::info("Stopping IO context due to termination signal");
                gIo.stop();
                if (gHeartbeatService)
                {
                    gHeartbeatService->stop(true);
                }
                return;
            }
            clock.expires_after(period);
            clock.async_wait(alarm);
        };
        clock.expires_after(period);
        clock.async_wait(alarm);
        gIo.run();
    }
    catch (const std::exception& e)
    {
        lg2::error("Error: {ERROR}", "ERROR", e.what());
        return 1;
    }

    return 0;
}