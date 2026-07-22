#include <pulp/timeline/transaction.hpp>

#include "transaction_automation_internal.hpp"
#include "transaction_internal.hpp"
#include "transaction_reduction_support.hpp"

#include <algorithm>
#include <tuple>

namespace pulp::timeline {

namespace {

std::optional<ModelError> validate_media(const Project& project, const Clip& clip) noexcept {
    const auto* media = std::get_if<MediaRef>(&clip.content());
    if (!media)
        return std::nullopt;
    const auto* asset = project.find_asset(media->asset_id);
    if (!asset)
        return ModelError{ModelErrorCode::MissingAsset, clip.id(), media->asset_id};
    if (media->source_start.value < 0)
        return ModelError{ModelErrorCode::InvalidMediaRange, clip.id(), media->asset_id};
    const auto start = static_cast<std::uint64_t>(media->source_start.value);
    if (start > asset->frame_count || media->frame_count > asset->frame_count - start)
        return ModelError{ModelErrorCode::InvalidMediaRange, clip.id(), media->asset_id};
    return std::nullopt;
}

// An active target identity with parent_id computed the one canonical way.
// detail::target_error compares a located identity against this expectation.
ItemLocation expected_location(ItemKind kind, const Project& project, ItemId sequence,
                               ItemId track = {}, ItemId clip = {}) {
    return ItemLocation{kind, immediate_parent_id(kind, project.id(), sequence, track, clip),
                        sequence, track, clip, true};
}

std::vector<detail::OwnedIdentity> owned_identities(const Clip& clip, ItemId sequence,
                                                    ItemId track) {
    const auto clip_parent = immediate_parent_id(ItemKind::Clip, {}, sequence, track, clip.id());
    const auto note_parent = immediate_parent_id(ItemKind::Note, {}, sequence, track, clip.id());
    std::vector<detail::OwnedIdentity> result{
        {clip.id(), ItemLocation{ItemKind::Clip, clip_parent, sequence, track, clip.id(), true}}};
    if (const auto* notes = std::get_if<NoteContent>(&clip.content())) {
        result.reserve(1 + notes->notes().size());
        for (const auto& note : notes->notes())
            result.push_back({note.id, ItemLocation{ItemKind::Note, note_parent, sequence, track,
                                                    clip.id(), true}});
    }
    return result;
}

} // namespace

DirtySet::DirtySet(std::vector<DirtyItem> items) : items_(std::move(items)) {
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
    return sizeof(DirtySet) + items_.size() * sizeof(DirtyItem);
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
            return detail::reject_reduction<ReducedTransaction>(
                ConflictCode::InvalidIdentifier, transaction, envelope.id);
        ids.push_back(envelope.id);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        return detail::reject_reduction<ReducedTransaction>(
            ConflictCode::CommandIdCollision, transaction,
            *std::adjacent_find(ids.begin(), ids.end()));

    Project project = original;
    std::vector<DirtyItem> dirty;
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
            if (const auto code = detail::target_error(
                    project, insert->track_id,
                    expected_location(ItemKind::Track, project, insert->sequence_id,
                                      insert->track_id)))
                return fail_target(*code, insert->track_id, insert->sequence_id);
            if (const auto media_error = validate_media(project, insert->clip))
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(detail::model_failure(transaction, envelope.id, *media_error)));
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
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_track.error())));
            auto next_sequence = sequence->replace_track(std::move(next_track).value());
            if (!next_sequence)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_sequence.error())));
            auto next_project = ProjectEditAccess::replace_sequence(
                project, std::move(next_sequence).value(), identity_plan->mutations,
                identity_plan->next_item_id);
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
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
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_track.error())));
            auto next_sequence = sequence->replace_track(std::move(next_track).value());
            if (!next_sequence)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_sequence.error())));
            auto next_project = ProjectEditAccess::replace_sequence(
                project, std::move(next_sequence).value(), identity_changes);
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_project.error())));
            project = std::move(next_project).value();
            inverses.emplace_back(InsertClip{remove->sequence_id, remove->track_id, removed});
            dirty.push_back({remove->clip_id, remove->track_id, remove->sequence_id,
                             DirtyFlags::Structure | DirtyFlags::Removed});
        } else if (detail::is_automation_command(envelope.command)) {
            auto reduced = detail::reduce_automation_command(
                project, envelope.command, transaction, envelope.id, allow_tombstone_restore);
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
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, replacement.error())));
            auto next_track = track->replace_clip(std::move(replacement).value());
            if (!next_track)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_track.error())));
            auto next_sequence = sequence->replace_track(std::move(next_track).value());
            if (!next_sequence)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_sequence.error())));
            auto next_project =
                ProjectEditAccess::replace_sequence(project, std::move(next_sequence).value());
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_project.error())));
            project = std::move(next_project).value();
            inverses.emplace_back(MoveClip{move->sequence_id, move->track_id, move->clip_id,
                                           move->replacement_range, move->expected_range});
            dirty.push_back({move->clip_id, move->track_id, move->sequence_id, DirtyFlags::Timing});
        } else if (const auto* velocity_value = std::get_if<SetNoteVelocity>(&envelope.command)) {
            const auto& velocity = *velocity_value;
            if (const auto code = detail::target_error(
                    project, velocity.note_id,
                    expected_location(ItemKind::Note, project, velocity.sequence_id,
                                      velocity.track_id, velocity.clip_id)))
                return fail_target(*code, velocity.note_id, velocity.clip_id);
            const auto* sequence = project.find_sequence(velocity.sequence_id);
            const auto* track = sequence->find_track(velocity.track_id);
            const auto* clip = track->find_clip(velocity.clip_id);
            const auto* notes = std::get_if<NoteContent>(&clip->content());
            if (!notes)
                return fail_target(ConflictCode::WrongTargetKind, velocity.clip_id);
            const auto found =
                std::find_if(notes->notes().begin(), notes->notes().end(),
                             [&](const NoteEvent& note) { return note.id == velocity.note_id; });
            if (found == notes->notes().end())
                return fail_target(ConflictCode::TargetMissing, velocity.note_id);
            if (found->velocity != velocity.expected_velocity)
                return fail_target(ConflictCode::ExpectedValueMismatch, velocity.note_id);
            NoteEvent replacement = *found;
            replacement.velocity = velocity.replacement_velocity;
            auto next_notes = notes->replace_note(replacement);
            if (!next_notes)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_notes.error())));
            auto next_clip = clip->with_content(std::move(next_notes).value());
            if (!next_clip)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_clip.error())));
            auto next_track = track->replace_clip(std::move(next_clip).value());
            if (!next_track)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_track.error())));
            auto next_sequence = sequence->replace_track(std::move(next_track).value());
            if (!next_sequence)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_sequence.error())));
            auto next_project =
                ProjectEditAccess::replace_sequence(project, std::move(next_sequence).value());
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_project.error())));
            project = std::move(next_project).value();
            inverses.emplace_back(SetNoteVelocity{
                velocity.sequence_id, velocity.track_id, velocity.clip_id, velocity.note_id,
                velocity.replacement_velocity, velocity.expected_velocity});
            dirty.push_back({velocity.note_id, velocity.track_id, velocity.sequence_id,
                             DirtyFlags::Content | DirtyFlags::Notes});
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
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_project.error())));
            project = std::move(next_project).value();
            inverses.emplace_back(RemoveAsset{create->asset.id});
            dirty.push_back(
                {create->asset.id, {}, {}, DirtyFlags::Structure | DirtyFlags::Added});
        } else if (const auto* drop_asset = std::get_if<RemoveAsset>(&envelope.command)) {
            if (const auto code = detail::target_error(
                    project, drop_asset->asset_id,
                    expected_location(ItemKind::Asset, project, {})))
                return fail_target(*code, drop_asset->asset_id);
            const auto* asset = project.find_asset(drop_asset->asset_id);
            if (!asset)
                return fail_target(ConflictCode::TargetMissing, drop_asset->asset_id);
            const MediaAsset removed = *asset;
            const detail::OwnedIdentity identity{removed.id,
                                                 expected_location(ItemKind::Asset, project, {})};
            const auto identity_changes = detail::plan_identity_deactivate(std::span<const detail::OwnedIdentity>(&identity, 1));
            auto next_project =
                ProjectEditAccess::remove_asset(project, drop_asset->asset_id, identity_changes);
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_project.error())));
            project = std::move(next_project).value();
            inverses.emplace_back(CreateAsset{removed});
            dirty.push_back(
                {drop_asset->asset_id, {}, {}, DirtyFlags::Structure | DirtyFlags::Removed});
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
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_clip.error())));
            auto next_track = track->replace_clip(std::move(next_clip).value());
            if (!next_track)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_track.error())));
            auto next_sequence = sequence->replace_track(std::move(next_track).value());
            if (!next_sequence)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
                        detail::model_failure(transaction, envelope.id, next_sequence.error())));
            auto next_project =
                ProjectEditAccess::replace_sequence(project, std::move(next_sequence).value());
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(
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
    return runtime::Result<ReducedTransaction, TransactionError>(runtime::Ok(
        ReducedTransaction{std::move(project), DirtySet(std::move(dirty)), std::move(inverses)}));
}

runtime::Result<ReducedTransaction, TransactionError>
reduce_transaction(const Project& project, const Transaction& transaction) {
    return detail::reduce_transaction(project, transaction, false);
}

} // namespace pulp::timeline
