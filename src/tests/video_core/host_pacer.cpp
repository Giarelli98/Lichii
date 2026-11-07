// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_present_pacer.h"
#include "video_core/renderer_vulkan/vk_host_pacer.h"

namespace {
using namespace std::chrono_literals;
using Pacer = Vulkan::HostPacer;
constexpr auto Start = Pacer::Clock::time_point{1s};
constexpr double Refresh = 8'333'333.0;

void Show(Pacer& pacer, u64 id, Pacer::Clock::time_point shown) {
    pacer.NoteSubmitted(id, shown - 1ms, false);
    pacer.NoteShown(id, shown, false);
}
}

TEST_CASE("Host pacing requires a known fixed refresh period", "[video_core][host_pacer]") {
    Pacer pacer;
    for (const auto period : {0.0, -1.0, 1e6, 100e6,
                              std::numeric_limits<double>::quiet_NaN()}) {
        pacer.Configure(true, period);
        REQUIRE_FALSE(pacer.Pacing());
        REQUIRE(pacer.Target(Start, 16'666'666) == Start);
    }
    pacer.Configure(true, std::nullopt);
    REQUIRE_FALSE(pacer.Pacing());
    pacer.Configure(false, Refresh);
    REQUIRE_FALSE(pacer.Pacing());
    pacer.Configure(true, Refresh);
    REQUIRE(pacer.Pacing());
    REQUIRE(pacer.Target(Start, 16'666'666) == Start);
}

TEST_CASE("Host pacing follows requested periods rather than arrival intervals",
          "[video_core][host_pacer]") {
    Pacer pacer;
    pacer.Configure(true, Refresh);
    Show(pacer, 1, Start);
    const auto now = Start + 1ms;
    REQUIRE(pacer.Target(now, 16'666'666) == Start + 14'666'666ns);
    REQUIRE(pacer.Target(now, 33'333'333) == Start + 31'333'332ns);
    REQUIRE(pacer.Target(now, 16'666'666) == Start + 14'666'666ns);
    REQUIRE(pacer.Target(now, 0) == now);
    pacer.NoteQueued(Start + 2s);
    REQUIRE(pacer.Target(now, 16'666'666) == Start + 14'666'666ns);
    REQUIRE(pacer.Target(Start + 50ms, 16'666'666) == Start + 50ms);
}

TEST_CASE("Host pacing abandons stale grids after disable loss or refresh changes",
          "[video_core][host_pacer]") {
    Pacer pacer;
    pacer.Configure(true, Refresh);
    Show(pacer, 1, Start);
    pacer.Configure(false, Refresh);
    REQUIRE(pacer.NsToNextRefresh(Start) == -1);
    pacer.Configure(true, Refresh);
    REQUIRE(pacer.Target(Start, 16'666'666) == Start);
    Show(pacer, 2, Start + 20ms);
    pacer.Configure(true, 16'666'666.0);
    REQUIRE(pacer.NsToNextRefresh(Start + 20ms) == -1);
    Show(pacer, 3, Start + 40ms);
    pacer.NoteSubmitted(4, Start + 41ms, false);
    REQUIRE(pacer.Outstanding() == 4);
    REQUIRE(pacer.Target(Start + 42ms, 33'333'333) == Start + 42ms);
    pacer.NoteShown(4, Start + 42ms, true);
    REQUIRE(pacer.Outstanding() == 0);
    REQUIRE(pacer.NsToNextRefresh(Start + 42ms) == -1);
    pacer.Configure(true, std::nullopt);
    REQUIRE_FALSE(pacer.Pacing());
}

TEST_CASE("Guest phase nudges never use inverted bounds or add sleep to late frames",
          "[core][host_pacer]") {
    using Guest = Service::Nx2PresentPacer;
    for (const auto slack : {-100ms, -1ms, 0ms, 1ms}) {
        REQUIRE(Guest::PhaseShift(10e6, slack) == 0ns);
        REQUIRE(Guest::PhaseShift(-10e6, slack) == 0ns);
    }
    REQUIRE(Guest::PhaseShift(10e6, 2ms) == 0ns);
    REQUIRE(Guest::PhaseShift(10e6, 2250us) == 250us);
    REQUIRE(Guest::PhaseShift(10e6, 3ms) == 500us);
    REQUIRE(Guest::PhaseShift(-10e6, 3ms) == -500us);
    Guest guest;
    guest.Advance(Start, 33ms);
    guest.Shift(-500us);
    REQUIRE(guest.Advance(Start + 1ms, 33ms).deadline == Start + 32500us);
}
