// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdlib>
#include <thread>
#include <algorithm>
#include <chrono>
#include <thread>

#include "core/file_sys/errors.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_storage.h"

namespace Service::FileSystem {

IStorage::IStorage(Core::System& system_, FileSys::VirtualFile backend_)
    : ServiceFramework{system_, "IStorage"}, backend(std::move(backend_)) {
    static const FunctionInfo functions[] = {
        {0, D<&IStorage::Read>, "Read"},
        {1, nullptr, "Write"},
        {2, nullptr, "Flush"},
        {3, nullptr, "SetSize"},
        {4, D<&IStorage::GetSize>, "GetSize"},
        {5, nullptr, "OperateRange"},
    };
    RegisterHandlers(functions);
}

Result IStorage::Read(
    OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_bytes,
    s64 offset, s64 length) {
    LOG_DEBUG(Service_FS, "called, offset={:#x}, length={}", offset, length);

    R_UNLESS(length >= 0, FileSys::ResultInvalidSize);
    R_UNLESS(offset >= 0, FileSys::ResultInvalidOffset);

    // EDEN_NX2_FAST_STORAGE=1: no read-speed throttle (40 MB/s, up to 1.6 ms a read).
    static const bool fast_storage = [] {
        const char* v = std::getenv("EDEN_NX2_FAST_STORAGE");
        return v && v[0] == '1';
    }();
    const auto now = std::chrono::steady_clock::now();
    u64 slept_us = 0;
    if (!fast_storage) {
        if (now < next_read_tick) {
            std::this_thread::sleep_for(next_read_tick - now);
            slept_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::steady_clock::now() - now)
                                            .count());
        }
        next_read_tick = std::chrono::steady_clock::now() +
                         std::chrono::nanoseconds{std::min(length, MaxChargedReadSize) *
                                                  1'000'000'000 / ReadBytesPerSecond};
    }

    // Read the data from the Storage backend
    const auto read_start = std::chrono::steady_clock::now();
    backend->Read(out_bytes.data(), length, offset);
    const u64 read_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now() - read_start)
                                             .count());

    // Every 10 s with reads: how long the game waited on storage (throttle and backend).
    static std::atomic<u64> reads{}, bytes{}, sleep_total{}, read_total{};
    static std::atomic<s64> window_start{};
    reads.fetch_add(1);
    bytes.fetch_add(static_cast<u64>(length));
    sleep_total.fetch_add(slept_us);
    read_total.fetch_add(read_us);
    const s64 now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    s64 start_ms = window_start.load();
    if (start_ms == 0) {
        window_start.compare_exchange_strong(start_ms, now_ms);
    } else if (now_ms - start_ms >= 10'000 && window_start.compare_exchange_strong(start_ms, now_ms)) {
        LOG_INFO(Service_FS,
                 "Storage reads last {:.1f} s: {} reads, {} KiB; throttle {:.1f} ms, backend {:.1f} ms",
                 (now_ms - start_ms) / 1000.0, reads.exchange(0), bytes.exchange(0) >> 10,
                 sleep_total.exchange(0) / 1000.0, read_total.exchange(0) / 1000.0);
    }

    R_SUCCEED();
}

Result IStorage::GetSize(Out<u64> out_size) {
    *out_size = backend->GetSize();

    LOG_DEBUG(Service_FS, "called, size={}", *out_size);

    R_SUCCEED();
}

} // namespace Service::FileSystem
