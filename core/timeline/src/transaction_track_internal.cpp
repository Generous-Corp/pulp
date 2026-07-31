#include "transaction_track_internal.hpp"

#include "media_reference_validation.hpp"
#include "owned_identity_traversal.hpp"
#include "sequence_graph_validation.hpp"
#include "sequence_scene_internal.hpp"
#include "transaction_reduction_support.hpp"

#include <optional>
#include <utility>
#include <vector>

namespace pulp::timeline::detail {
namespace {

ItemLocation expected(ItemKind kind, const Project& project, ItemId sequence, ItemId track = {}) {
    return ItemLocation{kind,     immediate_parent_id(kind, project.id(), sequence, track, {}),
                        sequence, track,
                        {},       true};
}

// A track owns identities four levels deep, two of them parented by a lane
// rather than by the track. The set is read from the shared traversal so it
// cannot drift from the one the model and the sequence commands walk.
std::vector<OwnedIdentity> owned_identities(const Track& track, ItemId sequence) {
    std::vector<OwnedIdentity> result;
    visit_track_owned_identities(track, [&](const ModelOwnedIdentity& identity) {
        result.push_back({identity.id,
                          ItemLocation{identity.kind,
                                       immediate_parent_id(identity.kind, {}, sequence,
                                                           identity.track, identity.clip,
                                                           identity.lane),
                                       sequence, identity.track, identity.clip, true}});
    });
    return result;
}

runtime::Result<TrackCommandReduction, TransactionError>
finish(const Project& project, const Sequence& sequence, Command inverse, ItemId track_id,
       DirtyFlags flags, std::span<const IdentityMutation> identities,
       std::optional<std::uint64_t> next_item_id, const Transaction& transaction,
       CommandId command) {
    auto next = ProjectEditAccess::replace_sequence(project, sequence, identities, next_item_id);
    if (!next)
        return runtime::Err(model_failure(transaction, command, next.error()));
    return runtime::Ok(TrackCommandReduction{std::move(next).value(), std::move(inverse),
                                             {track_id, track_id, sequence.id(), flags}});
}

runtime::Result<TrackCommandReduction, TransactionError>
insert_track(const Project& project, const InsertTrack& insert, const Transaction& transaction,
             CommandId command, bool restore) {
    if (const auto code =
            target_error(project, insert.sequence_id,
                         expected(ItemKind::Sequence, project, insert.sequence_id)))
        return reject_reduction<TrackCommandReduction>(*code, transaction, command,
                                                       insert.sequence_id);
    if (insert.before_track_id)
        if (const auto code = target_error(project, *insert.before_track_id,
                                           expected(ItemKind::Track, project, insert.sequence_id,
                                                    *insert.before_track_id)))
            return reject_reduction<TrackCommandReduction>(
                *code, transaction, command, *insert.before_track_id, insert.sequence_id);
    if (const auto media_error = validate_track_media(project, insert.track))
        return runtime::Err(model_failure(transaction, command, *media_error));
    for (const auto& clip : insert.track.clips())
        if (const auto* reference = std::get_if<SequenceRef>(&clip.content()))
            if (const auto graph_error = validate_sequence_edge(
                    project.sequences(), insert.sequence_id, reference->sequence_id))
                return runtime::Err(model_failure(transaction, command, *graph_error));

    const auto identities = owned_identities(insert.track, insert.sequence_id);
    if (const auto duplicate = duplicate_owned_identity(identities))
        return reject_reduction<TrackCommandReduction>(ConflictCode::IdentityNotAvailable,
                                                       transaction, command, *duplicate);
    auto plan = plan_identity_insert(project, identities, restore, transaction, command);
    if (!plan)
        return runtime::Err(plan.error());
    auto next = project.find_sequence(insert.sequence_id)
                    ->insert_track(insert.track, insert.before_track_id);
    if (!next)
        return runtime::Err(model_failure(transaction, command, next.error()));
    return finish(project, next.value(), RemoveTrack{insert.sequence_id, insert.track.id()},
                  insert.track.id(), DirtyFlags::Structure | DirtyFlags::Added, plan->mutations,
                  plan->next_item_id, transaction, command);
}

runtime::Result<TrackCommandReduction, TransactionError>
remove_track(const Project& project, const RemoveTrack& remove, const Transaction& transaction,
             CommandId command) {
    if (const auto code =
            target_error(project, remove.track_id,
                         expected(ItemKind::Track, project, remove.sequence_id, remove.track_id)))
        return reject_reduction<TrackCommandReduction>(*code, transaction, command,
                                                       remove.track_id, remove.sequence_id);
    const auto* sequence = project.find_sequence(remove.sequence_id);
    if (!sequence)
        return reject_reduction<TrackCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                       command, remove.track_id);
    // erase_track refuses to strand a launcher slot that sources a clip on the
    // track, so a sequence whose launcher still points at it fails here rather
    // than leaving the document referencing a removed clip.
    auto erased = SequenceEditAccess::erase_track(*sequence, remove.track_id);
    if (!erased)
        return runtime::Err(model_failure(transaction, command, erased.error()));
    auto result = std::move(erased).value();
    const auto identities = owned_identities(result.removed, remove.sequence_id);
    const auto mutations = plan_identity_deactivate(identities);
    // `following` names the track that stood after the removed one, so the
    // inverse restores authored position exactly rather than appending.
    return finish(project, result.sequence,
                  InsertTrack{remove.sequence_id, result.removed, result.following},
                  remove.track_id, DirtyFlags::Structure | DirtyFlags::Removed, mutations,
                  std::nullopt, transaction, command);
}

} // namespace

bool is_track_command(const Command& command) noexcept {
    return std::holds_alternative<InsertTrack>(command) ||
           std::holds_alternative<RemoveTrack>(command);
}

runtime::Result<TrackCommandReduction, TransactionError>
reduce_track_command(const Project& project, const Command& command,
                     const Transaction& transaction, CommandId command_id,
                     bool allow_tombstone_restore) {
    if (const auto* value = std::get_if<InsertTrack>(&command))
        return insert_track(project, *value, transaction, command_id, allow_tombstone_restore);
    if (const auto* value = std::get_if<RemoveTrack>(&command))
        return remove_track(project, *value, transaction, command_id);
    return reject_reduction<TrackCommandReduction>(ConflictCode::ModelInvariant, transaction,
                                                   command_id);
}

} // namespace pulp::timeline::detail
