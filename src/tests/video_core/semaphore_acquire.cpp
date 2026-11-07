// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "tests/core/system_test_context.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/renderer_null/null_rasterizer.h"

namespace Tegra::Engines {

struct PullerTestAccess {
    static Puller::PendingAcquire& Suspend(Puller& puller) {
        puller.pending_acquire = Puller::PendingAcquire{
            .address = 0x10000,
            .value = 1,
            .operation = Nx2SemaphoreWaiters::AcquireEqual,
            .since = std::chrono::steady_clock::now(),
            .last_fences = std::chrono::steady_clock::now(),
            .reported = true,
        };
        return *puller.pending_acquire;
    }
};

} // namespace Tegra::Engines

TEST_CASE("Deferred acquire polls always reach the poll or time bound",
          "[video_core][semaphore_acquire]") {
    using namespace std::chrono_literals;
    using Acquire = Tegra::Engines::Puller::PendingAcquire;
    const auto start = std::chrono::steady_clock::time_point{10s};
    for (const bool reported : {false, true}) {
        Acquire acquire{};
        acquire.reported = reported;
        acquire.since = start - 3s;
        acquire.last_drain = start;
        for (u32 round = 0; round < 4; ++round) {
            for (u32 poll = 1; poll < 16; ++poll) {
                REQUIRE(acquire.CanDeferPoll(start + 999us));
            }
            REQUIRE_FALSE(acquire.CanDeferPoll(start + 999us));
            acquire.unmet_polls = 0;
        }
        REQUIRE_FALSE(acquire.CanDeferPoll(start + 1ms));
        acquire.unmet_polls = 0;
        REQUIRE_FALSE(acquire.CanDeferPoll(start + 2s));
    }
}

TEST_CASE("Acquire candidates never complete from a queued or unmatched release",
          "[video_core][semaphore_acquire]") {
    ScopedSystemTestContext context;
    Core::System system;
    Tegra::MemoryManager memory{system};
    Tegra::Control::ChannelState channel{0};
    Tegra::DmaPusher dma{system, memory, channel};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    dma.BindRasterizer(&rasterizer);
    auto& split = VideoCommon::GpuSplit::Get();
    auto& waiters = Tegra::Nx2SemaphoreWaiters::Get();
    auto& mode = VideoCore::Nx2Debug::g_sem_fast;
    const int old_mode = mode.load();
    const bool old_ampere = Tegra::g_nx2_ampere.exchange(true);
    const auto old_accuracy = Settings::values.gpu_accuracy.GetValue();
    const auto old_current_accuracy = Settings::values.current_gpu_accuracy;
    const auto old_dma = Settings::values.dma_accuracy.GetValue();
    const auto old_renderer = Settings::values.renderer_backend.GetValue();
    const auto old_fences = Settings::values.gpu_fence_behavior.GetValue();
    SCOPE_EXIT {
        split.Stop();
        waiters.Remove(&dma.puller);
        mode.store(old_mode);
        Tegra::g_nx2_ampere.store(old_ampere);
        Settings::values.gpu_accuracy.SetValue(old_accuracy);
        Settings::values.current_gpu_accuracy = old_current_accuracy;
        Settings::values.dma_accuracy.SetValue(old_dma);
        Settings::values.renderer_backend.SetValue(old_renderer);
        Settings::values.gpu_fence_behavior.SetValue(old_fences);
    };
    Settings::values.gpu_accuracy.SetValue(Settings::GpuAccuracy::Low);
    Settings::values.current_gpu_accuracy = Settings::GpuAccuracy::Low;
    Settings::values.dma_accuracy.SetValue(Settings::DmaAccuracy::Default);
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
    Settings::values.gpu_fence_behavior.SetValue(Settings::GpuFenceBehavior::Default);
    split.MaybeStart(system);
    REQUIRE(split.Active());

    auto& acquire = Tegra::Engines::PullerTestAccess::Suspend(dma.puller);
    dma.suspensions.store(1);
    waiters.Add(&dma.puller, 0x20000, 1, Tegra::Nx2SemaphoreWaiters::AcquireEqual);
    waiters.OnWrite(0x20004, 1);
    waiters.OnWrite(0x20000, 0);
    REQUIRE_FALSE(waiters.Latched(&dma.puller));

    mode.store(1);
    std::atomic<u32> executed{};
    split.Defer([&] { executed.fetch_add(1); });
    REQUIRE_FALSE(dma.puller.CanResumeAcquire(dma));
    REQUIRE(executed.load() == 1);

    mode.store(2);
    acquire.last_drain = std::chrono::steady_clock::now() + std::chrono::hours{1};
    for (u32 poll = 1; poll < 16; ++poll) {
        REQUIRE_FALSE(dma.puller.CanResumeAcquire(dma));
        REQUIRE(acquire.unmet_polls == poll);
    }
    REQUIRE_FALSE(dma.puller.CanResumeAcquire(dma));
    REQUIRE(acquire.unmet_polls == 0);
    REQUIRE(acquire.last_drain <= std::chrono::steady_clock::now());

    acquire.last_drain = std::chrono::steady_clock::now() + std::chrono::hours{1};
    REQUIRE_FALSE(dma.puller.CanResumeAcquire(dma));
    REQUIRE(acquire.unmet_polls == 1);
    mode.store(0);
    REQUIRE_FALSE(dma.puller.CanResumeAcquire(dma));
    REQUIRE(acquire.unmet_polls == 0);

    Settings::values.gpu_fence_behavior.SetValue(Settings::GpuFenceBehavior::Balanced);
    acquire.last_fences = std::chrono::steady_clock::now() - std::chrono::seconds{1};
    const auto before_retry = acquire.last_fences;
    REQUIRE_FALSE(dma.puller.CanResumeAcquire(dma));
    REQUIRE(acquire.last_fences > before_retry);
    Settings::values.gpu_fence_behavior.SetValue(Settings::GpuFenceBehavior::Default);

    waiters.OnWrite(0x20000, 1);
    waiters.OnWrite(0x20000, 2);
    REQUIRE(waiters.Latched(&dma.puller));
    mode.store(1);
    split.Defer([&] { executed.fetch_add(1); });
    REQUIRE(dma.puller.CanResumeAcquire(dma));
    REQUIRE(executed.load() == 1);
    REQUIRE(dma.puller.HasPendingAcquire());
    REQUIRE(dma.puller.ResumeAcquire(dma));
    REQUIRE_FALSE(dma.puller.HasPendingAcquire());
    REQUIRE(dma.suspensions.load() == 2);
    REQUIRE_FALSE(waiters.Latched(&dma.puller));
    split.Drain();
    REQUIRE(executed.load() == 2);
}
