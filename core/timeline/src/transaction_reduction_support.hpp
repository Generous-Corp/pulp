#pragma once

#include <pulp/timeline/transaction.hpp>

#include <optional>
#include <span>
#include <utility>
#include <vector>

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

struct OwnedIdentity {
    ItemId item;
    ItemLocation location;
};

struct IdentityInsertPlan {
    std::vector<IdentityMutation> mutations;
    std::uint64_t next_item_id = 0;
};

TransactionError reduction_error(ConflictCode code, const Transaction& transaction,
                                 CommandId command = {}, ItemId item = {}, ItemId related = {},
                                 std::optional<ModelError> model = std::nullopt);

template <typename T>
runtime::Result<T, TransactionError>
reject_reduction(ConflictCode code, const Transaction& transaction, CommandId command = {},
                 ItemId item = {}, ItemId related = {},
                 std::optional<ModelError> model = std::nullopt) {
    return runtime::Err(
        reduction_error(code, transaction, command, item, related, std::move(model)));
}

TransactionError model_failure(const Transaction& transaction, CommandId command,
                               const ModelError& error);

std::optional<ConflictCode> target_error(const Project& project, ItemId item,
                                         const ItemLocation& expected);

std::optional<ItemId> duplicate_owned_identity(std::span<const OwnedIdentity> identities);

runtime::Result<IdentityInsertPlan, TransactionError>
plan_identity_insert(const Project& project, std::span<const OwnedIdentity> identities,
                     bool allow_tombstone_restore, const Transaction& transaction,
                     CommandId command);

std::vector<IdentityMutation>
plan_identity_deactivate(std::span<const OwnedIdentity> identities);

} // namespace pulp::timeline::detail
