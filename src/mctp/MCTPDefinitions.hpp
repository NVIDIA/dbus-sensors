/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

// Shared mctpd (au.com.codeconstruct.MCTP1) D-Bus name/path/interface
// constants, consolidated here so the MCTP reactor consumers do not each
// repeat their own copies.

inline constexpr const char* mctpdBusName = "au.com.codeconstruct.MCTP1";
inline constexpr const char* mctpdControlPath = "/au/com/codeconstruct/mctp1";
inline constexpr const char* mctpdEndpointPath =
    "/au/com/codeconstruct/mctp1/networks/1/endpoints/";
inline constexpr const char* mctpdNetworkPath =
    "/au/com/codeconstruct/mctp1/networks/1";
inline constexpr const char* mctpdNetworksSubtree =
    "/au/com/codeconstruct/mctp1/networks/";
inline constexpr const char* mctpdControlInterface =
    "au.com.codeconstruct.MCTP.BusOwner1";
inline constexpr const char* mctpdEndpointControlInterface =
    "au.com.codeconstruct.MCTP.Endpoint1";
inline constexpr const char* mctpdNetworkInterface =
    "au.com.codeconstruct.MCTP.Network1";
inline constexpr const char* mctpdBridgeInterface =
    "au.com.codeconstruct.MCTP.Bridge1";
inline constexpr const char* mctpdObjectManagerInterface =
    "org.freedesktop.DBus.ObjectManager";
inline constexpr const char* associationInterface =
    "xyz.openbmc_project.Association";
