// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::Nx2UploadStats {

// With EDEN_NX2_GPU_PROFILE: where the back's "texture upload" time goes, by path, every 10 s,
// and each upload of 4 ms or more (the first 200).
enum class Path : u32 {
    Plain,       ///< CPU unswizzle into staging
    Chunked,     ///< Very large image, unswizzled in 32 MiB pieces
    Converted,   ///< CPU unswizzle and format conversion (e.g. ASTC on the CPU)
    Accelerated, ///< Raw copy, decoded by a compute pass (ASTC/BCn on the GPU)
    AsyncDecode, ///< Queued to the decode worker
    Layers,      ///< Only written layers
    Sparse,      ///< Sparse layers or tiles
    Other,       ///< Unbacked, MSAA, async unswizzle
    Deduplicated, ///< Skipped: same bytes as the last upload (EDEN_NX2_UPLOAD_DEDUP)
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Path::Count)> names{
    "plain", "chunked", "converted", "accelerated", "async decode", "layers", "sparse", "other",
    "deduplicated",
};

struct Totals {
    u64 count{};
    u64 bytes{};
    u64 us{};
};

inline void Note(Path path, u64 bytes, u64 us, u32 format, u32 width, u32 height, u32 levels,
                 u32 layers) {
    static std::array<Totals, static_cast<size_t>(Path::Count)> totals{};
    static auto window = std::chrono::steady_clock::now();
    static u32 slow_logged{};
    auto& t = totals[static_cast<size_t>(path)];
    ++t.count;
    t.bytes += bytes;
    t.us += us;
    if (us >= 4000 && slow_logged < 200) {
        ++slow_logged;
        LOG_INFO(HW_GPU, "Slow texture upload: {:.1f} ms, {} KiB, path {}, format {}, {}x{}, "
                         "{} levels, {} layers",
                 us / 1000.0, bytes >> 10, names[static_cast<size_t>(path)], format, width,
                 height, levels, layers);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds{10}) {
        return;
    }
    window = now;
    std::string line;
    for (size_t i = 0; i < totals.size(); ++i) {
        if (totals[i].count == 0) {
            continue;
        }
        line += fmt::format(" | {} {} in {:.1f} ms ({} KiB)", names[i], totals[i].count,
                            totals[i].us / 1000.0, totals[i].bytes >> 10);
        totals[i] = {};
    }
    auto& dedup = GetDedupStats();
    if (dedup.same != 0 || dedup.gpu_written_same != 0) {
        line += fmt::format(" || same content as last upload {} ({} KiB), skipped {} ({} KiB), "
                            "same but GPU-written since {}",
                            dedup.same, dedup.same_bytes >> 10, dedup.skipped,
                            dedup.skipped_bytes >> 10, dedup.gpu_written_same);
        dedup = {};
    }
    LOG_INFO(HW_GPU, "Texture uploads last 10 s:{}", line);
}

// EDEN_NX2_UPLOAD_DEDUP=1: an upload from guest memory whose bytes hash the same as the image's
// last upload, with no GPU write to the image since (modification tick unchanged), is skipped:
// the host copy already holds those bytes. Ocarina of Time re-sent ~360 MB/s of textures.
inline bool DedupEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("EDEN_NX2_UPLOAD_DEDUP");
        return v && v[0] == '1';
    }();
    return enabled;
}

/// Fast 64-bit content hash (4 multiply-xor lanes, 32 bytes a step). Never returns 0.
inline u64 Hash(const u8* data, size_t size) {
    constexpr u64 k0 = 0x9E3779B97F4A7C15ULL, k1 = 0xC2B2AE3D27D4EB4FULL,
                  k2 = 0x165667B19E3779F9ULL, k3 = 0xD6E8FEB86659FD93ULL;
    u64 a = k0 ^ size, b = k1, c = k2, d = k3;
    const auto mix = [](u64 h, u64 v, u64 k) {
        h ^= v * k;
        h = (h << 31) | (h >> 33);
        return h * 0xFF51AFD7ED558CCDULL;
    };
    size_t i = 0;
    for (; i + 32 <= size; i += 32) {
        u64 w[4];
        std::memcpy(w, data + i, 32);
        a = mix(a, w[0], k1);
        b = mix(b, w[1], k2);
        c = mix(c, w[2], k3);
        d = mix(d, w[3], k0);
    }
    for (; i + 8 <= size; i += 8) {
        u64 w;
        std::memcpy(&w, data + i, 8);
        a = mix(a, w, k1);
    }
    u64 tail = 0;
    std::memcpy(&tail, data + i, size - i);
    b = mix(b, tail, k2);
    u64 h = a ^ (b * 3) ^ (c * 5) ^ (d * 7);
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ULL;
    h ^= h >> 29;
    return h | 1;
}

struct DedupStats {
    u64 same{}, same_bytes{}, skipped{}, skipped_bytes{}, gpu_written_same{};
};
inline DedupStats& GetDedupStats() {
    static DedupStats stats;
    return stats;
}

class Timer {
public:
    explicit Timer(bool enabled_) : enabled{enabled_} {
        if (enabled) {
            start = std::chrono::steady_clock::now();
        }
    }
    ~Timer() {
        if (enabled) {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
            Note(path, bytes, static_cast<u64>(us), format, width, height, levels, layers);
        }
    }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    Path path{Path::Plain};
    u64 bytes{};
    u32 format{}, width{}, height{}, levels{}, layers{};

private:
    bool enabled;
    std::chrono::steady_clock::time_point start{};
};

} // namespace VideoCommon::Nx2UploadStats
