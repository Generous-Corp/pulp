#include "transaction_track_state_internal.hpp"

#include "project_state_access.hpp"
#include "transaction_reduction_support.hpp"

namespace pulp::timeline::detail {
namespace {

std::optional<ModelError> validate_freeze(const Project& project, ItemId track_id,
                                          const std::optional<TrackFreeze>& freeze) noexcept {
    if (!freeze)
        return std::nullopt;
    const auto* asset = project.find_asset(freeze->media.asset_id);
    if (!asset)
        return ModelError{ModelErrorCode::MissingAsset, track_id, freeze->media.asset_id};
    if (freeze->media.source_start.value < 0)
        return ModelError{ModelErrorCode::InvalidMediaRange, track_id, freeze->media.asset_id};
    const auto start = static_cast<std::uint64_t>(freeze->media.source_start.value);
    if (start > asset->frame_count || freeze->media.frame_count > asset->frame_count - start)
        return ModelError{ModelErrorCode::InvalidMediaRange, track_id, freeze->media.asset_id};
    if (asset->sample_rate.normalized() != freeze->sample_rate.normalized())
        return ModelError{ModelErrorCode::IncompatibleSampleRate, track_id, freeze->media.asset_id};
    return std::nullopt;
}

runtime::Result<TrackStateCommandReduction, TransactionError>
reduce_set_track_freeze(const Project& project, const SetTrackFreeze& set,
                        const Transaction& transaction, CommandId command) {
    const ItemLocation expected{
        ItemKind::Track,
        immediate_parent_id(ItemKind::Track, project.id(), set.sequence_id, set.track_id, {}),
        set.sequence_id,
        set.track_id,
        {},
        true};
    if (const auto code = target_error(project, set.track_id, expected))
        return reject_reduction<TrackStateCommandReduction>(*code, transaction, command,
                                                            set.track_id, set.sequence_id);
    const auto* sequence = project.find_sequence(set.sequence_id);
    const auto* track = sequence ? sequence->find_track(set.track_id) : nullptr;
    if (!track)
        return reject_reduction<TrackStateCommandReduction>(
            ConflictCode::TargetMissing, transaction, command, set.track_id, set.sequence_id);
    if (track->freeze() != set.expected)
        return reject_reduction<TrackStateCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                            transaction, command, set.track_id);
    if (const auto error = validate_freeze(project, set.track_id, set.replacement))
        return runtime::Err(model_failure(transaction, command, *error));

    auto next_track = track->with_freeze(set.replacement);
    if (!next_track)
        return runtime::Err(model_failure(transaction, command, next_track.error()));
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    auto next_project =
        ProjectEditAccess::replace_sequence(project, std::move(next_sequence).value());
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));

    return runtime::Ok(TrackStateCommandReduction{
        std::move(next_project).value(),
        SetTrackFreeze{set.sequence_id, set.track_id, set.replacement, set.expected},
        {set.track_id, set.track_id, set.sequence_id, DirtyFlags::Content | DirtyFlags::Freeze}});
}

} // namespace

bool is_track_state_command(const Command& command) noexcept {
    return std::holds_alternative<SetTrackFreeze>(command);
}

runtime::Result<TrackStateCommandReduction, TransactionError>
reduce_track_state_command(const Project& project, const Command& command,
                           const Transaction& transaction, CommandId command_id) {
    if (const auto* freeze = std::get_if<SetTrackFreeze>(&command))
        return reduce_set_track_freeze(project, *freeze, transaction, command_id);
    return reject_reduction<TrackStateCommandReduction>(ConflictCode::ModelInvariant, transaction,
                                                        command_id);
}

} // namespace pulp::timeline::detail
