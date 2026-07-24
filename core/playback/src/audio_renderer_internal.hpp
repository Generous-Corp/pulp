#pragma once

#include <pulp/playback/audio_renderer.hpp>

#include <memory>

namespace pulp::playback::detail {

class AudioSampleRateConverterCache {
  public:
    AudioSampleRateConverterCache();
    ~AudioSampleRateConverterCache();
    AudioSampleRateConverterCache(AudioSampleRateConverterCache&&) noexcept;
    AudioSampleRateConverterCache& operator=(AudioSampleRateConverterCache&&) noexcept;
    AudioSampleRateConverterCache(const AudioSampleRateConverterCache&) = delete;
    AudioSampleRateConverterCache& operator=(const AudioSampleRateConverterCache&) = delete;

    runtime::Result<bool, AudioRendererError>
    prepare(timebase::RationalRate source, timebase::RationalRate target, timeline::ItemId item,
            timeline::ItemId related_item, const AudioRendererLimits& limits);

    runtime::Result<std::shared_ptr<const audio::PreparedSampleRateConversion>, AudioRendererError>
    seed(timebase::RationalRate source, timebase::RationalRate target,
         std::shared_ptr<const audio::PreparedSampleRateConversion> converter,
         timeline::ItemId item, timeline::ItemId related_item, const AudioRendererLimits& limits);

    runtime::Result<std::shared_ptr<const audio::PreparedVariableRateConversion>,
                    AudioRendererError>
    seed_host(std::shared_ptr<const audio::AudioFileData> source, std::uint64_t source_start,
              std::uint64_t source_frames,
              std::shared_ptr<const audio::PreparedVariableRateConversion> converter,
              timeline::ItemId item, timeline::ItemId related_item,
              const AudioRendererLimits& limits);

    runtime::Result<bool, AudioRendererError>
    prepare_host(std::shared_ptr<const audio::AudioFileData> source, std::uint64_t source_start,
                 std::uint64_t source_frames, timeline::ItemId item, timeline::ItemId related_item,
                 const AudioRendererLimits& limits);

  private:
    friend runtime::Result<AudioClipRendererProgram, AudioRendererError>
    compile_audio_clip_program_cached(const timeline::Clip&, const timeline::Project&,
                                      const timebase::CompiledTempoMap&,
                                      const DecodedAudioAssetPool&, const AudioRendererLimits&,
                                      AudioSampleRateConverterCache&);
    friend runtime::Result<AudioClipRendererProgram, AudioRendererError>
    compile_take_comp_segment_program_cached(const timeline::TakeLane&, std::size_t,
                                             const timeline::Project&,
                                             const timebase::CompiledTempoMap&,
                                             const DecodedAudioAssetPool&,
                                             const AudioRendererLimits&,
                                             AudioSampleRateConverterCache&);
    friend runtime::Result<AudioClipRendererProgram, AudioRendererError>
    compile_track_freeze_program_cached(const timeline::Track&, const timeline::Project&,
                                        const timebase::CompiledTempoMap&,
                                        const DecodedAudioAssetPool&, const AudioRendererLimits&,
                                        AudioSampleRateConverterCache&);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

runtime::Result<AudioClipRendererProgram, AudioRendererError> compile_audio_clip_program_cached(
    const timeline::Clip& clip, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits, AudioSampleRateConverterCache& cache);

runtime::Result<bool, AudioRendererError> prepare_audio_clip_sample_rate_converters(
    const timeline::Clip& clip, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits, AudioSampleRateConverterCache& cache);

runtime::Result<bool, AudioRendererError> prepare_take_comp_segment_sample_rate_converter(
    const timeline::TakeLane& lane, std::size_t segment_index, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits, AudioSampleRateConverterCache& cache);

runtime::Result<bool, AudioRendererError> prepare_track_freeze_sample_rate_converter(
    const timeline::Track& track, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits, AudioSampleRateConverterCache& cache);

runtime::Result<AudioClipRendererProgram, AudioRendererError>
compile_take_comp_segment_program_cached(const timeline::TakeLane& lane, std::size_t segment_index,
                                         const timeline::Project& project,
                                         const timebase::CompiledTempoMap& tempo_map,
                                         const DecodedAudioAssetPool& assets,
                                         const AudioRendererLimits& limits,
                                         AudioSampleRateConverterCache& cache);

runtime::Result<AudioClipRendererProgram, AudioRendererError> compile_track_freeze_program_cached(
    const timeline::Track& track, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits, AudioSampleRateConverterCache& cache);

} // namespace pulp::playback::detail
