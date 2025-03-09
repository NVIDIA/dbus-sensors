/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

//NOLINTBEGIN
#pragma once

#include "base.h"
#include "types.hpp"

#include <phosphor-logging/lg2.hpp>
#include <unistd.h> // for close()

#include <string>
#include <vector>

namespace utils
{

constexpr bool Tx = true;
constexpr bool Rx = false;

/** @struct CustomFD
 *
 *  RAII wrapper for file descriptor.
 */
struct CustomFD
{
    CustomFD(const CustomFD&) = delete;
    CustomFD& operator=(const CustomFD&) = delete;
    CustomFD(CustomFD&&) = delete;
    CustomFD& operator=(CustomFD&&) = delete;

    CustomFD(int fd) : fd(fd) {}

    ~CustomFD()
    {
        if (fd >= 0)
        {
            close(fd);
        }
    }

    int operator()() const
    {
        return fd;
    }

    operator int() const
    {
        return fd;
    }

  private:
    int fd = -1;
};

/** @brief Print the buffer
 *
 *  @param[in] isTx - True if the buffer is an outgoing NSM message, false if
                      the buffer is an incoming NSM message
 *  @param[in] buffer - Buffer to print
 *  @param[in] tag - Tag to identify the message
 *  @param[in] eid - EID of the message
 *
 *  @return - None
 */
void printBuffer(bool isTx, const std::vector<uint8_t>& buffer, uint8_t tag,
                 eid_t eid);

/** @brief Print the buffer
 *
 *  @param[in] isTx - True if the buffer is an outgoing NSM message, false if
                      the buffer is an incoming NSM message
 *  @param[in] buffer - NSM message buffer to log
 *  @param[in] bufferLen - NSM message buffer length
 *  @param[in] tag - Tag to identify the message
 *  @param[in] eid - EID of the message
 *
 *  @return - None
 */
void printBuffer(bool isTx, const uint8_t* buffer, size_t bufferLen,
                 uint8_t tag, eid_t eid);

} // namespace utils
//NOLINTEND
