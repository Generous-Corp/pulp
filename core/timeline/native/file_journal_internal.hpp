#pragma once

#include <cstddef>

namespace pulp::timeline::detail {

using FileJournalWriteHook = void (*)(std::size_t bytes_written) noexcept;

class FileJournalTestAccess {
  public:
    static void set_write_hook(std::size_t maximum_chunk, FileJournalWriteHook hook) noexcept;
    static void clear_write_hook() noexcept;
};

} // namespace pulp::timeline::detail
