// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/storage_binding_policy.h"

using VideoCommon::StorageBindingPolicy;

TEST_CASE("Compute storage sizing preserves DLSS and native table contracts",
          "[video_core][storage_binding_policy]") {
    constexpr u32 MiB = 1024 * 1024;
    const auto direct = StorageBindingPolicy::Compute(true, false, false);
    CHECK(direct.Size(32 * MiB, 256, true) == 256);
    CHECK(direct.ReadsSizeWord(true));
    CHECK(direct.require_contiguous);
    for (u64 invalid : {u64{0}, u64{33 * MiB}, u64{1} << 32}) {
        CHECK(direct.Size(32 * MiB, invalid, true) == 8 * MiB);
    }
    const auto dlss = StorageBindingPolicy::Compute(true, true, false);
    CHECK(dlss.Size(32 * MiB, 0x10a, false) == 8 * MiB);
    CHECK_FALSE(dlss.ReadsSizeWord(false));
    CHECK(dlss.require_contiguous);
    const auto indirect = StorageBindingPolicy::Compute(true, false, true);
    CHECK(indirect.Size(32 * MiB, 256, false) == 32 * MiB);
    CHECK(indirect.Size(80 * MiB, 256, false) == 64 * MiB);
    CHECK(indirect.Size(32 * MiB, 256, true) == 8 * MiB);
    CHECK_FALSE(indirect.ReadsSizeWord(true));
    CHECK_FALSE(indirect.ReadsSizeWord(false));
    CHECK(indirect.require_contiguous);
    const auto legacy = StorageBindingPolicy::Compute(false, false, false);
    CHECK(legacy.Size(32 * MiB, 256, true) == 256);
    CHECK_FALSE(legacy.require_contiguous);
    for (bool table : {false, true}) {
        const auto graphics = StorageBindingPolicy::Graphics(true, table);
        CHECK(graphics.require_contiguous);
        CHECK(graphics.Size(32 * MiB, 256, true) == 8 * MiB);
        CHECK(graphics.Size(32 * MiB, 256, false) == (table ? 32 : 8) * MiB);
    }
}
