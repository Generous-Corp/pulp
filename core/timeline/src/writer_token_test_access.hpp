#pragma once

#include <pulp/timeline/document_session.hpp>

namespace pulp::timeline::detail {

class WriterTokenTestAccess {
  public:
    static void set_next_ids(WriterToken& token, std::uint64_t transaction, std::uint64_t command,
                             std::uint64_t undo_group) {
        token.next_transaction_.store(transaction, std::memory_order_relaxed);
        token.next_command_.store(command, std::memory_order_relaxed);
        token.next_undo_group_.store(undo_group, std::memory_order_relaxed);
    }
};

} // namespace pulp::timeline::detail
