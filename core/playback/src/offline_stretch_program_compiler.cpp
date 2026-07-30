#include "offline_stretch_program_compiler.hpp"

#include <new>
#include <stdexcept>
#include <utility>
#include <variant>

namespace pulp::playback::detail {

OfflineStretchProgramCompileStatus
OfflineStretchProgramCompiler::fail(AudioRendererError renderer,
                                    OfflineStretchErrorCode offline) noexcept {
    error_ = {renderer, offline};
    return OfflineStretchProgramCompileStatus::Failed;
}

OfflineStretchProgramCompileStatus OfflineStretchProgramCompiler::step(
    const timeline::Clip& clip, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits, double source_frame_offset, std::uint64_t document_revision,
    std::uint64_t program_generation, OfflineStretchArtifactCache& artifact_cache,
    AudioSampleRateConverterCache& converter_cache) noexcept {
#if defined(__cpp_exceptions)
    try {
#endif
        const auto* media = std::get_if<timeline::MediaRef>(&clip.content());
        const auto* decoded = media ? assets.find(media->asset_id) : nullptr;
        if (!media || !decoded || source_frame_offset != 0.0)
            return fail({AudioRendererErrorCode::OfflineStretchFailed, clip.id(), {}, 0, 0},
                        OfflineStretchErrorCode::InvalidClip);

        if (!started_) {
            const auto begun =
                job_.begin(clip, project, tempo_map, *decoded,
                           OfflineStretchLimits{limits.max_offline_stretch_input_frames,
                                                limits.max_offline_stretch_output_frames,
                                                limits.max_offline_stretch_input_bytes,
                                                limits.max_sample_rate_converter_bytes,
                                                limits.max_offline_stretch_scratch_allocation_bytes,
                                                limits.max_offline_stretch_output_bytes,
                                                limits.max_offline_stretch_artifact_bytes,
                                                limits.max_offline_stretch_cache_bytes,
                                                limits.max_offline_stretch_artifacts},
                           OfflineStretchAlgorithmConfig{limits.offline_stretch_algorithm_version,
                                                         limits.offline_stretch_max_time_ratio},
                           limits.offline_stretch_max_block_frames, &artifact_cache);
            started_ = true;
            if (begun == OfflineStretchCompileStatus::Failed)
                return fail({AudioRendererErrorCode::OfflineStretchFailed, clip.id(), {}, 0, 0},
                            job_.error().code);
            if (begun == OfflineStretchCompileStatus::Progress)
                return OfflineStretchProgramCompileStatus::Progress;
        }

        if (job_.status() == OfflineStretchCompileStatus::Progress) {
            const auto stepped = job_.step();
            if (stepped == OfflineStretchCompileStatus::Failed)
                return fail({AudioRendererErrorCode::OfflineStretchFailed, clip.id(), {}, 0, 0},
                            job_.error().code);
            return OfflineStretchProgramCompileStatus::Progress;
        }

        if (!artifact_) {
            cache_hit_ = job_.cache_hit();
            artifact_ = job_.take();
        }
        if (!artifact_)
            return fail({AudioRendererErrorCode::OfflineStretchFailed, clip.id(), {}, 0, 0},
                        OfflineStretchErrorCode::ProcessorProtocolError);

        if (!host_prepared_) {
            auto prepared =
                converter_cache.prepare_host(artifact_->audio, 0, artifact_->key.target_frame_count,
                                             clip.id(), media->asset_id, limits);
            if (!prepared)
                return fail(prepared.error(), OfflineStretchErrorCode::None);
            if (!*prepared)
                return OfflineStretchProgramCompileStatus::Progress;
            host_prepared_ = true;
        }

        auto compiled =
            compile_audio_clip_program_cached(clip, project, tempo_map, assets, limits,
                                              converter_cache, source_frame_offset, artifact_);
        if (!compiled)
            return fail(compiled.error(), OfflineStretchErrorCode::None);
        program_.emplace(std::move(compiled).value());
        program_->offline_stretch_provenance =
            std::make_shared<const OfflineStretchProvenance>(OfflineStretchProvenance{
                clip.id(), project.id(), document_revision, program_generation, cache_hit_});
        return OfflineStretchProgramCompileStatus::Complete;
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return fail({AudioRendererErrorCode::CapacityExceeded, clip.id(), {}, 0, 0},
                    OfflineStretchErrorCode::CapacityExceeded);
    } catch (const std::length_error&) {
        return fail({AudioRendererErrorCode::CapacityExceeded, clip.id(), {}, 0, 0},
                    OfflineStretchErrorCode::CapacityExceeded);
    }
#endif
}

AudioClipRendererProgram OfflineStretchProgramCompiler::take() noexcept {
    auto result = std::move(*program_);
    reset();
    return result;
}

void OfflineStretchProgramCompiler::reset() noexcept {
    artifact_.reset();
    program_.reset();
    error_ = {};
    started_ = false;
    host_prepared_ = false;
    cache_hit_ = false;
}

} // namespace pulp::playback::detail
