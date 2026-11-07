// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <unordered_map>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::Nx2ReleaseTrack {

// EDEN_NX2_ACQUIRE_QUEUED=1: semaphore releases the front has queued for the back are recorded
// (address space, address -> highest payload). A later AcquireGequal on that semaphore which
// such a release meets does not wait for the back: the back runs the release before anything
// queued after the acquire, and the front's own reads of back state drain on their own
// (macro DMA reads, list writers, maps). Every 64th such acquire still drains and checks the
// value in memory; a mismatch turns the semaphore off for good (sticky).
// Front thread only.
inline bool Enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("EDEN_NX2_ACQUIRE_QUEUED");
        const char* aggressive = std::getenv("EDEN_NX2_AGGRESSIVE");
        return (v && v[0] == '1') || (aggressive && aggressive[0] == '1');
    }();
    return enabled;
}

struct Entry {
    u32 value{};
    u32 uses{};
    bool sticky{};
};

struct Stats {
    u64 skipped{};
    u64 verified{};
    u64 failed{};
    u64 no_release{};
};

inline std::unordered_map<u64, Entry>& Map() {
    static std::unordered_map<u64, Entry> map;
    return map;
}

inline Stats& GetStats() {
    static Stats stats;
    return stats;
}

inline u64 Key(size_t address_space, u64 gpu_addr) {
    return (static_cast<u64>(address_space) << 50) ^ gpu_addr;
}

inline void NoteRelease(size_t address_space, u64 gpu_addr, u32 value) {
    if (!Enabled()) {
        return;
    }
    auto& map = Map();
    if (map.size() > 65536) {
        map.clear();
    }
    Entry& entry = map[Key(address_space, gpu_addr)];
    // Semaphores count up (NVN); keep the highest queued payload.
    if (static_cast<s32>(value - entry.value) > 0 || entry.uses == 0) {
        entry.value = value;
    }
    if (entry.uses == 0) {
        entry.uses = 1;
    }
}

enum class Decision { Drain, Skip, Verify };

/// Whether an AcquireGequal for `target` can skip the drain.
inline Decision Check(size_t address_space, u64 gpu_addr, u32 target) {
    if (!Enabled()) {
        return Decision::Drain;
    }
    auto& map = Map();
    const auto it = map.find(Key(address_space, gpu_addr));
    if (it == map.end() || it->second.sticky ||
        static_cast<s32>(it->second.value - target) < 0) {
        ++GetStats().no_release;
        return Decision::Drain;
    }
    if ((++it->second.uses & 63) == 0) {
        return Decision::Verify;
    }
    ++GetStats().skipped;
    return Decision::Skip;
}

/// After a Verify drain: the value now in memory.
inline void Verified(size_t address_space, u64 gpu_addr, u32 target, u32 current) {
    auto& stats = GetStats();
    ++stats.verified;
    if (static_cast<s32>(current - target) < 0) {
        ++stats.failed;
        if (auto it = Map().find(Key(address_space, gpu_addr)); it != Map().end()) {
            it->second.sticky = true;
        }
        if (stats.failed <= 16) {
            LOG_WARNING(HW_GPU,
                        "Queued-release acquire at {:#x}: {:#x} after the drain, wanted {:#x}; "
                        "that semaphore drains from now on",
                        gpu_addr, current, target);
        }
    }
    if ((stats.verified & 255) == 0) {
        LOG_INFO(HW_GPU,
                 "Queued-release acquires: {} skipped, {} verified ({} failed), {} without a "
                 "queued release",
                 stats.skipped, stats.verified, stats.failed, stats.no_release);
    }
}

} // namespace VideoCommon::Nx2ReleaseTrack
