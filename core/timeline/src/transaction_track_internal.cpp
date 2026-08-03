#include "transaction_track_internal.hpp"

#include "media_reference_validation.hpp"
#include "owned_identity_traversal.hpp"
#include "sequence_graph_validation.hpp"
#include "sequence_scene_internal.hpp"
#include "transaction_dispatch_internal.hpp"
#include "transaction_reduction_support.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>
#include <variant>
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

// A track's authored position, named by the track it stands before. The last
// track stands before nothing, which is the same empty value InsertTrack uses
// to mean "append", so one encoding serves placement, the optimistic gate, and
// the inverse.
std::optional<ItemId> authored_position(const Sequence& sequence, ItemId track_id) {
    const auto order = sequence.track_order();
    const auto placed = std::find(order.begin(), order.end(), track_id);
    if (placed == order.end() || std::next(placed) == order.end())
        return std::nullopt;
    return *std::next(placed);
}

runtime::Result<TrackCommandReduction, TransactionError>
move_track(const Project& project, const MoveTrack& move, const Transaction& transaction,
           CommandId command) {
    if (const auto code =
            target_error(project, move.track_id,
                         expected(ItemKind::Track, project, move.sequence_id, move.track_id)))
        return reject_reduction<TrackCommandReduction>(*code, transaction, command, move.track_id,
                                                       move.sequence_id);
    if (move.replacement_before_track_id)
        if (const auto code =
                target_error(project, *move.replacement_before_track_id,
                             expected(ItemKind::Track, project, move.sequence_id,
                                      *move.replacement_before_track_id)))
            return reject_reduction<TrackCommandReduction>(
                *code, transaction, command, *move.replacement_before_track_id, move.sequence_id);
    const auto* sequence = project.find_sequence(move.sequence_id);
    if (!sequence)
        return reject_reduction<TrackCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                       command, move.track_id);
    if (authored_position(*sequence, move.track_id) != move.expected_before_track_id)
        return reject_reduction<TrackCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                       transaction, command, move.track_id);

    // move_track permutes authored order alone, so no identity changes state and
    // the compiled program keeps its token. Composing this from erase and insert
    // would do neither, and would refuse outright for a track whose clip a
    // launcher slot sources.
    auto next = sequence->move_track(move.track_id, move.replacement_before_track_id);
    if (!next)
        return runtime::Err(model_failure(transaction, command, next.error()));
    return finish(project, next.value(),
                  MoveTrack{move.sequence_id, move.track_id, move.replacement_before_track_id,
                            move.expected_before_track_id},
                  move.track_id, DirtyFlags::Structure, {}, std::nullopt, transaction, command);
}

} // namespace

bool is_track_command(const Command& command) noexcept {
    return std::visit([]<typename T>(const T&) { return is_track_command_type<T>; }, command);
}

runtime::Result<TrackCommandReduction, TransactionError>
reduce_track_command(const Project& project, const Command& command,
                     const Transaction& transaction, CommandId command_id,
                     bool allow_tombstone_restore) {
    // The family predicate above this call and the arms below it are two
    // statements of the same list, and a chain of get_if proves nothing about
    // its own coverage. Visiting puts a claimed-but-unhandled command in front
    // of the compiler, which is where the outer dispatch already resolves it.
    return std::visit(
        [&]<typename T>(
            const T& value) -> runtime::Result<TrackCommandReduction, TransactionError> {
            if constexpr (std::is_same_v<T, InsertTrack>)
                return insert_track(project, value, transaction, command_id,
                                    allow_tombstone_restore);
            else if constexpr (std::is_same_v<T, RemoveTrack>)
                return remove_track(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, MoveTrack>)
                return move_track(project, value, transaction, command_id);
            else {
                static_assert(!is_track_command_type<T>,
                              "a command claimed by is_track_command_type in "
                              "transaction_dispatch_internal.hpp has no arm here; add "
                              "one, or drop it from the claim list");
                // Reached only by an alternative no family claims, which the
                // caller's predicate already excludes. Kept as a rejection rather
                // than std::unreachable(): this TU is -fno-exceptions, so being
                // wrong here would abort the process, not fail one transaction.
                return reject_reduction<TrackCommandReduction>(ConflictCode::ModelInvariant,
                                                               transaction, command_id);
            }
        },
        command);
}

} // namespace pulp::timeline::detail
