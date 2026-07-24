#pragma once

#include <pulp/timeline/transaction.hpp>

namespace pulp::timeline::detail {

struct TrackStateCommandReduction {
    Project project;
    Command inverse;
    DirtyItem dirty;
};

bool is_track_state_command(const Command& command) noexcept;

runtime::Result<TrackStateCommandReduction, TransactionError>
reduce_track_state_command(const Project& project, const Command& command,
                           const Transaction& transaction, CommandId command_id);

} // namespace pulp::timeline::detail
