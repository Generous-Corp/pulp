#include <pulp/playback/program.hpp>
#include <pulp/playback/track_automation_program.hpp>

#include <algorithm>
#include <limits>

namespace pulp::playback {

runtime::Result<ProgramGeneration, ProgramError>
next_program_generation(ProgramGeneration current) noexcept {
    if (current == std::numeric_limits<ProgramGeneration>::max())
        return runtime::Err(ProgramError{ProgramErrorCode::InvalidGeneration, {}});
    return runtime::Ok(current + 1);
}

const CompiledNoteModifier* find_note_modifier(std::span<const CompiledNoteModifier> modifiers,
                                               timeline::ItemId note_id) noexcept {
    const auto found = std::lower_bound(modifiers.begin(), modifiers.end(), note_id.value,
                                        [](const CompiledNoteModifier& entry, std::uint64_t wanted) {
                                            return entry.modifier.note_id.value < wanted;
                                        });
    return found != modifiers.end() && found->modifier.note_id == note_id ? &*found : nullptr;
}

TrackProgram::TrackProgram(timeline::ItemId id, ProgramGeneration generation,
                           ProviderSelectorProgram provider, RendererStatePolicy state_policy,
                           std::vector<timeline::ItemId> clip_ids,
                           std::vector<NoteProgramEvent> note_events,
                           std::vector<CompiledNoteModifier> note_modifiers,
                           std::shared_ptr<const AudioTrackRendererProgram> audio_program,
                           std::vector<timeline::ItemId> device_placement_ids,
                           std::shared_ptr<const TrackAutomationProgram> automation_program,
                           std::uint64_t expanded_clip_count,
                           std::uint64_t expanded_note_event_count,
                           std::uint64_t generated_id_start,
                           std::uint64_t generated_id_count) noexcept
    : id_(id), generation_(generation), provider_(provider), state_policy_(state_policy),
      clip_ids_(std::move(clip_ids)), note_events_(std::move(note_events)),
      note_modifiers_(std::move(note_modifiers)), audio_program_(std::move(audio_program)),
      device_placement_ids_(std::move(device_placement_ids)),
      automation_program_(std::move(automation_program)),
      expanded_clip_count_(expanded_clip_count),
      expanded_note_event_count_(expanded_note_event_count),
      generated_id_start_(generated_id_start),
      generated_id_count_(generated_id_count) {}

PlaybackProgram::PlaybackProgram(ProgramGeneration generation, std::uint64_t document_revision,
                                 timeline::ItemId project_id, timeline::ItemId sequence_id,
                                 std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
                                 std::shared_ptr<const DecodedAudioAssetPool> audio_assets,
                                 AudioRendererLimits audio_limits,
                                 AutomationPlaybackLimits automation_limits,
                                 std::uint64_t generated_id_base,
                                 std::vector<std::shared_ptr<const TrackProgram>> tracks) noexcept
    : generation_(generation), document_revision_(document_revision), project_id_(project_id),
      sequence_id_(sequence_id), tempo_map_(std::move(tempo_map)),
      audio_assets_(std::move(audio_assets)), audio_limits_(audio_limits),
      automation_limits_(automation_limits),
      generated_id_base_(generated_id_base),
      tracks_(std::move(tracks)) {}

const std::shared_ptr<const TrackProgram>*
PlaybackProgram::find_track_owner(timeline::ItemId id) const noexcept {
    const auto found = std::lower_bound(
        tracks_.begin(), tracks_.end(), id,
        [](const auto& track, timeline::ItemId value) { return track->id() < value; });
    return found != tracks_.end() && (*found)->id() == id ? &*found : nullptr;
}

const TrackProgram* PlaybackProgram::find_track(timeline::ItemId id) const noexcept {
    const auto* owner = find_track_owner(id);
    return owner ? owner->get() : nullptr;
}

} // namespace pulp::playback
