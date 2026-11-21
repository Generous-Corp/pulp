#pragma once

#include <cstdint>

#include <pulp/runtime/detail/durable_file_replacement.hpp>

namespace pulp::runtime::detail {

struct DurableFileReplacementTestAccess {
    /// Number of drive-cache flushes (F_FULLFSYNC) issued by commit() since
    /// process start; stays put for relaxed commits.
    static std::uint64_t full_sync_count() noexcept;
    static void fail_parent_directory_sync(DurableFileReplacement& replacement) noexcept;
};

} // namespace pulp::runtime::detail
