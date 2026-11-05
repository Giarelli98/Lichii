// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"

namespace VideoCommon {

struct StorageBindingPolicy {
    bool size_from_mapping;
    bool require_contiguous;
    bool table_pointer;

    static constexpr StorageBindingPolicy Graphics(bool native, bool table) {
        return {native || table, native || table, table};
    }

    static constexpr StorageBindingPolicy Compute(bool native, bool dlss, bool table) {
        return {dlss || (native && table), native || table, table};
    }

    constexpr bool ReadsSizeWord(bool written) const {
        return !size_from_mapping && !(table_pointer && !written);
    }

    constexpr u32 Size(u32 mapped, u64 next_qword, bool written) const {
        if (table_pointer && !written) {
            return (std::min)(mapped, u32{64 * 1024 * 1024});
        }
        const u32 packed_size = static_cast<u32>(next_qword);
        if (!size_from_mapping && (next_qword >> 32) == 0 && packed_size != 0 &&
            packed_size <= mapped) {
            return packed_size;
        }
        return (std::min)(mapped, u32{8 * 1024 * 1024});
    }
};

} // namespace VideoCommon
