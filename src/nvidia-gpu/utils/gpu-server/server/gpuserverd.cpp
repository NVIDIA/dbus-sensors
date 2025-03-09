/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

//NOLINTBEGIN
#include "instance_id.hpp"
#include "mctp_endpoint_manager.hpp"
#include "socket_handler.hpp"
#include "socket_manager.hpp"
#include "manager.hpp"

#include <err.h>
#include <getopt.h>

#include <phosphor-logging/lg2.hpp>
#include <sdeventplus/event.hpp>

#include <iostream>

void optionUsage(void)
{
    std::cerr << "Usage: gpuserverd [options]\n";
    std::cerr << "Options:\n";
    std::cerr << " [--verbose] - would enable verbosity\n";
    std::cerr << " [--socket PATH] - Unix domain socket path\n";
}

int main(int argc, char** argv)
{
    bool verbose = false;
    std::string socketPath = "/run/gpuserverd.sock";

    // Parse command line options
    static struct option long_options[] = {
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {"socket", required_argument, 0, 's'},
        {0, 0, 0, 0}};

    int argflag;
    while ((argflag = getopt_long(argc, argv, "hvs:", long_options, nullptr)) >=
           0)
    {
        switch (argflag)
        {
            case 'h':
                optionUsage();
                exit(EXIT_SUCCESS);
            case 'v':
                verbose = true;
                break;
            case 's':
                socketPath = optarg;
                break;
            default:
                exit(EXIT_FAILURE);
        }
    }

    try
    {
        if (verbose)
        {
            lg2::info("Starting GPUSERVER Daemon");
            lg2::info("Socket path: {PATH}", "PATH", socketPath);
        }

        // Initialize event loop
        auto event = sdeventplus::Event::get_default();
        event.set_watchdog(false);

        if (verbose)
            lg2::info("Event loop initialized");

        // Initialize core components
        nsm::InstanceIdDb instanceIdDb;
        mctp_socket::Manager sockManager;

        // Initialize request handler
        requester::Handler<requester::Request> reqHandler(event, instanceIdDb,
                                                          sockManager);

#ifdef MCTP_IN_KERNEL
        // Initialize socket handler
        mctp_socket::InKernelHandler sockHandler(
            event, reqHandler, sockManager, verbose);
#else
        // Initialize socket handler
        mctp_socket::DaemonHandler sockHandler(event, reqHandler,
                                               sockManager, verbose);
#endif

        reqHandler.setSocketHandler(&sockHandler);

        mctp::EndpointManager endpointManager(sockHandler, verbose);

        // Initialize gpuserver manager
        gpuserver::Manager mgr(
            event, socketPath, reqHandler, endpointManager, verbose);

        if (verbose)
            lg2::info("GPUSERVER daemodaemon initialized, entering main loop");

        return event.loop();
    }
    catch (const std::exception& e)
    {
        lg2::error("GPUSERVER daemon failed: {ERROR}", "ERROR", e.what());
        return -1;
    }
}
//NOLINTEND
