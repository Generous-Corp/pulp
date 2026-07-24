#pragma once

#include <pulp/timeline/transaction.hpp>

namespace pulp::timeline::detail {

struct TakeCommandReduction {
    Project project;
    Command inverse;
    DirtyItem dirty;
};

bool is_take_command(const Command& command) noexcept;

runtime::Result<TakeCommandReduction, TransactionError>
reduce_take_command(const Project& project, const Command& command, const Transaction& transaction,
                    CommandId command_id, bool allow_tombstone_restore);

} // namespace pulp::timeline::detail
