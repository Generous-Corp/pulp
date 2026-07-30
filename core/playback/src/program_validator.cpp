#include "program_validator.hpp"

#include "track_audio_program_compiler.hpp"

namespace pulp::playback::detail {

ProgramValidationStep ProgramValidator::step(
    std::span<const std::shared_ptr<const TrackProgram>> tracks,
    timeline::ItemId project_id, std::uint64_t document_revision,
    std::uint64_t generation) noexcept {
    if (track_ == tracks.size()) return {ProgramValidationStatus::Complete, {}};
    const auto& track = tracks[track_];
    if (clip_ == 0
        && (!track->id().valid() || track->generation() == 0
            || (track_ && tracks[track_ - 1]->id() == track->id())))
        return {ProgramValidationStatus::Failed,
                {CompileErrorCode::InvalidStructure, track->id(), document_revision}};

    const auto clips = track->ordered_clip_ids();
    if (clip_ < clips.size()) {
        const auto id = clips[clip_++];
        if (!id.valid())
            return {ProgramValidationStatus::Failed,
                    {CompileErrorCode::InvalidStructure, id, document_revision}};
        return {};
    }

    const auto audio_clips = track->audio_program()
        ? track->audio_program()->clips()
        : std::span<const AudioClipRendererProgram>{};
    if (audio_clip_ < audio_clips.size()) {
        const auto& clip = audio_clips[audio_clip_++];
        if (!TrackAudioProgramCompiler::has_current_provenance(
                clip, project_id, document_revision, generation))
            return {ProgramValidationStatus::Failed,
                    {CompileErrorCode::AudioProgramInvalid, clip.id, document_revision,
                     AudioRendererErrorCode::OfflineStretchFailed}};
        return {};
    }

    const auto notes = track->arrangement_note_events();
    if (note_ < notes.size()) {
        const auto& event = notes[note_];
        const bool malformed =
            !event.clip_id.valid() || !event.note_id.valid() || event.pitch > 127
            || event.channel > 15
            || static_cast<unsigned>(event.kind)
                   > static_cast<unsigned>(NoteProgramEventKind::On)
            || (note_ != 0 && note_program_event_less(event, notes[note_ - 1]));
        ++note_;
        if (malformed)
            return {ProgramValidationStatus::Failed,
                    {CompileErrorCode::InvalidStructure, event.note_id, document_revision}};
        return {};
    }

    clip_ = 0;
    audio_clip_ = 0;
    note_ = 0;
    ++track_;
    return {};
}

} // namespace pulp::playback::detail
