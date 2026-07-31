#include <pulp/playback/offline_stretch_artifact.hpp>

#include <pulp/playback/audio_renderer.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace pulp::playback {
namespace {

bool checked_payload_bytes(std::uint64_t frames, std::uint64_t channels, std::uint64_t sample_bytes,
                           std::uint64_t& bytes) noexcept {
    if (channels != 0 && frames > std::numeric_limits<std::uint64_t>::max() / channels)
        return false;
    const auto samples = frames * channels;
    if (sample_bytes != 0 && samples > std::numeric_limits<std::uint64_t>::max() / sample_bytes)
        return false;
    bytes = samples * sample_bytes;
    return true;
}

bool add_retained_bytes(std::uint64_t amount, std::uint64_t& total) noexcept {
    if (amount > std::numeric_limits<std::uint64_t>::max() - total)
        return false;
    total += amount;
    return true;
}

bool add_capacity_bytes(std::size_t count, std::size_t element_bytes,
                        std::uint64_t& total) noexcept {
    if (count != 0 && element_bytes > std::numeric_limits<std::uint64_t>::max() / count)
        return false;
    return add_retained_bytes(static_cast<std::uint64_t>(count) * element_bytes, total);
}

std::uint64_t artifact_bytes(const OfflineStretchArtifact& artifact) noexcept {
    if (!artifact.audio)
        return 0;
    constexpr auto allocation_overhead = 8u * sizeof(void*);
    std::uint64_t total =
        sizeof(OfflineStretchArtifact) + sizeof(audio::AudioFileData) + 2u * allocation_overhead;
    if (!add_capacity_bytes(artifact.key.tempo_points.capacity(), sizeof(timebase::TempoPoint),
                            total) ||
        !add_retained_bytes(allocation_overhead, total) ||
        !add_capacity_bytes(artifact.audio->channels.capacity(), sizeof(std::vector<float>),
                            total) ||
        !add_retained_bytes(allocation_overhead, total))
        return std::numeric_limits<std::uint64_t>::max();
    for (const auto& channel : artifact.audio->channels) {
        if (!add_capacity_bytes(channel.capacity(), sizeof(float), total) ||
            !add_retained_bytes(allocation_overhead, total))
            return std::numeric_limits<std::uint64_t>::max();
    }
    return total;
}

OfflineStretchErrorCode map_prepare_error(audio::FiniteTimeStretchPrepareStatus status) noexcept {
    using Source = audio::FiniteTimeStretchPrepareStatus;
    switch (status) {
    case Source::Prepared:
        return OfflineStretchErrorCode::None;
    case Source::InvalidSource:
    case Source::InvalidSlice:
        return OfflineStretchErrorCode::MissingSource;
    case Source::InvalidSampleRate:
        return OfflineStretchErrorCode::UnsupportedSampleRate;
    case Source::InvalidBlockSize:
        return OfflineStretchErrorCode::InvalidAlgorithmConfig;
    case Source::InvalidTarget:
    case Source::InvalidRatio:
        return OfflineStretchErrorCode::InvalidTempoSchedule;
    case Source::CapacityExceeded:
        return OfflineStretchErrorCode::CapacityExceeded;
    case Source::SampleRateConverterPrepareFailed:
        return OfflineStretchErrorCode::SampleRateConverterPrepareFailed;
    case Source::ProcessorPrepareFailed:
        return OfflineStretchErrorCode::ProcessorPrepareFailed;
    }
    return OfflineStretchErrorCode::ProcessorPrepareFailed;
}

OfflineStretchErrorCode map_step_error(audio::FiniteTimeStretchFailure failure) noexcept {
    using Source = audio::FiniteTimeStretchFailure;
    switch (failure) {
    case Source::None:
        return OfflineStretchErrorCode::None;
    case Source::AllocationFailed:
        return OfflineStretchErrorCode::CapacityExceeded;
    case Source::InvalidRatio:
        return OfflineStretchErrorCode::InvalidRatio;
    case Source::OutputTooShort:
        return OfflineStretchErrorCode::OutputTooShort;
    case Source::OutputTooLong:
        return OfflineStretchErrorCode::OutputTooLong;
    case Source::ProcessorProtocolError:
        return OfflineStretchErrorCode::ProcessorProtocolError;
    }
    return OfflineStretchErrorCode::ProcessorProtocolError;
}

} // namespace

OfflineStretchErrorCode
detail::offline_stretch_error_code(audio::FiniteTimeStretchFailure failure) noexcept {
    return map_step_error(failure);
}

