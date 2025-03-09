// NOLINTBEGIN
#include "client_connection.hpp"

#include "mctp_endpoint_manager.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <coroutine.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdeventplus/source/base.hpp>

namespace gpuserver
{

ClientConnection::ClientConnection(
    sdeventplus::Event& event, int fd,
    requester::Handler<requester::Request>& reqHandler,
    mctp::EndpointManager& endpointManager, bool verbose,
    DisconnectCallback cb) :
    fd(fd), reqHandler(reqHandler), endpointManager(endpointManager),
    verbose(verbose), disconnectCallback(std::move(cb))
{
    io = std::make_unique<sdeventplus::source::IO>(
        event, fd, EPOLLIN,
        std::bind_front(&ClientConnection::handleMessage, this));
    io->set_priority(sdEventSourceMaxPriority);
}

ClientConnection::~ClientConnection()
{
    io.reset();
    if (fd >= 0)
    {
        close(fd);
    }
}

void ClientConnection::handleMessage(sdeventplus::source::IO& io, int fd,
                                     uint32_t revents)
{
    if (!(revents & EPOLLIN))
    {
        if (verbose)
            lg2::info("No data available to read on fd {FD}", "FD", fd);
        return;
    }

    // Read the message in chunks using a fixed-size buffer
    static constexpr size_t BUFFER_SIZE = 4096; // Reasonable buffer size
    std::vector<uint8_t> buffer(BUFFER_SIZE);

    ssize_t len = recv(fd, buffer.data(), BUFFER_SIZE, 0);

    // Enhanced error handling and logging
    if (len <= 0)
    {
        if (len == 0)
        {
            if (verbose)
            {
                lg2::info("Client disconnected gracefully on fd {FD}", "FD",
                          fd);
            }
        }
        else
        {
            lg2::error("Error reading from client: {ERROR} (fd={FD})", "ERROR",
                       strerror(errno), "FD", fd);
        }

        io.set_enabled(sdeventplus::source::Enabled::Off);
        if (disconnectCallback)
        {
            if (verbose)
                lg2::info("Triggering disconnect callback for fd {FD}", "FD",
                          fd);
            disconnectCallback(fd);
        }
        return;
    }

    if (static_cast<unsigned int>(len) < sizeof(struct gpuserver_api_msg))
    {
        lg2::error("Received incomplete message header");
        return;
    }

    // Validate the header
    auto* header = reinterpret_cast<struct gpuserver_api_msg*>(buffer.data());

    // Sanity check the payload length
    if (header->payload_len > 1024 * 1024) // 1MB max message size
    {
        lg2::error("Message payload too large: {SIZE} bytes", "SIZE",
                   header->payload_len);
        return;
    }

    if (verbose)
    {
        lg2::info("Successfully read {BYTES} bytes from client on fd {FD}",
                  "BYTES", len, "FD", fd);
    }

    // Process the message
    wrapper(buffer.data(), len);
}

void ClientConnection::wrapper(const uint8_t* msg, size_t len)
{
    handleRequest(msg, len).detach();
}

void ClientConnection::handleResponse(const uint8_t* msg, size_t msgSize)
{
    nsm_header_info hdrFields{};
    auto hdr = reinterpret_cast<const nsm_msg_hdr*>(msg);
    if (NSM_SUCCESS != unpack_nsm_header(hdr, &hdrFields))
    {
        lg2::error("Empty NSM response header");
        return;
    }

    const size_t nsmRespMinimumLen = sizeof(struct nsm_msg_hdr) +
                                     NSM_RESPONSE_MIN_LEN;

    if (NSM_RESPONSE == hdrFields.nsm_msg_type && msgSize >= nsmRespMinimumLen)
    {
        auto response = reinterpret_cast<const nsm_msg*>(hdr);
        if (verbose)
        {
            lg2::info("Sending response to client, length={LEN}", "LEN",
                      msgSize);
        }
        sendResponse(response, msgSize);
    }
    else
    {
        lg2::error("Invalid NSM message type or size");
    }
}

requester::Coroutine ClientConnection::handlePassthroughRequest(
    uint8_t eid, const uint8_t* payload, size_t payloadLen)
{
    std::vector<uint8_t> request(payload, payload + payloadLen);

    const nsm_msg* responseMsg = nullptr;
    size_t responseLen = 0;

    auto rc = co_await requester::SendRecvNsmMsg<
        requester::Handler<requester::Request>>(reqHandler, eid, request,
                                                &responseMsg, &responseLen);

    if (rc != NSM_SW_SUCCESS)
    {
        lg2::error("Failed to send/receive NSM message, rc={RC}", "RC", rc);
        co_return 1;
    }

    if (responseMsg)
    {
        handleResponse(reinterpret_cast<const uint8_t*>(responseMsg),
                       responseLen);
    }
    co_return 0;
}

requester::Coroutine
    ClientConnection::handleDiscoveryRequest(const mctp_endpoint_msg* mctpMsg)
{
    if (verbose)
    {
        lg2::info(
            "Received MCTP discovery message - Event: {EVENT}, EID: {EID}",
            "EVENT", static_cast<int>(mctpMsg->event), "EID",
            static_cast<int>(mctpMsg->eid));
    }

    int result = 0;
    switch (mctpMsg->event)
    {
        case MCTP_ENDPOINT_ADDED:
        case MCTP_ENDPOINT_UPDATED:
            result = endpointManager.registerEndpoint(
                mctpMsg->eid, mctpMsg->type, mctpMsg->protocol,
                std::vector<uint8_t>(mctpMsg->address,
                                     mctpMsg->address + mctpMsg->address_len));
            break;

        default:
            lg2::error("Unknown MCTP endpoint event: {EVENT}", "EVENT",
                       static_cast<int>(mctpMsg->event));
            co_return 1;
    }

    if (result < 0)
    {
        lg2::error(
            "Failed to process MCTP endpoint event {EVENT} for EID {EID}: {ERROR}",
            "EVENT", static_cast<int>(mctpMsg->event), "EID",
            static_cast<int>(mctpMsg->eid), "ERROR", strerror(-result));
        co_return 1;
    }

    co_return 0;
}

requester::Coroutine ClientConnection::handleRequest(const uint8_t* msg,
                                                     size_t len)
{
    if (verbose)
    {
        lg2::info("Received message from client, length={LEN}", "LEN", len);
    }

    auto apiMsg = reinterpret_cast<const gpuserver_api_msg*>(msg);

    switch (apiMsg->api_type)
    {
        case GPUSERVER_API_PASSTHROUGH_EID:
            co_return co_await handlePassthroughRequest(
                apiMsg->device.eid, apiMsg->payload, apiMsg->payload_len);

        case GPUSERVER_API_MCTP_DISCOVERY:
            co_return co_await handleDiscoveryRequest(
                reinterpret_cast<const mctp_endpoint_msg*>(apiMsg->payload));

        default:
            lg2::error("Unsupported API type: {TYPE}", "TYPE",
                       apiMsg->api_type);
            co_return 1;
    }
}

void ClientConnection::sendResponse(const nsm_msg* response, size_t responseLen)
{
    if (response == nullptr || responseLen == 0)
    {
        lg2::error("Empty response received");
        return;
    }

    const uint8_t* respData = reinterpret_cast<const uint8_t*>(response);
    ssize_t ret = send(fd, respData, responseLen, MSG_NOSIGNAL);
    if (ret < 0)
    {
        logError("Failed to send response to client", errno);
    }
}

void ClientConnection::logError(const char* message, int errnum)
{
    lg2::error("{MSG}: {ERROR}", "MSG", message, "ERROR", strerror(errnum));
}

} // namespace gpuserver
// NOLINTEND
