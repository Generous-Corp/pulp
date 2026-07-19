#include <pulp/timeline/transaction.hpp>

#include "transaction_internal.hpp"

#include <algorithm>
#include <limits>
#include <tuple>

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

namespace {

template <typename T>
runtime::Result<T, TransactionError>
reject(ConflictCode code, const Transaction& transaction, CommandId command = {}, ItemId item = {},
       ItemId related = {}, std::optional<ModelError> model = std::nullopt) {
    return runtime::Result<T, TransactionError>(runtime::Err(TransactionError{
        code, transaction.id, command, item, related, transaction.expected_revision, {}, model}));
}

std::optional<ConflictCode> target_error(const Project& project, ItemId id, ItemKind kind,
                                         ItemId sequence, ItemId track = {}, ItemId clip = {}) {
    const auto location = project.locate(id);
    if (!location)
        return ConflictCode::TargetMissing;
    if (!location->active)
        return ConflictCode::InactiveTarget;
    if (location->kind != kind)
        return ConflictCode::WrongTargetKind;
    if (location->sequence_id != sequence || location->track_id != track ||
        location->clip_id != clip)
        return ConflictCode::ParentMismatch;
    return std::nullopt;
}

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

std::vector<std::pair<ItemId, ItemKind>> owned_identities(const Clip& clip) {
    std::vector<std::pair<ItemId, ItemKind>> result{{clip.id(), ItemKind::Clip}};
    if (const auto* notes = std::get_if<NoteContent>(&clip.content())) {
        result.reserve(1 + notes->notes().size());
        for (const auto& note : notes->notes())
            result.emplace_back(note.id, ItemKind::Note);
    }
    return result;
}

TransactionError model_failure(const Transaction& transaction, CommandId command,
                               const ModelError& error) {
    return {ConflictCode::ModelInvariant,
            transaction.id,
            command,
            error.item,
            error.related_item,
            transaction.expected_revision,
            {},
            error};
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
        return reject<ReducedTransaction>(ConflictCode::InvalidIdentifier, transaction);
    if (transaction.commands.empty())
        return reject<ReducedTransaction>(ConflictCode::EmptyTransaction, transaction);
    std::vector<CommandId> ids;
    ids.reserve(transaction.commands.size());
    for (const auto& envelope : transaction.commands) {
        if (!envelope.id.valid() || envelope.id.writer != transaction.id.writer)
            return reject<ReducedTransaction>(ConflictCode::InvalidIdentifier, transaction,
                                              envelope.id);
        ids.push_back(envelope.id);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        return reject<ReducedTransaction>(ConflictCode::CommandIdCollision, transaction,
                                          *std::adjacent_find(ids.begin(), ids.end()));

    Project project = original;
    std::vector<DirtyItem> dirty;
    std::vector<Command> inverses;
    inverses.reserve(transaction.commands.size());

    for (const auto& envelope : transaction.commands) {
        auto fail_target = [&](ConflictCode code, ItemId item, ItemId related = {}) {
            return reject<ReducedTransaction>(code, transaction, envelope.id, item, related);
        };
        if (const auto* insert = std::get_if<InsertClip>(&envelope.command)) {
            if (const auto code = target_error(project, insert->sequence_id, ItemKind::Sequence,
                                               insert->sequence_id))
                return fail_target(*code, insert->sequence_id);
            if (const auto code = target_error(project, insert->track_id, ItemKind::Track,
                                               insert->sequence_id, insert->track_id))
                return fail_target(*code, insert->track_id, insert->sequence_id);
            if (const auto media_error = validate_media(project, insert->clip))
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, *media_error)));
            std::vector<IdentityMutation> identity_changes;
            std::uint64_t next = project.next_item_id();
            for (const auto [id, kind] : owned_identities(insert->clip)) {
                ItemLocation wanted{kind, insert->sequence_id, insert->track_id, insert->clip.id(),
                                    true};
                const auto existing = project.locate(id);
                if (allow_tombstone_restore && existing) {
                    if (!existing || existing->active || existing->kind != wanted.kind ||
                        existing->sequence_id != wanted.sequence_id ||
                        existing->track_id != wanted.track_id ||
                        existing->clip_id != wanted.clip_id)
                        return fail_target(ConflictCode::IdentityNotAvailable, id);
                    identity_changes.push_back({IdentityMutationKind::Reactivate, id, wanted});
                } else {
                    if (existing || id.value < project.next_item_id())
                        return fail_target(ConflictCode::IdentityNotAvailable, id);
                    identity_changes.push_back({IdentityMutationKind::Insert, id, wanted});
                    next = std::max(next, id.value == std::numeric_limits<std::uint64_t>::max() - 1
                                              ? std::numeric_limits<std::uint64_t>::max()
                                              : id.value + 1);
                }
            }
            const auto* sequence = project.find_sequence(insert->sequence_id);
            const auto* track = sequence->find_track(insert->track_id);
            auto next_track = track->insert_clip(insert->clip);
            if (!next_track)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, next_track.error())));
            auto next_sequence = sequence->replace_track(std::move(next_track).value());
            if (!next_sequence)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, next_sequence.error())));
            auto next_project = ProjectEditAccess::replace_sequence(
                project, std::move(next_sequence).value(), identity_changes, next);
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, next_project.error())));
            project = std::move(next_project).value();
            inverses.emplace_back(
                RemoveClip{insert->sequence_id, insert->track_id, insert->clip.id()});
            dirty.push_back({insert->clip.id(), insert->track_id, insert->sequence_id,
                             DirtyFlags::Structure | DirtyFlags::Added});
        } else if (const auto* remove = std::get_if<RemoveClip>(&envelope.command)) {
            if (const auto code =
                    target_error(project, remove->clip_id, ItemKind::Clip, remove->sequence_id,
                                 remove->track_id, remove->clip_id))
                return fail_target(*code, remove->clip_id, remove->track_id);
            const auto* sequence = project.find_sequence(remove->sequence_id);
            const auto* track = sequence ? sequence->find_track(remove->track_id) : nullptr;
            const auto* clip = track ? track->find_clip(remove->clip_id) : nullptr;
            if (!clip)
                return fail_target(ConflictCode::TargetMissing, remove->clip_id);
            const Clip removed = *clip;
            std::vector<IdentityMutation> identity_changes;
            for (const auto [id, kind] : owned_identities(removed))
                identity_changes.push_back(
                    {IdentityMutationKind::Deactivate,
                     id,
                     {kind, remove->sequence_id, remove->track_id, remove->clip_id, false}});
            auto next_track = track->erase_clip(remove->clip_id);
            if (!next_track)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, next_track.error())));
            auto next_sequence = sequence->replace_track(std::move(next_track).value());
            if (!next_sequence)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, next_sequence.error())));
            auto next_project = ProjectEditAccess::replace_sequence(
                project, std::move(next_sequence).value(), identity_changes);
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, next_project.error())));
            project = std::move(next_project).value();
            inverses.emplace_back(InsertClip{remove->sequence_id, remove->track_id, removed});
            dirty.push_back({remove->clip_id, remove->track_id, remove->sequence_id,
                             DirtyFlags::Structure | DirtyFlags::Removed});
        } else if (const auto* move = std::get_if<MoveClip>(&envelope.command)) {
            if (const auto code = target_error(project, move->clip_id, ItemKind::Clip,
                                               move->sequence_id, move->track_id, move->clip_id))
                return fail_target(*code, move->clip_id, move->track_id);
            const auto* sequence = project.find_sequence(move->sequence_id);
            const auto* track = sequence->find_track(move->track_id);
            const auto* clip = track->find_clip(move->clip_id);
            if (!equivalent(clip->time_range(), move->expected_range))
                return fail_target(ConflictCode::ExpectedValueMismatch, move->clip_id);
            auto replacement = clip->with_time_range(move->replacement_range);
            if (!replacement)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, replacement.error())));
            auto next_track = track->replace_clip(std::move(replacement).value());
            if (!next_track)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, next_track.error())));
            auto next_sequence = sequence->replace_track(std::move(next_track).value());
            if (!next_sequence)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, next_sequence.error())));
            auto next_project =
                ProjectEditAccess::replace_sequence(project, std::move(next_sequence).value());
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, next_project.error())));
            project = std::move(next_project).value();
            inverses.emplace_back(MoveClip{move->sequence_id, move->track_id, move->clip_id,
                                           move->replacement_range, move->expected_range});
            dirty.push_back({move->clip_id, move->track_id, move->sequence_id, DirtyFlags::Timing});
        } else if (const auto* velocity_value = std::get_if<SetNoteVelocity>(&envelope.command)) {
            const auto& velocity = *velocity_value;
            if (const auto code =
                    target_error(project, velocity.note_id, ItemKind::Note, velocity.sequence_id,
                                 velocity.track_id, velocity.clip_id))
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
                    runtime::Err(model_failure(transaction, envelope.id, next_notes.error())));
            auto next_clip = clip->with_content(std::move(next_notes).value());
            if (!next_clip)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, next_clip.error())));
            auto next_track = track->replace_clip(std::move(next_clip).value());
            if (!next_track)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, next_track.error())));
            auto next_sequence = sequence->replace_track(std::move(next_track).value());
            if (!next_sequence)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, next_sequence.error())));
            auto next_project =
                ProjectEditAccess::replace_sequence(project, std::move(next_sequence).value());
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, next_project.error())));
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
        } else {
            const auto& playback = std::get<SetClipPlaybackProperties>(envelope.command);
            if (const auto code =
                    target_error(project, playback.clip_id, ItemKind::Clip, playback.sequence_id,
                                 playback.track_id, playback.clip_id))
                return fail_target(*code, playback.clip_id);
            const auto* sequence = project.find_sequence(playback.sequence_id);
            const auto* track = sequence->find_track(playback.track_id);
            const auto* clip = track->find_clip(playback.clip_id);
            if (clip->playback_properties() != playback.expected)
                return fail_target(ConflictCode::ExpectedValueMismatch, playback.clip_id);
            auto next_clip = clip->with_playback_properties(playback.replacement);
            if (!next_clip)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, next_clip.error())));
            auto next_track = track->replace_clip(std::move(next_clip).value());
            if (!next_track)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, next_track.error())));
            auto next_sequence = sequence->replace_track(std::move(next_track).value());
            if (!next_sequence)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, next_sequence.error())));
            auto next_project =
                ProjectEditAccess::replace_sequence(project, std::move(next_sequence).value());
            if (!next_project)
                return runtime::Result<ReducedTransaction, TransactionError>(
                    runtime::Err(model_failure(transaction, envelope.id, next_project.error())));
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
