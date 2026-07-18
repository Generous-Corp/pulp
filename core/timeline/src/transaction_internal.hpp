#pragma once

#include <pulp/timeline/transaction.hpp>

namespace pulp::timeline::detail {

runtime::Result<ReducedTransaction, TransactionError>
reduce_transaction(const Project& project, const Transaction& transaction,
                   bool allow_tombstone_restore);

} // namespace pulp::timeline::detail
