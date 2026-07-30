#pragma once

#include "audio_renderer_internal.hpp"
#include "offline_stretch_program_compiler.hpp"

#include <pulp/playback/program.hpp>

#include <cstdint>
#include <optional>

namespace pulp::playback::detail {

enum class TrackAudioClipCompileStatus : std::uint8_t { Progress, Complete, Failed };

/// Owns per-task audio clip compilation, including the alternate offline
/// Stretch path and its generation-specific publication policy. The generic
/// program state machine sees one clip protocol regardless of time conform.
class TrackAudioProgramCompiler {
  public:
    TrackAudioClipCompileStatus
    step(const timeline::Clip& clip, const timeline::Project& project,
         const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
         const AudioRendererLimits& limits, double source_frame_offset,
         std::uint64_t document_revision, std::uint64_t program_generation,
         OfflineStretchArtifactCache& artifact_cache) noexcept;

    TrackAudioClipCompileStatus
    step_track_freeze(const timeline::Track& track, const timeline::Project& project,
                      const timebase::CompiledTempoMap& tempo_map,
                      const DecodedAudioAssetPool& assets,
                      const AudioRendererLimits& limits) noexcept;
    TrackAudioClipCompileStatus
    step_take_comp(const timeline::TakeLane& lane, std::size_t segment_index,
                   const timeline::Project& project,
                   const timebase::CompiledTempoMap& tempo_map,
                   const DecodedAudioAssetPool& assets,
                   const AudioRendererLimits& limits) noexcept;

    AudioClipRendererProgram take() noexcept;
    OfflineStretchProgramCompileError error() const noexcept { return error_; }
    AudioSampleRateConverterCache& converters() noexcept { return converters_; }

    static bool requires_generation_refresh(const TrackProgram& track) noexcept;
    static bool has_current_provenance(const AudioClipRendererProgram& clip,
                                       timeline::ItemId project_id,
                                       std::uint64_t document_revision,
                                       std::uint64_t program_generation) noexcept;

  private:
    AudioSampleRateConverterCache converters_;
    OfflineStretchProgramCompiler offline_stretch_;
    std::optional<AudioClipRendererProgram> completed_;
    OfflineStretchProgramCompileError error_;
};

} // namespace pulp::playback::detail
