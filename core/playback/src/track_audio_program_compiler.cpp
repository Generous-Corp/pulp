#include "track_audio_program_compiler.hpp"

#include <algorithm>
#include <utility>

namespace pulp::playback::detail {

TrackAudioClipCompileStatus TrackAudioProgramCompiler::step(
    const timeline::Clip& clip, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits, double source_frame_offset,
    std::uint64_t document_revision, std::uint64_t program_generation,
    OfflineStretchArtifactCache& artifact_cache) noexcept {
    if (clip.time_conform() == timeline::TimeConform::Stretch) {
        const auto status = offline_stretch_.step(
            clip, project, tempo_map, assets, limits, source_frame_offset, document_revision,
            program_generation, artifact_cache, converters_);
        if (status == OfflineStretchProgramCompileStatus::Progress)
            return TrackAudioClipCompileStatus::Progress;
        if (status == OfflineStretchProgramCompileStatus::Failed) {
            error_ = offline_stretch_.error();
            return TrackAudioClipCompileStatus::Failed;
        }
        completed_.emplace(offline_stretch_.take());
        return TrackAudioClipCompileStatus::Complete;
    }

    auto prepared = prepare_audio_clip_sample_rate_converters(
        clip, project, tempo_map, assets, limits, converters_);
    if (!prepared) {
        error_ = {prepared.error(), OfflineStretchErrorCode::None};
        return TrackAudioClipCompileStatus::Failed;
    }
    if (!*prepared) return TrackAudioClipCompileStatus::Progress;
    auto compiled = compile_audio_clip_program_cached(
        clip, project, tempo_map, assets, limits, converters_, source_frame_offset);
    if (!compiled) {
        error_ = {compiled.error(), OfflineStretchErrorCode::None};
        return TrackAudioClipCompileStatus::Failed;
    }
    completed_.emplace(std::move(compiled).value());
    return TrackAudioClipCompileStatus::Complete;
}

TrackAudioClipCompileStatus TrackAudioProgramCompiler::step_track_freeze(
    const timeline::Track& track, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits) noexcept {
    auto prepared = prepare_track_freeze_sample_rate_converter(
        track, project, tempo_map, assets, limits, converters_);
    if (!prepared) {
        error_ = {prepared.error(), OfflineStretchErrorCode::None};
        return TrackAudioClipCompileStatus::Failed;
    }
    if (!*prepared) return TrackAudioClipCompileStatus::Progress;
    auto compiled = compile_track_freeze_program_cached(
        track, project, tempo_map, assets, limits, converters_);
    if (!compiled) {
        error_ = {compiled.error(), OfflineStretchErrorCode::None};
        return TrackAudioClipCompileStatus::Failed;
    }
    completed_.emplace(std::move(compiled).value());
    return TrackAudioClipCompileStatus::Complete;
}

TrackAudioClipCompileStatus TrackAudioProgramCompiler::step_take_comp(
    const timeline::TakeLane& lane, std::size_t segment_index,
    const timeline::Project& project, const timebase::CompiledTempoMap& tempo_map,
    const DecodedAudioAssetPool& assets, const AudioRendererLimits& limits) noexcept {
    auto prepared = prepare_take_comp_segment_sample_rate_converter(
        lane, segment_index, project, tempo_map, assets, limits, converters_);
    if (!prepared) {
        error_ = {prepared.error(), OfflineStretchErrorCode::None};
        return TrackAudioClipCompileStatus::Failed;
    }
    if (!*prepared) return TrackAudioClipCompileStatus::Progress;
    auto compiled = compile_take_comp_segment_program_cached(
        lane, segment_index, project, tempo_map, assets, limits, converters_);
    if (!compiled) {
        error_ = {compiled.error(), OfflineStretchErrorCode::None};
        return TrackAudioClipCompileStatus::Failed;
    }
    completed_.emplace(std::move(compiled).value());
    return TrackAudioClipCompileStatus::Complete;
}

AudioClipRendererProgram TrackAudioProgramCompiler::take() noexcept {
    auto result = std::move(*completed_);
    completed_.reset();
    error_ = {};
    return result;
}

bool TrackAudioProgramCompiler::requires_generation_refresh(const TrackProgram& track) noexcept {
    const auto* audio_program = track.audio_program();
    return audio_program
        && std::any_of(audio_program->clips().begin(), audio_program->clips().end(),
                       [](const AudioClipRendererProgram& clip) {
                           return clip.source_time_mapping
                               == AudioClipRendererProgram::SourceTimeMapping::OfflineStretchArtifact;
                       });
}

bool TrackAudioProgramCompiler::has_current_provenance(
    const AudioClipRendererProgram& clip, timeline::ItemId project_id,
    std::uint64_t document_revision, std::uint64_t program_generation) noexcept {
    return clip.source_time_mapping
               != AudioClipRendererProgram::SourceTimeMapping::OfflineStretchArtifact
        || (clip.offline_stretch_provenance
            && clip.offline_stretch_provenance->matches(
                clip.id, project_id, document_revision, program_generation));
}

} // namespace pulp::playback::detail
