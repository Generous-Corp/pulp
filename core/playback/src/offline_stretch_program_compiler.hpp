#pragma once

#include "audio_renderer_internal.hpp"

#include <pulp/playback/offline_stretch_artifact.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace pulp::playback::detail {

enum class OfflineStretchProgramCompileStatus : std::uint8_t {
    Progress,
    Complete,
    Failed,
};

struct OfflineStretchProgramCompileError {
    AudioRendererError renderer;
    OfflineStretchErrorCode offline = OfflineStretchErrorCode::None;
};

/// Owns the multi-slice Stretch path so ProgramCompilerTask only coordinates
/// the generic clip lifecycle. One step performs at most one offline builder
/// unit or one host-converter builder unit.
class OfflineStretchProgramCompiler {
  public:
    OfflineStretchProgramCompileStatus
    step(const timeline::Clip& clip, const timeline::Project& project,
         const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
         const AudioRendererLimits& limits, double source_frame_offset,
         std::uint64_t document_revision, std::uint64_t program_generation,
         OfflineStretchArtifactCache& artifact_cache,
         AudioSampleRateConverterCache& converter_cache) noexcept;

    OfflineStretchProgramCompileError error() const noexcept {
        return error_;
    }
    AudioClipRendererProgram take() noexcept;
    void reset() noexcept;

  private:
    OfflineStretchProgramCompileStatus fail(AudioRendererError renderer,
                                            OfflineStretchErrorCode offline) noexcept;

    OfflineStretchCompileJob job_;
    std::shared_ptr<const OfflineStretchArtifact> artifact_;
    std::optional<AudioClipRendererProgram> program_;
    OfflineStretchProgramCompileError error_;
    bool started_ = false;
    bool host_prepared_ = false;
    bool cache_hit_ = false;
};

} // namespace pulp::playback::detail
