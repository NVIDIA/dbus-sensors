/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

//NOLINTBEGIN
#include "utils.hpp"

#include <string.h>

#include <phosphor-logging/lg2.hpp>

#include <string>
#include <vector>

namespace utils
{

void printBuffer(bool isTx, const std::vector<uint8_t>& buffer, uint8_t tag,
                 eid_t eid)
{
    if (!buffer.empty())
    {
        constexpr size_t headerSize = sizeof("EID: 1d, TAG: 03, Tx: ") - 1;
        constexpr size_t hexWithSpaceSize = sizeof("89 ") - 1;
        std::string output(headerSize + buffer.size() * hexWithSpaceSize, '\0');
        sprintf(output.data(), "EID: %02x, TAG: %02x, %s: ", eid, tag,
                isTx ? "Tx" : "Rx");
        for (size_t i = 0; i < buffer.size(); i++)
        {
            sprintf(&output[headerSize + i * hexWithSpaceSize], "%02x ",
                    buffer[i]);
        }
        // Changing last trailing space to string null terminator
        output.back() = '\0';
        lg2::info("{OUTPUT}", "OUTPUT", output);
    }
}

void printBuffer(bool isTx, const uint8_t* ptr, size_t bufferLen, uint8_t tag,
                 eid_t eid)
{
    auto outBuffer = std::vector<uint8_t>(ptr, ptr + bufferLen);
    printBuffer(isTx, outBuffer, tag, eid);
}

} // namespace utils
//NOLINTEND
