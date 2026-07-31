#include <pulp/timeline/transaction.hpp>

#include "media_reference_validation.hpp"
#include "owned_identity_traversal.hpp"
#include "sequence_graph_validation.hpp"
#include "transaction_automation_internal.hpp"
#include "transaction_internal.hpp"
#include "transaction_marker_internal.hpp"
#include "transaction_scene_internal.hpp"
#include "transaction_note_internal.hpp"
#include "transaction_reduction_support.hpp"
#include "transaction_sequence_internal.hpp"
#include "transaction_take_internal.hpp"
#include "transaction_track_internal.hpp"
#include "transaction_track_state_internal.hpp"

#include <algorithm>
#include <tuple>

namespace pulp::timeline {

namespace {

// An active target identity with parent_id computed the one canonical way.
// detail::target_error compares a located identity against this expectation.
ItemLocation expected_location(ItemKind kind, const Project& project, ItemId sequence,
                               ItemId track = {}, ItemId clip = {}) {
    return ItemLocation{kind,     immediate_parent_id(kind, project.id(), sequence, track, clip),
                        sequence, track,
                        clip,     true};
}

std::vector<detail::OwnedIdentity> owned_identities(const Clip& clip, ItemId sequence,
                                                    ItemId track) {
    std::vector<detail::OwnedIdentity> result;
    detail::visit_clip_owned_identities(
        clip, track, [&](const detail::ModelOwnedIdentity& identity) {
            result.push_back(
                {identity.id,
                 ItemLocation{identity.kind,
                              immediate_parent_id(identity.kind, {}, sequence, identity.track,
                                                  identity.clip, identity.lane),
                              sequence, identity.track, identity.clip, true}});
        });
    return result;
}

} // namespace

DirtySet::DirtySet(std::vector<DirtyItem> items, std::vector<DirtyContext> contexts)
    : items_(std::move(items)), contexts_(std::move(contexts)) {
    std::sort(contexts_.begin(), contexts_.end());
    contexts_.erase(std::unique(contexts_.begin(), contexts_.end()), contexts_.end());
    std::sort(items_.begin(), items_.end(), [](const DirtyItem& lhs, const DirtyItem& rhs) {
        return std::tuple(lhs.owner_sequence, lhs.owner_track, lhs.item) <
               std::tuple(rhs.owner_sequence, rhs.owner_track, rhs.item);
    });
    std::vector<DirtyItem> canonical;
    canonical.reserve(items_.size());
    for (const auto& item : items_) {
        if (!canonical.empty() && canonical.back().item == item.item &&
            canonical.back().owner_track == item.owner_track &&
            canonical.back().owner_sequence == item.owner_sequence) {
            canonical.back().flags = canonical.back().flags | item.flags;
        } else {
            canonical.push_back(item);
        }
    }
    items_ = std::move(canonical);
}

std::size_t DirtySet::retained_size() const noexcept {
    return sizeof(DirtySet) + items_.size() * sizeof(DirtyItem) +
           contexts_.size() * sizeof(DirtyContext);
}

runtime::Result<ReducedTransaction, TransactionError>
detail::reduce_transaction(const Project& original, const Transaction& transaction,
                           bool allow_tombstone_restore) {
    if (!transaction.id.valid())
        return detail::reject_reduction<ReducedTransaction>(ConflictCode::InvalidIdentifier,
                                                            transaction);
    if (transaction.commands.empty())
        return detail::reject_reduction<ReducedTransaction>(ConflictCode::EmptyTransaction,
                                                            transaction);
    std::vector<CommandId> ids;
    ids.reserve(transaction.commands.size());
    for (const auto& envelope : transaction.commands) {
        if (!envelope.id.valid() || envelope.id.writer != transaction.id.writer)
            return detail::reject_reduction<ReducedTransaction>(ConflictCode::InvalidIdentifier,
                                                                transaction, envelope.id);
        ids.push_back(envelope.id);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        return detail::reject_reduction<ReducedTransaction>(
            ConflictCode::CommandIdCollision, transaction,
            *std::adjacent_find(ids.begin(), ids.end()));

    Project project = original;
    std::vector<DirtyItem> dirty;
    std::vector<DirtyContext> dirty_contexts;
    std::vector<Command> inverses;
    inverses.reserve(transaction.commands.size());

    for (const auto& envelope : transaction.commands) {
        auto fail_target = [&](ConflictCode code, ItemId item, ItemId related = {}) {
            return detail::reject_reduction<ReducedTransaction>(code, transaction, envelope.id,
                                                                item, related);
        };
        if (const auto* insert = std::get_if<InsertClip>(&envelope.command)) {
            if (const auto code = detail::target_error(
                    project, insert->sequence_id,
                    expected_location(ItemKind::Sequence, project, insert->sequence_id)))
                return fail_target(*code, insert->sequence_id);
            if (const auto code =
                    detail::target_error(project, insert->track_id,
                                         expected_location(ItemKind::Track, project,
                                                           insert->sequence_id, insert->track_id)))
                return fail_target(*code, insert->track_id, insert->sequence_id);
            if (const auto media_error = detail::validate_clip_media(project, insert->clip))
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(detail::model_failure(transaction, envelope.id, *media_error)));
            if (const auto* reference = std::get_if<SequenceRef>(&insert->clip.content()))
                if (const auto graph_error = validate_sequence_edge(
                        project.sequences(), insert->sequence_id, reference->sequence_id))
                    return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                        detail::model_failure(transaction, envelope.id, *graph_error)));
            const auto identities =
                owned_identities(insert->clip, insert->sequence_id, insert->track_id);
            auto identity_plan = detail::plan_identity_insert(
                project, identities, allow_tombstone_restore, transaction, envelope.id);
            if (!identity_plan)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(identity_plan.error()));
            const auto* sequence = project.find_sequence(insert->sequence_id);
            const auto* track = sequence->find_track(insert->track_id);
            auto next_track = track->insert_clip(insert->clip);
            if (!next_track)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, next_track.error())));
            auto next_sequence = sequence->replace_track(std::move(next_track).value());
            if (!next_sequence)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, next_sequence.error())));
            auto next_project = ProjectEditAccess::replace_sequence(
                project, std::move(next_sequence).value(), identity_plan->mutations,
                identity_plan->next_item_id);
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, next_project.error())));
            project = std::move(next_project).value();
            inverses.emplace_back(
                RemoveClip{insert->sequence_id, insert->track_id, insert->clip.id()});
            dirty.push_back({insert->clip.id(), insert->track_id, insert->sequence_id,
                             DirtyFlags::Structure | DirtyFlags::Added});
        } else if (const auto* remove = std::get_if<RemoveClip>(&envelope.command)) {
            if (const auto code = detail::target_error(
                    project, remove->clip_id,
                    expected_location(ItemKind::Clip, project, remove->sequence_id,
                                      remove->track_id, remove->clip_id)))
                return fail_target(*code, remove->clip_id, remove->track_id);
            const auto* sequence = project.find_sequence(remove->sequence_id);
            const auto* track = sequence ? sequence->find_track(remove->track_id) : nullptr;
            const auto* clip = track ? track->find_clip(remove->clip_id) : nullptr;
            if (!clip)
                return fail_target(ConflictCode::TargetMissing, remove->clip_id);
            const Clip removed = *clip;
            const auto identities =
                owned_identities(removed, remove->sequence_id, remove->track_id);
            const auto identity_changes = detail::plan_identity_deactivate(identities);
            auto next_track = track->erase_clip(remove->clip_id);
            if (!next_track)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, next_track.error())));
            auto next_sequence = sequence->replace_track(std::move(next_track).value());
            if (!next_sequence)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, next_sequence.error())));
            auto next_project = ProjectEditAccess::replace_sequence(
                project, std::move(next_sequence).value(), identity_changes);
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, next_project.error())));
            project = std::move(next_project).value();
            inverses.emplace_back(InsertClip{remove->sequence_id, remove->track_id, removed});
            dirty.push_back({remove->clip_id, remove->track_id, remove->sequence_id,
                             DirtyFlags::Structure | DirtyFlags::Removed});
        } else if (detail::is_automation_command(envelope.command)) {
            auto reduced = detail::reduce_automation_command(project, envelope.command, transaction,
                                                             envelope.id, allow_tombstone_restore);
            if (!reduced)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(reduced.error()));
            project = std::move(reduced->project);
            inverses.push_back(std::move(reduced->inverse));
            dirty.push_back(reduced->dirty);
        } else if (detail::is_take_command(envelope.command)) {
            auto reduced = detail::reduce_take_command(project, envelope.command, transaction,
                                                       envelope.id, allow_tombstone_restore);
            if (!reduced)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(reduced.error()));
            project = std::move(reduced->project);
            inverses.push_back(std::move(reduced->inverse));
            dirty.push_back(reduced->dirty);
        } else if (detail::is_marker_command(envelope.command)) {
            auto reduced = detail::reduce_marker_command(project, envelope.command, transaction,
                                                         envelope.id, allow_tombstone_restore);
            if (!reduced)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(reduced.error()));
            project = std::move(reduced->project);
            inverses.push_back(std::move(reduced->inverse));
            dirty.push_back(reduced->dirty);
        } else if (detail::is_scene_command(envelope.command)) {
            auto reduced = detail::reduce_scene_command(project, envelope.command, transaction,
                                                        envelope.id, allow_tombstone_restore);
            if (!reduced)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(reduced.error()));
            project = std::move(reduced->project);
            inverses.push_back(std::move(reduced->inverse));
            dirty.push_back(reduced->dirty);
        } else if (detail::is_track_command(envelope.command)) {
            auto reduced = detail::reduce_track_command(project, envelope.command, transaction,
                                                        envelope.id, allow_tombstone_restore);
            if (!reduced)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(reduced.error()));
            project = std::move(reduced->project);
            inverses.push_back(std::move(reduced->inverse));
            dirty.push_back(reduced->dirty);
        } else if (detail::is_track_state_command(envelope.command)) {
            auto reduced = detail::reduce_track_state_command(project, envelope.command,
                                                              transaction, envelope.id);
            if (!reduced)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(reduced.error()));
            project = std::move(reduced->project);
            inverses.push_back(std::move(reduced->inverse));
            dirty.push_back(reduced->dirty);
        } else if (detail::is_sequence_command(envelope.command)) {
            auto reduced = detail::reduce_sequence_command(project, envelope.command, transaction,
                                                           envelope.id, allow_tombstone_restore);
            if (!reduced)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(reduced.error()));
            project = std::move(reduced->project);
            inverses.push_back(std::move(reduced->inverse));
            dirty.push_back(reduced->dirty);
        } else if (const auto* move = std::get_if<MoveClip>(&envelope.command)) {
            if (const auto code = detail::target_error(
                    project, move->clip_id,
                    expected_location(ItemKind::Clip, project, move->sequence_id, move->track_id,
                                      move->clip_id)))
                return fail_target(*code, move->clip_id, move->track_id);
            const auto* sequence = project.find_sequence(move->sequence_id);
            const auto* track = sequence->find_track(move->track_id);
            const auto* clip = track->find_clip(move->clip_id);
            if (!equivalent(clip->time_range(), move->expected_range))
                return fail_target(ConflictCode::ExpectedValueMismatch, move->clip_id);
            auto replacement = clip->with_time_range(move->replacement_range);
            if (!replacement)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, replacement.error())));
            auto next_track = track->replace_clip(std::move(replacement).value());
            if (!next_track)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, next_track.error())));
            auto next_sequence = sequence->replace_track(std::move(next_track).value());
            if (!next_sequence)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, next_sequence.error())));
            auto next_project =
                ProjectEditAccess::replace_sequence(project, std::move(next_sequence).value());
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, next_project.error())));
            project = std::move(next_project).value();
            inverses.emplace_back(MoveClip{move->sequence_id, move->track_id, move->clip_id,
                                           move->replacement_range, move->expected_range});
            dirty.push_back({move->clip_id, move->track_id, move->sequence_id, DirtyFlags::Timing});
        } else if (detail::is_note_command(envelope.command)) {
            auto reduced = detail::reduce_note_command(
                project, envelope.command, transaction, envelope.id,
                allow_tombstone_restore);
            if (!reduced)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(reduced.error()));
            project = std::move(reduced->project);
            inverses.push_back(std::move(reduced->inverse));
            dirty.push_back(reduced->dirty);
        } else if (const auto* tempo = std::get_if<SetTempoMap>(&envelope.command)) {
            if (project.tempo_map() != tempo->expected)
                return fail_target(ConflictCode::ExpectedValueMismatch, project.id());
            project = ProjectEditAccess::replace_tempo_map(project, tempo->replacement);
            inverses.emplace_back(SetTempoMap{tempo->replacement, tempo->expected});
            dirty.push_back({project.id(), {}, {}, DirtyFlags::Timing});
        } else if (const auto* meter = std::get_if<SetMeterMap>(&envelope.command)) {
            if (project.meter_map() != meter->expected)
                return fail_target(ConflictCode::ExpectedValueMismatch, project.id());
            project = ProjectEditAccess::replace_meter_map(project, meter->replacement);
            inverses.emplace_back(SetMeterMap{meter->replacement, meter->expected});
            dirty.push_back({project.id(), {}, {}, DirtyFlags::Timing});
        } else if (const auto* create = std::get_if<CreateAsset>(&envelope.command)) {
            const detail::OwnedIdentity identity{create->asset.id,
                                                 expected_location(ItemKind::Asset, project, {})};
            auto identity_plan = detail::plan_identity_insert(
                project, std::span<const detail::OwnedIdentity>(&identity, 1),
                allow_tombstone_restore, transaction, envelope.id);
            if (!identity_plan)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(identity_plan.error()));
            // Replay references the sealed asset by value; the model never
            // re-derives its ContentHash, so the append is byte-deterministic.
            auto next_project = ProjectEditAccess::append_asset(
                project, create->asset, identity_plan->mutations, identity_plan->next_item_id);
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, next_project.error())));
            project = std::move(next_project).value();
            inverses.emplace_back(RemoveAsset{create->asset.id});
            dirty.push_back({create->asset.id, {}, {}, DirtyFlags::Structure | DirtyFlags::Added});
        } else if (const auto* drop_asset = std::get_if<RemoveAsset>(&envelope.command)) {
            if (const auto code = detail::target_error(
                    project, drop_asset->asset_id, expected_location(ItemKind::Asset, project, {})))
                return fail_target(*code, drop_asset->asset_id);
            const auto* asset = project.find_asset(drop_asset->asset_id);
            if (!asset)
                return fail_target(ConflictCode::TargetMissing, drop_asset->asset_id);
            const MediaAsset removed = *asset;
            const detail::OwnedIdentity identity{removed.id,
                                                 expected_location(ItemKind::Asset, project, {})};
            const auto identity_changes = detail::plan_identity_deactivate(
                std::span<const detail::OwnedIdentity>(&identity, 1));
            auto next_project =
                ProjectEditAccess::remove_asset(project, drop_asset->asset_id, identity_changes);
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, next_project.error())));
            project = std::move(next_project).value();
            inverses.emplace_back(CreateAsset{removed});
            dirty.push_back(
                {drop_asset->asset_id, {}, {}, DirtyFlags::Structure | DirtyFlags::Removed});
        } else if (const auto* chord = std::get_if<SetChordScaleLane>(&envelope.command)) {
            if (const auto code = detail::target_error(
                    project, chord->sequence_id,
                    expected_location(ItemKind::Sequence, project, chord->sequence_id)))
                return fail_target(*code, chord->sequence_id);
            const auto* sequence = project.find_sequence(chord->sequence_id);
            if (!sequence)
                return fail_target(ConflictCode::TargetMissing, chord->sequence_id);
            if (!(sequence->chord_scale_lane() == chord->expected))
                return fail_target(ConflictCode::ExpectedValueMismatch, chord->sequence_id);
            auto next_project = ProjectEditAccess::replace_sequence(
                project, sequence->with_chord_scale_lane(chord->replacement));
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, next_project.error())));
            project = std::move(next_project).value();
            inverses.emplace_back(
                SetChordScaleLane{chord->sequence_id, chord->replacement, chord->expected});
            dirty.push_back({chord->sequence_id, {}, chord->sequence_id, DirtyFlags::Context});
            dirty_contexts.push_back({chord->sequence_id, CompileContextKind::ChordScale});
        } else if (const auto* groove = std::get_if<SetGroove>(&envelope.command)) {
            if (const auto code = detail::target_error(
                    project, groove->sequence_id,
                    expected_location(ItemKind::Sequence, project, groove->sequence_id)))
                return fail_target(*code, groove->sequence_id);
            const auto* sequence = project.find_sequence(groove->sequence_id);
            if (!sequence)
                return fail_target(ConflictCode::TargetMissing, groove->sequence_id);
            if (!(sequence->groove() == groove->expected))
                return fail_target(ConflictCode::ExpectedValueMismatch, groove->sequence_id);
            auto next_project = ProjectEditAccess::replace_sequence(
                project, sequence->with_groove(groove->replacement));
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, next_project.error())));
            project = std::move(next_project).value();
            inverses.emplace_back(
                SetGroove{groove->sequence_id, groove->replacement, groove->expected});
            dirty.push_back({groove->sequence_id, {}, groove->sequence_id, DirtyFlags::Context});
            dirty_contexts.push_back({groove->sequence_id, CompileContextKind::Groove});
        } else {
            const auto& playback = std::get<SetClipPlaybackProperties>(envelope.command);
            if (const auto code = detail::target_error(
                    project, playback.clip_id,
                    expected_location(ItemKind::Clip, project, playback.sequence_id,
                                      playback.track_id, playback.clip_id)))
                return fail_target(*code, playback.clip_id);
            const auto* sequence = project.find_sequence(playback.sequence_id);
            const auto* track = sequence->find_track(playback.track_id);
            const auto* clip = track->find_clip(playback.clip_id);
            if (clip->playback_properties() != playback.expected)
                return fail_target(ConflictCode::ExpectedValueMismatch, playback.clip_id);
            auto next_clip = clip->with_playback_properties(playback.replacement);
            if (!next_clip)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, next_clip.error())));
            auto next_track = track->replace_clip(std::move(next_clip).value());
            if (!next_track)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, next_track.error())));
            auto next_sequence = sequence->replace_track(std::move(next_track).value());
            if (!next_sequence)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, next_sequence.error())));
            auto next_project =
                ProjectEditAccess::replace_sequence(project, std::move(next_sequence).value());
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(runtime::Err(
                    detail::model_failure(transaction, envelope.id, next_project.error())));
            project = std::move(next_project).value();
            inverses.emplace_back(SetClipPlaybackProperties{playback.sequence_id, playback.track_id,
                                                            playback.clip_id, playback.replacement,
                                                            playback.expected});
            dirty.push_back(
                {playback.clip_id, playback.track_id, playback.sequence_id, DirtyFlags::Content});
        }
    }
    std::reverse(inverses.begin(), inverses.end());
    return runtime::Result<ReducedTransaction, TransactionError>(runtime::Ok(ReducedTransaction{
        std::move(project), DirtySet(std::move(dirty), std::move(dirty_contexts)),
        std::move(inverses)}));
}

runtime::Result<ReducedTransaction, TransactionError>
reduce_transaction(const Project& project, const Transaction& transaction) {
    return detail::reduce_transaction(project, transaction, false);
}

} // namespace pulp::timeline
