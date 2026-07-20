#pragma once

#include <pulp/timeline/transaction.hpp>

#include <utility>

namespace pulp::timeline {

struct ProjectEditAccess {
    static runtime::Result<Project, ModelError>
    replace_sequence(const Project& project, Sequence sequence,
                     std::span<const IdentityMutation> identities = {},
                     std::optional<std::uint64_t> next_item_id = std::nullopt) {
        return project.replace_sequence(std::move(sequence), identities, next_item_id);
    }
    static Project replace_tempo_map(const Project& project, timebase::TempoMap tempo_map) {
        return project.replace_tempo_map(std::move(tempo_map));
    }
    static Project replace_meter_map(const Project& project, timebase::MeterMap meter_map) {
        return project.replace_meter_map(std::move(meter_map));
    }
};

} // namespace pulp::timeline

namespace pulp::timeline::detail {

runtime::Result<ReducedTransaction, TransactionError>
reduce_transaction(const Project& project, const Transaction& transaction,
                   bool allow_tombstone_restore);

} // namespace pulp::timeline::detail
