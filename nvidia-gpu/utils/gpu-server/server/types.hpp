/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include <string>
#include <vector>

using eid_t = uint8_t;
using uuid_t = std::string;
using Request = std::vector<uint8_t>;
using Response = std::vector<uint8_t>;
using Command = uint8_t;
