#pragma once

#include <pulp/timeline/transaction.hpp>

namespace pulp::timeline::detail {

struct DeviceCommandReduction {
    Project project;
    Command inverse;
    DirtyItem dirty;
};

bool is_device_command(const Command& command) noexcept;

runtime::Result<DeviceCommandReduction, TransactionError>
reduce_device_command(const Project& project, const Command& command,
                      const Transaction& transaction, CommandId command_id,
                      bool allow_tombstone_restore);

} // namespace pulp::timeline::detail
