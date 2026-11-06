// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
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
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Path::Count)> names{
    "plain", "chunked", "converted", "accelerated", "async decode", "layers", "sparse", "other",
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
    LOG_INFO(HW_GPU, "Texture uploads last 10 s:{}", line);
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
