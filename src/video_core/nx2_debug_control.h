// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

/// Requests from the debug server (EDEN_NX2_DEBUG_SERVER, docs/nx2/debug-mcp.md) to the thread
/// that owns the rasterizer. The server posts them from its own thread; RasterizerVulkan takes
/// them at the end of each presented frame, so every cache they touch is used from its owner.
/// Without the server nothing is posted and the per-frame cost is one relaxed atomic load.
namespace VideoCore::Nx2Debug {

[[nodiscard]] inline int EnvSwitch(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? 0 : std::atoi(value);
}
/// The switch's value, read from the environment on first use (-1: not read yet). Static
/// initialization ran before the frontend applied nx2_switches.txt, so those settings were lost.
[[nodiscard]] inline int SwitchValue(std::atomic<int>& value, const char* name,
                                    int fallback = 0) {
    int current = value.load(std::memory_order_relaxed);
    if (current < 0) {
        int unset = -1;
        const int initial = std::getenv(name) != nullptr ? EnvSwitch(name) : fallback;
        value.compare_exchange_strong(unset, initial, std::memory_order_relaxed);
        current = value.load(std::memory_order_relaxed);
    }
    return current;
}
/// EDEN_NX2_HOST_PACE: the host frame pacer (vk_host_pacer.h). 1 = on, 0 = only
/// measure how long each frame was shown.
inline std::atomic<int> g_host_pace{-1};
inline std::atomic<int> g_host_fixed_refresh{-1};
inline std::atomic<int> g_swap_interval{-1};
inline std::atomic<int> g_fence_flush_us{-1};
/// EDEN_NX2_SEM_FAST (puller.cpp, docs/nx2/acquire-candidate.md): 0 drain every pending acquire
/// poll, 1 resume on a completed-release latch, 2 also defer unmet polls within 16 polls / 1 ms.
inline std::atomic<int> g_sem_fast{-1};

/// The live switch called `name`, or null.
[[nodiscard]] inline std::atomic<int>* FindSwitch(std::string_view name) {
    if (name == "EDEN_NX2_HOST_PACE") {
        return &g_host_pace;
    }
    if (name == "EDEN_NX2_HOST_PACE_FIXED_REFRESH") {
        return &g_host_fixed_refresh;
    }
    if (name == "EDEN_NX2_SWAP_INTERVAL") {
        return &g_swap_interval;
    }
    if (name == "EDEN_NX2_SEM_FAST") {
        return &g_sem_fast;
    }
    if (name == "EDEN_NX2_FENCE_FLUSH_US") {
        return &g_fence_flush_us;
    }
    return nullptr;
}

struct CaptureRequest {
    u64 id;
    std::filesystem::path directory;
    bool sampled;
    /// The uniform and storage buffers of every draw and dispatch, and dispatch group counts.
    bool buffers;
};

struct CaptureResult {
    std::string error;
};

struct GpuReadRequest {
    u64 id;
    u64 gpu_address;
    u64 size;
};

struct GpuReadResult {
    std::vector<u8> bytes;
    /// Tegra::MemoryManager::GetID of the address space that maps the whole range.
    u64 address_space;
    std::string error;
};

struct RejectChange {
    u64 generation;
    /// Whether a hash left the list, so programs rejected before must be lowered again.
    bool unrejected;
};

/// Opens a debug server session (clears requests and rejections) or ends it (wakes waiters).
void BeginSession();
void EndSession();

/// Rasterizer thread: whether a capture or a reject change is waiting.
[[nodiscard]] bool HasRequests() noexcept;

/// Server thread: queues a frame capture; the rasterizer takes it at the next frame end.
void RequestCapture(CaptureRequest request);
/// Server thread: withdraws a capture that was not taken yet (it timed out).
void CancelCapture(u64 id);
/// Server thread: waits for the capture's manifest to be complete. Empty on timeout or when the
/// session ends.
[[nodiscard]] std::optional<CaptureResult> WaitCapture(u64 id, std::chrono::milliseconds timeout);
/// Rasterizer thread.
[[nodiscard]] std::optional<CaptureRequest> TakeCapture();
/// Frame dump writer thread, once the manifest summary is written (or the capture failed).
void FinishCapture(u64 id, CaptureResult result);

/// Server thread: queues a read of GPU virtual memory; the rasterizer takes it at the next frame
/// end, where the GPU's own reads of its address spaces run.
void RequestGpuRead(GpuReadRequest request);
/// Server thread: withdraws a read that was not taken yet (it timed out).
void CancelGpuRead(u64 id);
/// Server thread: empty on timeout or when the session ends.
[[nodiscard]] std::optional<GpuReadResult> WaitGpuRead(u64 id, std::chrono::milliseconds timeout);
/// Rasterizer thread.
[[nodiscard]] std::optional<GpuReadRequest> TakeGpuRead();
/// Rasterizer thread.
void FinishGpuRead(u64 id, GpuReadResult result);

/// Server thread: adds or removes native program keys or shader hashes (as EDEN_NX2_REJECT_HASH
/// takes them). Returns the generation to wait for.
u64 SetRejected(std::span<const u64> hashes, bool rejected);
[[nodiscard]] std::vector<u64> RejectedHashes();
/// Lowering: whether the debug server rejects this program. Cheap while the list is empty.
[[nodiscard]] bool IsRejected(u64 hash);
[[nodiscard]] bool AnyRejected() noexcept;
/// Rasterizer thread: the pending reject change, once.
[[nodiscard]] std::optional<RejectChange> TakeRejectChange();
/// Rasterizer thread: the cached programs of this generation were dropped.
void NoteRejectApplied(u64 generation);
/// Server thread: waits until the rasterizer applied the generation.
[[nodiscard]] bool WaitRejectApplied(u64 generation, std::chrono::milliseconds timeout);

} // namespace VideoCore::Nx2Debug
