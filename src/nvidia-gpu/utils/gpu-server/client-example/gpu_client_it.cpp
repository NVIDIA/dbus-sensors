// NOLINTBEGIN
#include "gpuserver.h"

#include <errno.h>
#include <poll.h>
#include <string.h> // for strerror
#include <sys/epoll.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

class GpuClientIT
{
  public:
    struct TestCase {
        int index;
        std::vector<uint8_t> input;
        std::vector<uint8_t> expected;
        std::string description;
        std::vector<int> dontCarePositions;
        bool passed{false};
    };

    GpuClientIT(const std::string& socketPath, uint8_t eid) : eid(eid)
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

    ~GpuClientIT()
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

    // Convert hex string to bytes
    static std::vector<uint8_t> hexToBytes(const std::string& hex)
    {
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i < hex.length(); i += 2)
        {
            if (hex.substr(i, 2) == "XX")
            {
                bytes.push_back(0); // placeholder for don't care
            }
            else
            {
                bytes.push_back(static_cast<uint8_t>(
                    std::stoul(hex.substr(i, 2), nullptr, 16)));
            }
        }
        return bytes;
    }

    // Load test cases from CSV
    void loadTestCases(const std::string& csvPath)
    {
        std::ifstream file(csvPath);
        if (!file.is_open())
        {
            throw std::runtime_error("Failed to open CSV file: " + csvPath);
        }

        std::string line;
        // Skip header
        std::getline(file, line);

        while (std::getline(file, line))
        {
            std::stringstream ss(line);
            std::string index, input, expected, description;

            // Parse CSV line
            std::getline(ss, index, ',');
            std::getline(ss, input, ',');
            std::getline(ss, expected, ',');
            std::getline(ss, description);

            TestCase tc;
            tc.index = std::stoi(index);
            tc.input = hexToBytes(input);
            tc.expected = hexToBytes(expected);
            tc.description = description;

            // Find positions of "XX" in expected output
            size_t pos = 0;
            while ((pos = expected.find("XX", pos)) != std::string::npos)
            {
                tc.dontCarePositions.push_back(pos / 2);
                pos += 2;
            }

            testCases.push_back(tc);
        }
    }

    // Compare response with expected output
    bool compareResponse(const std::vector<uint8_t>& response,
                        const TestCase& testCase)
    {
        if (response.size() != testCase.expected.size())
        {
            std::cout << "Size mismatch. Expected: " << testCase.expected.size()
                      << ", Got: " << response.size() << std::endl;
            return false;
        }

        for (size_t i = 0; i < response.size(); i++)
        {
            // Skip comparison for don't care positions
            if (std::find(testCase.dontCarePositions.begin(),
                         testCase.dontCarePositions.end(),
                         i) != testCase.dontCarePositions.end())
            {
                continue;
            }

            if (response[i] != testCase.expected[i])
            {
                std::cout << "Mismatch at position " << i << ". Expected: "
                          << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<int>(testCase.expected[i])
                          << ", Got: " << static_cast<int>(response[i])
                          << std::dec << std::endl;
                return false;
            }
        }
        return true;
    }

    // Execute a single test case
    void executeTestCase(TestCase& testCase)
    {
        std::cout << "\nExecuting test case " << testCase.index << ": "
                  << testCase.description << std::endl;

        // Send message
        ssize_t sent = gpuserver_send_msg(ctx, GPUSERVER_API_PASSTHROUGH_EID,
                                         eid, testCase.input.data(),
                                         testCase.input.size());
        if (sent < 0)
        {
            std::cout << "❌ Failed: Error sending message: " << strerror(-sent)
                      << std::endl;
            testCase.passed = false;
            return;
        }

        // Wait for response with timeout
        struct epoll_event events[1];
        int nfds = epoll_wait(epollFd, events, 1, 5000); // 5 second timeout
        if (nfds <= 0)
        {
            std::cout << "❌ Failed: Timeout waiting for response" << std::endl;
            testCase.passed = false;
            return;
        }

        // Receive response
        std::vector<uint8_t> response(1024);
        ssize_t respLen =
            gpuserver_recv(ctx, response.data(), response.size());
        if (respLen <= 0)
        {
            std::cout << "❌ Failed: Error receiving response: "
                      << strerror(-respLen) << std::endl;
            testCase.passed = false;
            return;
        }

        // Resize response to actual length
        response.resize(respLen);

        // Print received response in hex
        std::cout << "Received response: ";
        for (uint8_t byte : response)
        {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(byte) << " ";
        }
        std::cout << std::dec << std::endl;

        // Compare with expected output
        testCase.passed = compareResponse(response, testCase);
        std::cout << (testCase.passed ? "✅ Passed" : "❌ Failed") << std::endl;
    }

    // Run all test cases
    void run(const std::string& csvPath)
    {
        loadTestCases(csvPath);
        
        for (auto& testCase : testCases)
        {
            executeTestCase(testCase);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        printResults();
    }

    // Print test results summary
    void printResults() const
    {
        int totalTests = testCases.size();
        int passedTests = 0;

        std::cout << "\n=== Test Results Summary ===\n" << std::endl;

        std::cout << "Passed Tests:" << std::endl;
        for (const auto& test : testCases)
        {
            if (test.passed)
            {
                passedTests++;
                std::cout << "  ✅ [" << test.index << "] " << test.description
                          << std::endl;
            }
        }

        std::cout << "\nFailed Tests:" << std::endl;
        for (const auto& test : testCases)
        {
            if (!test.passed)
            {
                std::cout << "  ❌ [" << test.index << "] " << test.description
                          << std::endl;
            }
        }

        double passPercentage =
            (totalTests > 0) ? (passedTests * 100.0 / totalTests) : 0;

        std::cout << "\nSummary:" << std::endl;
        std::cout << "Total Tests: " << totalTests << std::endl;
        std::cout << "Passed: " << passedTests << " (" << std::fixed
                  << std::setprecision(2) << passPercentage << "%)" << std::endl;
        std::cout << "Failed: " << (totalTests - passedTests) << " ("
                  << (100.0 - passPercentage) << "%)" << std::endl;
    }

  private:
    gpuserver_ctx* ctx{nullptr};
    int epollFd{-1};
    uint8_t eid;
    std::vector<TestCase> testCases;
};

void printUsage(const char* programName)
{
    std::cerr << "Usage: " << programName << " <EID> <csv_path>" << std::endl;
    std::cerr << "  EID: Value between 0x00 and 0xFF" << std::endl;
    std::cerr << "  csv_path: Path to the CSV file containing test cases"
              << std::endl;
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        printUsage(argv[0]);
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

    // Get socket path from environment or use default
    std::string socketPath = "/run/gpuserverd.sock";
    if (const char* path = std::getenv("GPUSERVER_SOCKET"))
    {
        socketPath = path;
    }

    try
    {
        GpuClientIT client(socketPath, eid);
        client.run(argv[2]);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
// NOLINTEND
