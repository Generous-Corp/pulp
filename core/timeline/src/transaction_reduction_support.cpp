#include "transaction_reduction_support.hpp"

#include <algorithm>
#include <limits>

namespace pulp::timeline::detail {
namespace {

bool same_owner(const ItemLocation& lhs, const ItemLocation& rhs) noexcept {
    return lhs.kind == rhs.kind && lhs.sequence_id == rhs.sequence_id &&
           lhs.track_id == rhs.track_id && lhs.clip_id == rhs.clip_id &&
           lhs.automation_lane_id == rhs.automation_lane_id;
}

} // namespace

TransactionError reduction_error(ConflictCode code, const Transaction& transaction,
                                 CommandId command, ItemId item, ItemId related,
                                 std::optional<ModelError> model) {
    return {code,
            transaction.id,
            command,
            item,
            related,
            transaction.expected_revision,
            {},
            std::move(model)};
}

TransactionError model_failure(const Transaction& transaction, CommandId command,
                               const ModelError& error) {
    return reduction_error(ConflictCode::ModelInvariant, transaction, command, error.item,
                           error.related_item, error);
}

std::optional<ConflictCode> target_error(const Project& project, ItemId item,
                                         const ItemLocation& expected) {
    const auto location = project.locate(item);
    if (!location)
        return ConflictCode::TargetMissing;
    if (!location->active)
        return ConflictCode::InactiveTarget;
    if (location->kind != expected.kind)
        return ConflictCode::WrongTargetKind;
    if (!same_owner(*location, expected))
        return ConflictCode::ParentMismatch;
    return std::nullopt;
}

std::optional<ItemId> duplicate_owned_identity(std::span<const OwnedIdentity> identities) {
    std::vector<ItemId> ordered;
    ordered.reserve(identities.size());
    for (const auto& identity : identities)
        ordered.push_back(identity.item);
    std::sort(ordered.begin(), ordered.end());
    const auto duplicate = std::adjacent_find(ordered.begin(), ordered.end());
    return duplicate == ordered.end() ? std::nullopt : std::optional<ItemId>(*duplicate);
}

runtime::Result<IdentityInsertPlan, TransactionError>
plan_identity_insert(const Project& project, std::span<const OwnedIdentity> identities,
                     bool allow_tombstone_restore, const Transaction& transaction,
                     CommandId command) {
    IdentityInsertPlan plan;
    plan.mutations.reserve(identities.size());
    plan.next_item_id = project.next_item_id();
    for (const auto& identity : identities) {
        auto wanted = identity.location;
        wanted.active = true;
        const auto existing = project.locate(identity.item);
        if (allow_tombstone_restore && existing) {
            if (existing->active || !same_owner(*existing, wanted))
                return reject_reduction<IdentityInsertPlan>(
                    ConflictCode::IdentityNotAvailable, transaction, command, identity.item);
            plan.mutations.push_back(
                {IdentityMutationKind::Reactivate, identity.item, wanted});
        } else {
            if (existing || identity.item.value < project.next_item_id())
                return reject_reduction<IdentityInsertPlan>(
                    ConflictCode::IdentityNotAvailable, transaction, command, identity.item);
            plan.mutations.push_back({IdentityMutationKind::Insert, identity.item, wanted});
            plan.next_item_id =
                std::max(plan.next_item_id,
                         identity.item.value == std::numeric_limits<std::uint64_t>::max() - 1
                             ? std::numeric_limits<std::uint64_t>::max()
                             : identity.item.value + 1);
        }
    }
    return runtime::Ok(std::move(plan));
}

std::vector<IdentityMutation>
plan_identity_deactivate(std::span<const OwnedIdentity> identities) {
    std::vector<IdentityMutation> mutations;
    mutations.reserve(identities.size());
    for (const auto& identity : identities) {
        auto location = identity.location;
        location.active = false;
        mutations.push_back({IdentityMutationKind::Deactivate, identity.item, location});
    }
    return mutations;
}

} // namespace pulp::timeline::detail
