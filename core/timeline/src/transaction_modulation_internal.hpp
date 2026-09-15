#pragma once

#include <pulp/timeline/transaction.hpp>

namespace pulp::timeline::detail {

struct ModulationCommandReduction {
    Project project;
    Command inverse;
    DirtyItem dirty;
};

bool is_modulation_command(const Command& command) noexcept;

runtime::Result<ModulationCommandReduction, TransactionError>
reduce_modulation_command(const Project& project, const Command& command,
                          const Transaction& transaction, CommandId command_id,
                          bool allow_tombstone_restore);

} // namespace pulp::timeline::detail
