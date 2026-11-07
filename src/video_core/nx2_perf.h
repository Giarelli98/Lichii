// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>

namespace VideoCommon::Nx2Perf {

inline bool Verify() {
    static const bool verify = [] {
        const char* value = std::getenv("EDEN_NX2_PERF_VERIFY");
        return value != nullptr && value[0] == '1';
    }();
    return verify;
}

/// The frame period the game asks for (nx2:vi2 present word 12 at the speed limit), 0 while
/// unknown or unpaced. The host frame pacer (vk_host_pacer.h) never holds frames longer.
inline std::atomic<long long> g_guest_frame_interval_ns{0};

/// The display's measured refresh period while the host frame pacer runs, else 0. The game's
/// present pacing (nx2_gsv.h) times its frames to whole multiples of it (genlock).
inline std::atomic<long long> g_host_refresh_ns{0};

/// At 30 FPS and below the host frame pacer presents frames as soon as they are rendered and
/// publishes how long before the display's next refresh the last one was ready (ns), or -1
/// while it isn't pacing that way. The game's present pacing (nx2_gsv.h) moves its frames until
/// that is ~6 ms: each lands on a refresh with little wait (input lag) and evenly spaced.
inline std::atomic<long long> g_host_phase_ns{-1};

} // namespace VideoCommon::Nx2Perf
