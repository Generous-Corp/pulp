#pragma once

#include <pulp/runtime/detail/durable_file_replacement.hpp>

namespace pulp::runtime::detail {

struct DurableFileReplacementTestAccess {
    static void fail_parent_directory_sync(DurableFileReplacement& replacement) noexcept;
};

} // namespace pulp::runtime::detail