bool detail::offline_stretch_frame_distance(std::int64_t start, std::int64_t end,
                                            std::uint64_t& distance) noexcept {
    if (end <= start)
        return false;
    if (start >= 0) {
        distance = static_cast<std::uint64_t>(end) - static_cast<std::uint64_t>(start);
        return true;
    }
    const auto start_magnitude = static_cast<std::uint64_t>(-(start + 1)) + 1u;
    if (end < 0) {
        const auto end_magnitude = static_cast<std::uint64_t>(-(end + 1)) + 1u;
        distance = start_magnitude - end_magnitude;
        return true;
    }
    const auto positive_end = static_cast<std::uint64_t>(end);
    if (start_magnitude > std::numeric_limits<std::uint64_t>::max() - positive_end)
        return false;
    distance = start_magnitude + positive_end;
    return true;
}

std::shared_ptr<const OfflineStretchArtifact>
OfflineStretchArtifactCache::find(const OfflineStretchArtifactKey& key) const noexcept {
    const auto found =
        std::find_if(artifacts_.begin(), artifacts_.end(),
                     [&key](const auto& artifact) { return artifact && artifact->key == key; });
    return found == artifacts_.end() ? nullptr : *found;
}

bool OfflineStretchArtifactCache::insert(std::shared_ptr<const OfflineStretchArtifact> artifact,
                                         OfflineStretchLimits limits) noexcept {
    if (!artifact || !artifact->audio || !artifact->key.source_content_hash.valid() ||
        !artifact->key.decoded_content_hash.valid() ||
        artifact->key.timeline_sample_rate.denominator != 1 ||
        artifact->key.timeline_sample_rate.numerator != artifact->audio->sample_rate ||
        artifact->key.target_frame_count != artifact->audio->num_frames() ||
        artifact->key.channel_count != artifact->audio->num_channels() ||
        !std::all_of(artifact->audio->channels.begin(), artifact->audio->channels.end(),
                     [&artifact](const auto& channel) {
                         return channel.size() == artifact->key.target_frame_count;
                     }))
        return false;
    if (find(artifact->key))
        return true;
    const auto artifact_retained = artifact_bytes(*artifact);
    constexpr auto cache_slot_bytes =
        sizeof(std::shared_ptr<const OfflineStretchArtifact>) + 8u * sizeof(void*);
    if (artifact_retained > std::numeric_limits<std::uint64_t>::max() - cache_slot_bytes)
        return false;
    const auto bytes = artifact_retained + cache_slot_bytes;
    if (artifact_retained > limits.max_artifact_bytes ||
        artifacts_.size() >= limits.max_cached_artifacts || bytes > limits.max_cache_bytes ||
        retained_bytes_ > limits.max_cache_bytes - bytes)
        return false;
#if defined(__cpp_exceptions)
    try {
#endif
        artifacts_.push_back(std::move(artifact));
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
#endif
    retained_bytes_ += bytes;
    return true;
}

void OfflineStretchArtifactCache::clear() noexcept {
    std::vector<std::shared_ptr<const OfflineStretchArtifact>>{}.swap(artifacts_);
    retained_bytes_ = 0;
}

void OfflineStretchArtifactCache::constrain(OfflineStretchLimits limits) noexcept {
    if (artifacts_.size() > limits.max_cached_artifacts ||
        retained_bytes_ > limits.max_cache_bytes ||
        std::any_of(artifacts_.begin(), artifacts_.end(), [&limits](const auto& artifact) {
            return !artifact || artifact_bytes(*artifact) > limits.max_artifact_bytes;
        }))
        clear();
}

struct OfflineStretchCompileJob::Impl {
    static float ratio_at_frame(void* context, std::uint64_t frame,
                                std::uint64_t input_frames) noexcept {
        auto& self = *static_cast<Impl*>(context);
        if (frame <= self.previous_input_frame || input_frames == 0)
            return self.previous_ratio;
        const auto endpoint = std::min(frame, input_frames);
        const auto tick_span = static_cast<long double>(self.key_value.musical_tick_end.value) -
                               static_cast<long double>(self.key_value.musical_tick_start.value);
        const auto phase =
            static_cast<long double>(endpoint) / static_cast<long double>(input_frames);
        const auto tick =
            static_cast<long double>(self.key_value.musical_tick_start.value) + tick_span * phase;
        const auto raw = self.tempo_map->fractional_ticks_to_samples(tick) - self.raw_start;
        const auto corrected =
            raw +
            phase * (static_cast<long double>(self.key_value.target_frame_count) - self.raw_end);
        const auto ratio =
            static_cast<float>((corrected - self.previous_output_frame) /
                               static_cast<long double>(endpoint - self.previous_input_frame));
        self.previous_input_frame = endpoint;
        self.previous_output_frame = corrected;
        if (std::isfinite(ratio) && ratio > 0.0f)
            self.previous_ratio = ratio;
        return ratio;
    }

    OfflineStretchCompileStatus fail(OfflineStretchErrorCode code, timeline::ItemId item = {},
                                     timeline::ItemId related = {}, std::uint64_t actual = 0,
                                     std::uint64_t limit = 0) noexcept {
        result_status = OfflineStretchCompileStatus::Failed;
        error_value = {code, item, related, actual, limit};
        return result_status;
    }

    OfflineStretchCompileStatus begin(const timeline::Clip& clip, const timeline::Project& project,
                                      const timebase::CompiledTempoMap& compiled_tempo_map,
                                      const DecodedAudioAsset& decoded,
                                      OfflineStretchLimits requested_limits,
                                      OfflineStretchAlgorithmConfig algorithm,
                                      std::uint32_t work_block_frames,
                                      OfflineStretchArtifactCache* requested_cache) {
        result_status = OfflineStretchCompileStatus::Failed;
        error_value = {};
        cache_was_hit = false;
        artifact.reset();
        cache = requested_cache;
        limits = requested_limits;
        tempo_map = &compiled_tempo_map;
        previous_input_frame = 0;
        previous_output_frame = 0.0L;
        previous_ratio = 1.0f;

        const auto* media = std::get_if<timeline::MediaRef>(&clip.content());
        const auto* metadata = media ? project.find_asset(media->asset_id) : nullptr;
        if (!media || clip.time_anchor() != timeline::ClipTimeAnchor::Musical ||
            clip.time_conform() != timeline::TimeConform::Stretch || clip.end() <= clip.start())
            return fail(OfflineStretchErrorCode::InvalidClip, clip.id());
        if (!metadata || !decoded.audio)
            return fail(OfflineStretchErrorCode::MissingSource, clip.id(), media->asset_id);
        if (!decoded.content_hash.valid() || decoded.content_hash != metadata->content_hash ||
            !decoded.decoded_content_hash.valid())
            return fail(OfflineStretchErrorCode::AssetMetadataMismatch, clip.id(), media->asset_id);
        if (!compiled_tempo_map.matches(project.tempo_map().points()))
            return fail(OfflineStretchErrorCode::InvalidTempoSchedule, clip.id());
        if (decoded.audio->num_channels() == 0 ||
            decoded.audio->num_channels() >
                static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
            return fail(OfflineStretchErrorCode::InvalidClip, clip.id(), media->asset_id);
        if (work_block_frames == 0 ||
            work_block_frames > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            algorithm.version == 0 || !std::isfinite(algorithm.max_time_ratio) ||
            algorithm.max_time_ratio < 1.0f)
            return fail(OfflineStretchErrorCode::InvalidAlgorithmConfig, clip.id());
        if (metadata->sample_rate.denominator != 1 ||
            metadata->sample_rate.numerator != decoded.audio->sample_rate ||
            metadata->frame_count != decoded.audio->num_frames())
            return fail(OfflineStretchErrorCode::AssetMetadataMismatch, clip.id(), media->asset_id);
        const auto source_start = static_cast<std::uint64_t>(media->source_start.value);
        if (media->source_start.value < 0 || source_start > decoded.audio->num_frames() ||
            media->frame_count == 0 ||
            media->frame_count > decoded.audio->num_frames() - source_start)
            return fail(OfflineStretchErrorCode::InvalidClip, clip.id(), media->asset_id);
        const auto timeline_rate = compiled_tempo_map.sample_rate().normalized();
        if (!timeline_rate.valid() || timeline_rate.denominator != 1 ||
            timeline_rate.numerator == 0 ||
            timeline_rate.numerator > std::numeric_limits<std::uint32_t>::max())
            return fail(OfflineStretchErrorCode::UnsupportedSampleRate, clip.id(), media->asset_id);
        const auto start_sample = compiled_tempo_map.ticks_to_samples(clip.start()).value;
        const auto end_sample = compiled_tempo_map.ticks_to_samples(clip.end()).value;
        if (end_sample <= start_sample)
            return fail(OfflineStretchErrorCode::InvalidClip, clip.id(), media->asset_id);
        std::uint64_t target = 0;
        if (!detail::offline_stretch_frame_distance(start_sample, end_sample, target))
            return fail(OfflineStretchErrorCode::InvalidClip, clip.id(), media->asset_id);
        const auto scaled = static_cast<long double>(media->frame_count) *
                            timeline_rate.as_long_double() / metadata->sample_rate.as_long_double();
        if (!std::isfinite(scaled) || scaled <= 0.0L ||
            scaled > static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
            return fail(OfflineStretchErrorCode::UnsupportedSampleRate, clip.id(), media->asset_id);
        const auto input_frames = static_cast<std::uint64_t>(std::ceil(scaled));

        const auto tempo_points = project.tempo_map().points();
        if (tempo_points.size() > limits.max_artifact_bytes / sizeof(timebase::TempoPoint))
            return fail(OfflineStretchErrorCode::CapacityExceeded, clip.id());

        key_value = {metadata->content_hash,
                     decoded.decoded_content_hash,
                     source_start,
                     media->frame_count,
                     metadata->sample_rate.normalized(),
                     timeline_rate,
                     clip.start(),
                     clip.end(),
                     input_frames,
                     target,
                     decoded.audio->num_channels(),
                     algorithm,
                     std::vector<timebase::TempoPoint>(tempo_points.begin(), tempo_points.end())};
        std::uint64_t input_bytes = 0;
        std::uint64_t output_bytes = 0;
        std::uint64_t artifact_payload_bytes = 0;
        const auto channels = static_cast<std::uint64_t>(decoded.audio->num_channels());
        if (input_frames > limits.max_input_frames || target > limits.max_output_frames ||
            !checked_payload_bytes(input_frames, channels, sizeof(double), input_bytes) ||
            input_bytes > limits.max_input_bytes ||
            !checked_payload_bytes(target, channels, sizeof(double), output_bytes) ||
            output_bytes > limits.max_output_bytes ||
            !checked_payload_bytes(target, channels, sizeof(float), artifact_payload_bytes) ||
            artifact_payload_bytes > limits.max_artifact_bytes)
            return fail(OfflineStretchErrorCode::CapacityExceeded, clip.id(), media->asset_id);
        if (cache) {
            cache->constrain(limits);
            if (limits.max_cached_artifacts == 0 || artifact_payload_bytes > limits.max_cache_bytes)
                return fail(OfflineStretchErrorCode::CapacityExceeded, clip.id(), media->asset_id);
            artifact = cache->find(key_value);
            if (artifact && artifact_bytes(*artifact) <= limits.max_artifact_bytes) {
                cache_was_hit = true;
                result_status = OfflineStretchCompileStatus::Complete;
                return result_status;
            }
            artifact.reset();
        }

        raw_start = compiled_tempo_map.fractional_ticks_to_samples(
            static_cast<long double>(clip.start().value));
        raw_end = compiled_tempo_map.fractional_ticks_to_samples(
                      static_cast<long double>(clip.end().value)) -
                  raw_start;
        if (!std::isfinite(raw_start) || !std::isfinite(raw_end) || raw_end <= 0.0L)
            return fail(OfflineStretchErrorCode::InvalidTempoSchedule, clip.id());
        audio::FiniteTimeStretchConfig config;
        config.source = decoded.audio;
        config.source_start = source_start;
        config.source_frame_count = media->frame_count;
        config.timeline_sample_rate = static_cast<std::uint32_t>(timeline_rate.numerator);
        config.target_frame_count = target;
        config.max_block_frames = work_block_frames;
        config.max_time_ratio = algorithm.max_time_ratio;
        config.ratio_at_input_frame = &Impl::ratio_at_frame;
        config.ratio_context = this;
        config.max_input_frames = limits.max_input_frames;
        config.max_output_frames = limits.max_output_frames;
        config.max_input_bytes = limits.max_input_bytes;
        config.max_output_bytes = limits.max_output_bytes;
        config.max_artifact_bytes = limits.max_artifact_bytes;
        config.max_sample_rate_converter_bytes = limits.max_sample_rate_converter_bytes;
        config.max_scratch_allocation_bytes = limits.max_scratch_allocation_bytes;
        const auto prepared = stretch.prepare(std::move(config));
        if (prepared != audio::FiniteTimeStretchPrepareStatus::Prepared)
            return fail(map_prepare_error(prepared), clip.id(), media->asset_id);
        result_status = OfflineStretchCompileStatus::Progress;
        return result_status;
    }

    OfflineStretchCompileStatus step() noexcept {
        if (result_status != OfflineStretchCompileStatus::Progress)
            return result_status;
        const auto stepped = stretch.step();
        if (stepped == audio::FiniteTimeStretchStepStatus::Progress)
            return result_status;
        if (stepped == audio::FiniteTimeStretchStepStatus::Failed) {
            const auto failure = stretch.failure();
            return fail(failure == audio::FiniteTimeStretchFailure::None
                            ? map_prepare_error(stretch.prepare_status())
                            : detail::offline_stretch_error_code(failure));
        }
        auto audio = stretch.take();
        if (!audio || audio->sample_rate != key_value.timeline_sample_rate.numerator ||
            audio->num_frames() != key_value.target_frame_count) {
            return fail(OfflineStretchErrorCode::ProcessorProtocolError);
        }
#if defined(__cpp_exceptions)
        try {
#endif
            artifact = std::make_shared<const OfflineStretchArtifact>(
                OfflineStretchArtifact{key_value, std::move(audio)});
#if defined(__cpp_exceptions)
        } catch (const std::bad_alloc&) {
            return fail(OfflineStretchErrorCode::CapacityExceeded);
        } catch (const std::length_error&) {
            return fail(OfflineStretchErrorCode::CapacityExceeded);
        }
#endif
        const auto retained = artifact_bytes(*artifact);
        if (retained > limits.max_artifact_bytes)
            return fail(OfflineStretchErrorCode::CapacityExceeded, {}, {}, retained,
                        limits.max_artifact_bytes);
        // Program compilation supplies its persistent cache, making these
        // count/byte limits the aggregate residency bound for generated audio.
        if (cache && !cache->insert(artifact, limits))
            return fail(OfflineStretchErrorCode::CapacityExceeded, {}, {}, cache->retained_bytes(),
                        limits.max_cache_bytes);
        result_status = OfflineStretchCompileStatus::Complete;
        return result_status;
    }

    OfflineStretchCompileStatus result_status = OfflineStretchCompileStatus::Failed;
    OfflineStretchError error_value;
    bool cache_was_hit = false;
    OfflineStretchArtifactKey key_value;
    OfflineStretchLimits limits;
    OfflineStretchArtifactCache* cache = nullptr;
    const timebase::CompiledTempoMap* tempo_map = nullptr;
    long double raw_start = 0.0L;
    long double raw_end = 0.0L;
    std::uint64_t previous_input_frame = 0;
    long double previous_output_frame = 0.0L;
    float previous_ratio = 1.0f;
    audio::FiniteTimeStretchJob stretch;
    std::shared_ptr<const OfflineStretchArtifact> artifact;
};

OfflineStretchCompileJob::OfflineStretchCompileJob() : impl_(std::make_unique<Impl>()) {}
OfflineStretchCompileJob::~OfflineStretchCompileJob() = default;
OfflineStretchCompileJob::OfflineStretchCompileJob(OfflineStretchCompileJob&&) noexcept = default;
OfflineStretchCompileJob&
OfflineStretchCompileJob::operator=(OfflineStretchCompileJob&&) noexcept = default;

OfflineStretchCompileStatus OfflineStretchCompileJob::begin(
    const timeline::Clip& clip, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map,
    const DecodedAudioAsset& decoded, OfflineStretchLimits limits,
    OfflineStretchAlgorithmConfig algorithm, std::uint32_t work_block_frames,
    OfflineStretchArtifactCache* cache) {
#if defined(__cpp_exceptions)
    try {
#endif
        return impl_->begin(clip, project, tempo_map, decoded, limits, algorithm, work_block_frames,
                            cache);
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return impl_->fail(OfflineStretchErrorCode::CapacityExceeded, clip.id());
    } catch (const std::length_error&) {
        return impl_->fail(OfflineStretchErrorCode::CapacityExceeded, clip.id());
    }
#endif
}

OfflineStretchCompileStatus OfflineStretchCompileJob::step() noexcept {
    return impl_->step();
}
OfflineStretchCompileStatus OfflineStretchCompileJob::status() const noexcept {
    return impl_->result_status;
}
OfflineStretchError OfflineStretchCompileJob::error() const noexcept {
    return impl_->error_value;
}
bool OfflineStretchCompileJob::cache_hit() const noexcept {
    return impl_->cache_was_hit;
}
const OfflineStretchArtifactKey* OfflineStretchCompileJob::key() const noexcept {
    return impl_->result_status == OfflineStretchCompileStatus::Failed ? nullptr
                                                                       : &impl_->key_value;
}
std::shared_ptr<const OfflineStretchArtifact> OfflineStretchCompileJob::take() noexcept {
    return impl_->result_status == OfflineStretchCompileStatus::Complete
               ? std::move(impl_->artifact)
               : nullptr;
}

} // namespace pulp::playback
