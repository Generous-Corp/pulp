#include "transaction_track_state_internal.hpp"

#include "media_reference_validation.hpp"
#include "project_state_access.hpp"
#include "transaction_dispatch_internal.hpp"
#include "transaction_reduction_support.hpp"

#include <variant>

namespace pulp::timeline::detail {
namespace {

std::optional<ModelError> validate_freeze(const Project& project, ItemId track_id,
                                          const std::optional<TrackFreeze>& freeze) noexcept {
    if (!freeze)
        return std::nullopt;
    if (const auto error =
            validate_media_reference(project, freeze->media, track_id))
        return error;
    const auto* asset = project.find_asset(freeze->media.asset_id);
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

runtime::Result<TrackStateCommandReduction, TransactionError>
reduce_set_track_mixer(const Project& project, const SetTrackMixer& set,
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
    if (track->mixer() != set.expected)
        return reject_reduction<TrackStateCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                            transaction, command, set.track_id);

    // Range and NaN refusal lives in the model, so an out-of-range replacement
    // surfaces here as a model failure rather than a silently clamped fader.
    auto next_track = track->with_mixer(set.replacement);
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
        SetTrackMixer{set.sequence_id, set.track_id, set.replacement, set.expected},
        {set.track_id, set.track_id, set.sequence_id, DirtyFlags::Content | DirtyFlags::Mixer}});
}

runtime::Result<TrackStateCommandReduction, TransactionError>
reduce_set_track_name(const Project& project, const SetTrackName& set,
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
    if (track->name() != set.expected)
        return reject_reduction<TrackStateCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                            transaction, command, set.track_id);

    auto next_sequence = sequence->replace_track(track->with_name(set.replacement));
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    auto next_project =
        ProjectEditAccess::replace_sequence(project, std::move(next_sequence).value());
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));

    return runtime::Ok(TrackStateCommandReduction{
        std::move(next_project).value(),
        SetTrackName{set.sequence_id, set.track_id, set.replacement, set.expected},
        {set.track_id, set.track_id, set.sequence_id, DirtyFlags::Content}});
}

} // namespace

bool is_track_state_command(const Command& command) noexcept {
    return std::visit([]<typename T>(const T&) { return is_track_state_command_type<T>; }, command);
}

runtime::Result<TrackStateCommandReduction, TransactionError>
reduce_track_state_command(const Project& project, const Command& command,
                           const Transaction& transaction, CommandId command_id) {
    // The family predicate above this call and the arms below it are two
    // statements of the same list, and a chain of get_if proves nothing about
    // its own coverage. Visiting puts a claimed-but-unhandled command in front
    // of the compiler, which is where the outer dispatch already resolves it.
    return std::visit(
        [&]<typename T>(
            const T& value) -> runtime::Result<TrackStateCommandReduction, TransactionError> {
            if constexpr (std::is_same_v<T, SetTrackFreeze>)
                return reduce_set_track_freeze(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, SetTrackMixer>)
                return reduce_set_track_mixer(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, SetTrackName>)
                return reduce_set_track_name(project, value, transaction, command_id);
            else {
                static_assert(!is_track_state_command_type<T>,
                              "a command claimed by is_track_state_command_type in "
                              "transaction_dispatch_internal.hpp has no arm here; add "
                              "one, or drop it from the claim list");
                // Reached only by an alternative no family claims, which the
                // caller's predicate already excludes. Kept as a rejection rather
                // than std::unreachable(): this TU is -fno-exceptions, so being
                // wrong here would abort the process, not fail one transaction.
                return reject_reduction<TrackStateCommandReduction>(ConflictCode::ModelInvariant,
                                                                    transaction, command_id);
            }
        },
        command);
}

} // namespace pulp::timeline::detail
