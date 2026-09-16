// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lpu::power
{
inline constexpr size_t slotCount = 16;
enum class Mode : uint8_t
{
    unset = 0,
    maxQ = 1,
    maxP = 2,
};

enum class Failure
{
    unavailable,
    invalidArgument,
    writeFailure,
};

struct Error : std::runtime_error
{
    Error(Failure failure, const std::string& detail) :
        std::runtime_error(detail), failure(failure)
    {}
    Failure failure;
};

class Transport
{
  public:
    virtual ~Transport() = default;
    // Resolve all 16 endpoints for each transaction, never a best-effort
    // subset.
    virtual void refresh() = 0;
    virtual Mode read(size_t slot) = 0;
    virtual void write(size_t slot, Mode mode) = 0;
};

class Controller
{
  public:
    explicit Controller(Transport& transport) : transport(transport) {}

    // Empty means unset or non-uniform. Transport failure is an error, not
    // MaxP.
    std::string get()
    {
        transport.refresh();
        const auto modes = readAll();
        for (Mode mode : modes)
        {
            if (mode == Mode::unset || mode != modes.front())
            {
                return {};
            }
        }
        return modes.front() == Mode::maxQ ? "MaxQ" : "MaxP";
    }

    void set(std::string_view value)
    {
        if (value != "MaxQ" && value != "MaxP")
        {
            throw Error(Failure::invalidArgument, "Expected MaxQ or MaxP");
        }
        const Mode target = value == "MaxQ" ? Mode::maxQ : Mode::maxP;
        transport.refresh();
        const auto before = readAll(); // Preflight EVERY LPU before any write.
        std::array<bool, slotCount> attempted{};
        try
        {
            for (size_t slot = 0; slot < slotCount; ++slot)
            {
                if (before[slot] == target)
                {
                    continue;
                }
                // Even a failed write may have reached firmware.
                attempted[slot] = true;
                transport.write(slot, target);
                if (transport.read(slot) != target)
                {
                    throw Error(Failure::writeFailure,
                                "Power mode write/readback mismatch at slot " +
                                    std::to_string(slot + 1));
                }
            }
            for (Mode mode : readAll())
            {
                if (mode != target)
                {
                    throw Error(Failure::writeFailure,
                                "Tray power mode final verification failed");
                }
            }
        }
        catch (const std::exception& error)
        {
            // SMBus has no tray-wide atomic transaction. Restore known previous
            // selectors where possible, but NEVER turn partial failure into
            // 2xx.
            bool restored = true;
            for (size_t slot = 0; slot < slotCount; ++slot)
            {
                if (!attempted[slot])
                {
                    continue;
                }
                if (before[slot] == Mode::unset)
                {
                    restored = false; // Firmware does not accept writes of 0.
                    continue;
                }
                try
                {
                    transport.write(slot, before[slot]);
                    restored &= transport.read(slot) == before[slot];
                }
                catch (const std::exception&)
                {
                    restored = false;
                }
            }
            throw Error(Failure::writeFailure,
                        std::string(error.what()) +
                            (restored
                                 ? "; changed slots restored"
                                 : "; rollback incomplete; read actual modes"));
        }
    }

  private:
    std::array<Mode, slotCount> readAll()
    {
        std::array<Mode, slotCount> result{};
        for (size_t slot = 0; slot < slotCount; ++slot)
        {
            result[slot] = transport.read(slot);
            if (result[slot] != Mode::unset && result[slot] != Mode::maxQ &&
                result[slot] != Mode::maxP)
            {
                throw Error(Failure::unavailable,
                            "Invalid power mode at slot " +
                                std::to_string(slot + 1));
            }
        }
        return result;
    }

    Transport& transport;
};
} // namespace lpu::power
