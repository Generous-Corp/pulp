#include <pulp/playback/audio_renderer.hpp>

#include "audio_renderer_internal.hpp"

#include <pulp/audio/sample_rate_conversion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <tuple>

namespace pulp::playback {

bool AudioClipConversionArtifact::matches(
    const std::shared_ptr<const audio::AudioFileData>& source, std::uint64_t source_start,
    std::uint64_t source_frames, double source_frames_per_timeline_frame,
    bool requires_host) const noexcept {
    return source_ == source && source_start_ == source_start &&
           source_frames_ == source_frames &&
           source_frames_per_timeline_frame_ == source_frames_per_timeline_frame &&
           (!sample_rate_converter_ || sample_rate_converter_->ready()) &&
           static_cast<bool>(host_rate_converter_) == requires_host &&
           (!host_rate_converter_ ||
            host_rate_converter_->matches_source_slice(source, source_start, source_frames));
}

bool AudioClipRendererProgram::uses_sample_rate_conversion() const noexcept {
    return conversion_artifact && conversion_artifact->sample_rate_converter();
}

bool AudioClipRendererProgram::uses_host_rate_conversion() const noexcept {
    return conversion_artifact && conversion_artifact->host_rate_converter();
}

bool AudioClipRendererProgram::shares_sample_rate_conversion_with(
    const AudioClipRendererProgram& other) const noexcept {
    if (!conversion_artifact || !other.conversion_artifact)
        return false;
    const auto& converter = conversion_artifact->sample_rate_converter();
    return converter && converter == other.conversion_artifact->sample_rate_converter();
}

bool AudioClipRendererProgram::shares_host_rate_conversion_with(
    const AudioClipRendererProgram& other) const noexcept {
    if (!conversion_artifact || !other.conversion_artifact)
        return false;
    const auto& converter = conversion_artifact->host_rate_converter();
    return converter && converter == other.conversion_artifact->host_rate_converter();
}

namespace {

template <typename T>
runtime::Result<T, AudioRendererError> fail(AudioRendererErrorCode code, timeline::ItemId item = {},
                                            timeline::ItemId related = {}, std::uint64_t actual = 0,
                                            std::uint64_t limit = 0) {
    return runtime::Err(AudioRendererError{code, item, related, actual, limit});
}

bool valid_audio(const audio::AudioFileData& value, const AudioRendererLimits& limits) noexcept {
    if (value.sample_rate == 0 || value.channels.empty() ||
        value.channels.size() > limits.max_channels || value.num_frames() == 0 ||
        value.num_frames() > limits.max_asset_frames)
        return false;
    const auto frames = value.channels.front().size();
    return std::all_of(value.channels.begin(), value.channels.end(),
                       [frames](const auto& channel) { return channel.size() == frames; });
}

struct ResolvedAudioMedia {
    const DecodedAudioAsset* decoded = nullptr;
    std::uint64_t source_start = 0;
    timebase::RationalRate source_rate;
};

runtime::Result<ResolvedAudioMedia, AudioRendererError>
resolve_audio_media(timeline::ItemId item, const timeline::MediaRef& media,
                    const timeline::Project& project, const DecodedAudioAssetPool& assets,
                    const AudioRendererLimits& limits,
                    AudioRendererErrorCode range_error = AudioRendererErrorCode::InvalidClipRange) {
    const auto* metadata = project.find_asset(media.asset_id);
    const auto* decoded = assets.find(media.asset_id);
    if (!metadata || !decoded)
        return fail<ResolvedAudioMedia>(AudioRendererErrorCode::MissingDecodedAsset, item,
                                        media.asset_id);
    if (!valid_audio(*decoded->audio, limits))
        return fail<ResolvedAudioMedia>(AudioRendererErrorCode::InvalidAsset, item, media.asset_id);
    if (metadata->frame_count != decoded->audio->num_frames() ||
        metadata->sample_rate.denominator != 1 ||
        metadata->sample_rate.numerator != decoded->audio->sample_rate)
        return fail<ResolvedAudioMedia>(AudioRendererErrorCode::AssetMetadataMismatch, item,
                                        media.asset_id);
    const auto source_start = static_cast<std::uint64_t>(media.source_start.value);
    if (source_start > decoded->audio->num_frames() ||
        media.frame_count > decoded->audio->num_frames() - source_start)
        return fail<ResolvedAudioMedia>(range_error, item, media.asset_id);
    return runtime::Ok(
        ResolvedAudioMedia{decoded, source_start, metadata->sample_rate.normalized()});
}

std::uint64_t positive_distance(std::int64_t start, std::int64_t end) noexcept {
    if (end <= start)
        return 0;
    return static_cast<std::uint64_t>(end) - static_cast<std::uint64_t>(start);
}

bool add_fits(std::int64_t start, std::uint64_t count) noexcept {
    return count <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
           start <= std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(count);
}

std::optional<AudioRendererError>
validate_clip_program(const AudioClipRendererProgram& clip,
                      const AudioRendererLimits& limits) noexcept {
    const auto error = [&clip](AudioRendererErrorCode code, std::uint64_t actual = 0,
                               std::uint64_t limit = 0) {
        return AudioRendererError{code, clip.id, clip.asset_id, actual, limit};
    };
    if (!clip.id.valid() || !clip.asset_id.valid())
        return error(AudioRendererErrorCode::InvalidIdentity);
    if (!clip.audio || clip.audio->sample_rate == 0 || clip.audio->channels.empty() ||
        clip.audio->num_frames() == 0)
        return error(AudioRendererErrorCode::InvalidAsset);
    if (clip.audio->channels.size() > limits.max_channels)
        return error(AudioRendererErrorCode::CapacityExceeded, clip.audio->channels.size(),
                     limits.max_channels);
    if (clip.audio->num_frames() > limits.max_asset_frames)
        return error(AudioRendererErrorCode::CapacityExceeded, clip.audio->num_frames(),
                     limits.max_asset_frames);
    const auto source_frames = clip.audio->num_frames();
    if (!std::all_of(
            clip.audio->channels.begin(), clip.audio->channels.end(),
            [source_frames](const auto& channel) { return channel.size() == source_frames; }))
        return error(AudioRendererErrorCode::InvalidAsset);
    if (clip.timeline_frame_count == 0 || clip.renderable_timeline_frames == 0 ||
        !add_fits(clip.timeline_start, clip.timeline_frame_count) || clip.source_frame_count == 0 ||
        clip.source_start > source_frames ||
        clip.source_frame_count > source_frames - clip.source_start ||
        !std::isfinite(clip.source_frames_per_timeline_frame) ||
        clip.source_frames_per_timeline_frame <= 0.0)
        return error(AudioRendererErrorCode::InvalidClipRange);
    if (!std::isfinite(clip.gain_linear) || clip.gain_linear < 0.0f ||
        clip.fade_in_frames > clip.timeline_frame_count ||
        clip.fade_out_frames > clip.timeline_frame_count)
        return error(AudioRendererErrorCode::InvalidFade);
    if (!clip.conversion_artifact ||
        !clip.conversion_artifact->matches(
            clip.audio, clip.source_start, clip.source_frame_count,
            clip.source_frames_per_timeline_frame,
            clip.time_domain == AudioClipRendererProgram::TimeDomain::Musical))
        return error(AudioRendererErrorCode::InvalidAsset);
    switch (clip.time_domain) {
    case AudioClipRendererProgram::TimeDomain::Musical:
        if (clip.musical_tick_end.value <= clip.musical_tick_start.value)
            return error(AudioRendererErrorCode::InvalidClipRange);
        break;
    case AudioClipRendererProgram::TimeDomain::Absolute:
        if (clip.musical_tick_start.value != 0 || clip.musical_tick_end.value != 0)
            return error(AudioRendererErrorCode::InvalidClipRange);
        break;
    default:
        return error(AudioRendererErrorCode::InvalidClipRange);
    }
    return std::nullopt;
}

std::optional<std::int64_t> scale_position(std::int64_t value, timebase::RationalRate from,
                                           timebase::RationalRate to) noexcept {
    const auto scaled =
        static_cast<long double>(value) * to.as_long_double() / from.as_long_double();
    if (!std::isfinite(scaled) ||
        scaled < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
        scaled > static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
    return static_cast<std::int64_t>(std::floor(scaled));
}

struct WideUnsigned {
    std::uint64_t high = 0;
    std::uint64_t low = 0;
};

WideUnsigned multiply_wide(std::uint64_t lhs, std::uint64_t rhs) noexcept {
    constexpr std::uint64_t mask = std::numeric_limits<std::uint32_t>::max();
    const auto lhs_low = lhs & mask;
    const auto lhs_high = lhs >> 32u;
    const auto rhs_low = rhs & mask;
    const auto rhs_high = rhs >> 32u;
    const auto low_product = lhs_low * rhs_low;
    const auto middle = lhs_high * rhs_low + (low_product >> 32u);
    const auto middle_low = (middle & mask) + lhs_low * rhs_high;
    return {lhs_high * rhs_high + (middle >> 32u) + (middle_low >> 32u),
            (middle_low << 32u) | (low_product & mask)};
}

bool wide_less(WideUnsigned lhs, WideUnsigned rhs) noexcept {
    return lhs.high < rhs.high || (lhs.high == rhs.high && lhs.low < rhs.low);
}

WideUnsigned subtract_wide(WideUnsigned lhs, WideUnsigned rhs) noexcept {
    return {lhs.high - rhs.high - (lhs.low < rhs.low), lhs.low - rhs.low};
}

struct WideDivision {
    WideUnsigned quotient;
    WideUnsigned remainder;
};

WideDivision divide_wide(WideUnsigned numerator, WideUnsigned denominator) noexcept {
    WideDivision result;
    if ((denominator.high & (std::uint64_t{1} << 63u)) != 0) {
        if (!wide_less(numerator, denominator)) {
            result.quotient.low = 1;
            result.remainder = subtract_wide(numerator, denominator);
        } else {
            result.remainder = numerator;
        }
        return result;
    }
    for (int bit = 127; bit >= 0; --bit) {
        const auto input_bit =
            bit >= 64 ? (numerator.high >> (bit - 64)) & 1u : (numerator.low >> bit) & 1u;
        result.remainder = {result.remainder.high << 1u | result.remainder.low >> 63u,
                            result.remainder.low << 1u | input_bit};
        if (!wide_less(result.remainder, denominator)) {
            result.remainder = subtract_wide(result.remainder, denominator);
            if (bit >= 64)
                result.quotient.high |= std::uint64_t{1} << (bit - 64);
            else
                result.quotient.low |= std::uint64_t{1} << bit;
        }
    }
    return result;
}

// Media metadata guarantees an integer source rate. Use a portable 128-bit
// quotient here so large legal timeline positions never pass through floating
// point before the checked floor operation.
std::optional<std::int64_t> scale_media_position(std::int64_t value, timebase::RationalRate from,
                                                 timebase::RationalRate to) noexcept {
    from = from.normalized();
    to = to.normalized();
    if (!from.valid() || !to.valid() || from.denominator != 1)
        return std::nullopt;
    const bool negative = value < 0;
    const auto magnitude = negative ? std::uint64_t{0} - static_cast<std::uint64_t>(value)
                                    : static_cast<std::uint64_t>(value);
    const auto numerator = multiply_wide(magnitude, to.numerator);
    const auto denominator = multiply_wide(from.numerator, to.denominator);
    if (denominator.high == 0 && denominator.low == 0)
        return std::nullopt;
    auto divided = divide_wide(numerator, denominator);
    const bool has_remainder = divided.remainder.high != 0 || divided.remainder.low != 0;
    if (negative && has_remainder) {
        ++divided.quotient.low;
        if (divided.quotient.low == 0)
            ++divided.quotient.high;
    }
    const auto limit = negative
                           ? std::uint64_t{1} << 63u
                           : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (divided.quotient.high != 0 || divided.quotient.low > limit)
        return std::nullopt;
    if (!negative)
        return static_cast<std::int64_t>(divided.quotient.low);
    if (divided.quotient.low == (std::uint64_t{1} << 63u))
        return std::numeric_limits<std::int64_t>::min();
    return -static_cast<std::int64_t>(divided.quotient.low);
}

std::optional<std::uint64_t> scale_duration_ceil(std::uint64_t value, timebase::RationalRate from,
                                                 timebase::RationalRate to) noexcept {
    const auto scaled =
        static_cast<long double>(value) * to.as_long_double() / from.as_long_double();
    if (!std::isfinite(scaled) || scaled <= 0.0L ||
        scaled > static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
        return std::nullopt;
    return static_cast<std::uint64_t>(std::ceil(scaled));
}

} // namespace

runtime::Result<std::shared_ptr<const DecodedAudioAssetPool>, AudioRendererError>
DecodedAudioAssetPool::create(std::vector<DecodedAudioAsset> assets, AudioRendererLimits limits) {
    if (limits.max_channels == 0 || limits.max_asset_frames == 0)
        return fail<std::shared_ptr<const DecodedAudioAssetPool>>(
            AudioRendererErrorCode::CapacityExceeded);
    std::sort(assets.begin(), assets.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.id < rhs.id; });
    for (std::size_t index = 0; index < assets.size(); ++index) {
        const auto& asset = assets[index];
        if (!asset.id.valid())
            return fail<std::shared_ptr<const DecodedAudioAssetPool>>(
                AudioRendererErrorCode::InvalidIdentity, asset.id);
        if (index != 0 && assets[index - 1].id == asset.id)
            return fail<std::shared_ptr<const DecodedAudioAssetPool>>(
                AudioRendererErrorCode::DuplicateAsset, asset.id);
        if (!asset.audio || !valid_audio(*asset.audio, limits))
            return fail<std::shared_ptr<const DecodedAudioAssetPool>>(
                AudioRendererErrorCode::InvalidAsset, asset.id);
    }
    return runtime::Ok(
        std::shared_ptr<const DecodedAudioAssetPool>(new DecodedAudioAssetPool(std::move(assets))));
}

runtime::Result<DecodedAudioAsset, AudioRendererError>
DecodedAudioAssetPool::decode_wav(timeline::ItemId id, std::span<const std::uint8_t> bytes,
                                  audio::WavDecodeLimits decode_limits) {
    if (!id.valid())
        return fail<DecodedAudioAsset>(AudioRendererErrorCode::InvalidIdentity, id);
    auto decoded = audio::decode_wav(bytes, decode_limits);
    if (!decoded)
        return fail<DecodedAudioAsset>(AudioRendererErrorCode::InvalidAsset, id);
    return runtime::Ok(
        DecodedAudioAsset{id, std::make_shared<const audio::AudioFileData>(std::move(*decoded))});
}

const DecodedAudioAsset* DecodedAudioAssetPool::find(timeline::ItemId id) const noexcept {
    const auto found = std::lower_bound(
        assets_.begin(), assets_.end(), id,
        [](const DecodedAudioAsset& asset, timeline::ItemId wanted) { return asset.id < wanted; });
    return found != assets_.end() && found->id == id ? &*found : nullptr;
}

std::int64_t AudioClipRendererProgram::timeline_end() const noexcept {
    return timeline_start + static_cast<std::int64_t>(timeline_frame_count);
}

runtime::Result<AudioClipRendererProgram, AudioRendererError>
detail::compile_audio_clip_program_cached(const timeline::Clip& clip,
                                          const timeline::Project& project,
                                          const timebase::CompiledTempoMap& tempo_map,
                                          const DecodedAudioAssetPool& assets,
                                          const AudioRendererLimits& limits,
                                          AudioSampleRateConverterCache& cache) {
    const auto* media = std::get_if<timeline::MediaRef>(&clip.content());
    if (!media)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidClipRange, clip.id());
    auto resolved = resolve_audio_media(clip.id(), *media, project, assets, limits);
    if (!resolved)
        return runtime::Err(resolved.error());
    if (resolved->decoded->audio->sample_rate > timebase::kMaximumCompiledSampleRate ||
        tempo_map.sample_rate().as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate))
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::UnsupportedSampleRate,
                                              clip.id(), media->asset_id);

    std::int64_t timeline_start = 0;
    std::uint64_t timeline_frames = 0;
    std::uint64_t fade_in_frames = 0;
    std::uint64_t fade_out_frames = 0;
    const auto source_rate = resolved->source_rate;
    const auto timeline_rate = tempo_map.sample_rate().normalized();
    auto converter = cache.get(source_rate, timeline_rate, clip.id(), media->asset_id, limits);
    if (!converter)
        return runtime::Err(converter.error());
    const auto renderable_frames =
        scale_duration_ceil(media->frame_count, source_rate, timeline_rate);
    if (!renderable_frames ||
        *renderable_frames > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::UnsupportedSampleRate,
                                              clip.id(), media->asset_id);
    const auto playback = clip.playback_properties();
    if (clip.time_anchor() == timeline::ClipTimeAnchor::Musical) {
        const auto start = tempo_map.ticks_to_samples(clip.start()).value;
        const auto end = tempo_map.ticks_to_samples(clip.end()).value;
        timeline_start = start;
        timeline_frames = positive_distance(start, end);
        const auto fade_in_tick =
            clip.start() +
            timebase::TickDuration{static_cast<std::int64_t>(playback.fade_in_duration)};
        const auto fade_out_tick =
            clip.end() -
            timebase::TickDuration{static_cast<std::int64_t>(playback.fade_out_duration)};
        fade_in_frames = positive_distance(start, tempo_map.ticks_to_samples(fade_in_tick).value);
        fade_out_frames = positive_distance(tempo_map.ticks_to_samples(fade_out_tick).value, end);
    } else {
        if (clip.absolute_sample_rate().as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate))
            return fail<AudioClipRendererProgram>(AudioRendererErrorCode::UnsupportedSampleRate,
                                                  clip.id(), media->asset_id);
        const auto projected_start =
            scale_position(clip.absolute_start().value, clip.absolute_sample_rate(), timeline_rate);
        const auto projected_end =
            scale_position(clip.absolute_end().value, clip.absolute_sample_rate(), timeline_rate);
        const auto fade_in_position =
            clip.absolute_start().value + static_cast<std::int64_t>(playback.fade_in_duration);
        const auto fade_out_position =
            clip.absolute_end().value - static_cast<std::int64_t>(playback.fade_out_duration);
        const auto projected_fade_in_end =
            scale_position(fade_in_position, clip.absolute_sample_rate(), timeline_rate);
        const auto projected_fade_out_start =
            scale_position(fade_out_position, clip.absolute_sample_rate(), timeline_rate);
        if (!projected_start || !projected_end || !projected_fade_in_end ||
            !projected_fade_out_start || *projected_end < *projected_start ||
            *projected_fade_in_end < *projected_start || *projected_fade_out_start > *projected_end)
            return fail<AudioClipRendererProgram>(AudioRendererErrorCode::UnsupportedSampleRate,
                                                  clip.id(), media->asset_id);
        timeline_start = *projected_start;
        timeline_frames = positive_distance(*projected_start, *projected_end);
        fade_in_frames = positive_distance(*projected_start, *projected_fade_in_end);
        fade_out_frames = positive_distance(*projected_fade_out_start, *projected_end);
    }
    if (timeline_frames == 0 || !add_fits(timeline_start, timeline_frames))
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidClipRange, clip.id(),
                                              media->asset_id);
    if (!std::isfinite(playback.gain_linear) || playback.gain_linear < 0.0f ||
        fade_in_frames > timeline_frames || fade_out_frames > timeline_frames)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidFade, clip.id());
    std::shared_ptr<const audio::PreparedVariableRateConversion> host_rate_converter;
    if (clip.time_anchor() == timeline::ClipTimeAnchor::Musical) {
        auto prepared = cache.get_host(resolved->decoded->audio, resolved->source_start,
                                       media->frame_count, clip.id(), media->asset_id, limits);
        if (!prepared)
            return runtime::Err(prepared.error());
        host_rate_converter = std::move(prepared).value();
    }
    return runtime::Ok(AudioClipRendererProgram{
        clip.id(), media->asset_id, resolved->decoded->audio, timeline_start, timeline_frames,
        resolved->source_start, media->frame_count, *renderable_frames,
        static_cast<double>(source_rate.as_long_double() / timeline_rate.as_long_double()),
        std::make_shared<AudioClipConversionArtifact>(
            resolved->decoded->audio, resolved->source_start, media->frame_count,
            static_cast<double>(source_rate.as_long_double() / timeline_rate.as_long_double()),
            std::move(converter).value(), std::move(host_rate_converter)),
        playback.gain_linear, fade_in_frames, fade_out_frames,
        AudioClipRendererProgram::SourceKind::ArrangementClip, 0,
        clip.time_anchor() == timeline::ClipTimeAnchor::Musical
            ? AudioClipRendererProgram::TimeDomain::Musical
            : AudioClipRendererProgram::TimeDomain::Absolute,
        clip.time_anchor() == timeline::ClipTimeAnchor::Musical ? clip.start()
                                                                : timebase::TickPosition{},
        clip.time_anchor() == timeline::ClipTimeAnchor::Musical ? clip.end()
                                                                : timebase::TickPosition{}});
}

runtime::Result<bool, AudioRendererError> detail::prepare_audio_clip_sample_rate_converters(
    const timeline::Clip& clip, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits, AudioSampleRateConverterCache& cache) {
    const auto* media = std::get_if<timeline::MediaRef>(&clip.content());
    if (!media)
        return runtime::Ok(true);
    auto resolved = resolve_audio_media(clip.id(), *media, project, assets, limits);
    if (!resolved)
        return runtime::Err(resolved.error());
    if (resolved->decoded->audio->sample_rate > timebase::kMaximumCompiledSampleRate ||
        tempo_map.sample_rate().as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate))
        return runtime::Err(AudioRendererError{AudioRendererErrorCode::UnsupportedSampleRate,
                                               clip.id(), media->asset_id});
    auto fixed = cache.prepare(resolved->source_rate, tempo_map.sample_rate(), clip.id(),
                               media->asset_id, limits);
    if (!fixed || !*fixed)
        return fixed;
    if (clip.time_anchor() != timeline::ClipTimeAnchor::Musical)
        return runtime::Ok(true);
    return cache.prepare_host(resolved->decoded->audio, resolved->source_start, media->frame_count,
                              clip.id(), media->asset_id, limits);
}

runtime::Result<bool, AudioRendererError> detail::prepare_take_comp_segment_sample_rate_converter(
    const timeline::TakeLane& lane, std::size_t segment_index, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits, AudioSampleRateConverterCache& cache) {
    const auto segments = lane.comp_segments();
    if (segment_index >= segments.size())
        return fail<bool>(AudioRendererErrorCode::InvalidTakeComp, lane.id());
    const auto* take = lane.find_take(segments[segment_index].take_id);
    if (!take)
        return runtime::Err(AudioRendererError{AudioRendererErrorCode::InvalidTakeComp, lane.id(),
                                               segments[segment_index].take_id});
    auto resolved = resolve_audio_media(take->id(), take->media(), project, assets, limits,
                                        AudioRendererErrorCode::InvalidTakeComp);
    if (!resolved)
        return runtime::Err(resolved.error());
    const auto source_rate = take->sample_rate().normalized();
    const auto timeline_rate = tempo_map.sample_rate().normalized();
    if (source_rate != resolved->source_rate ||
        source_rate.as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate) ||
        timeline_rate.as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate))
        return runtime::Err(AudioRendererError{AudioRendererErrorCode::UnsupportedSampleRate,
                                               take->id(), take->media().asset_id});
    return cache.prepare(source_rate, timeline_rate, take->id(), take->media().asset_id, limits);
}

runtime::Result<bool, AudioRendererError> detail::prepare_track_freeze_sample_rate_converter(
    const timeline::Track& track, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits, AudioSampleRateConverterCache& cache) {
    if (!track.freeze())
        return fail<bool>(AudioRendererErrorCode::InvalidClipRange, track.id());
    auto resolved = resolve_audio_media(track.id(), track.freeze()->media, project, assets, limits);
    if (!resolved)
        return runtime::Err(resolved.error());
    const auto source_rate = track.freeze()->sample_rate.normalized();
    const auto timeline_rate = tempo_map.sample_rate().normalized();
    if (source_rate != resolved->source_rate ||
        source_rate.as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate) ||
        timeline_rate.as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate))
        return runtime::Err(AudioRendererError{AudioRendererErrorCode::UnsupportedSampleRate,
                                               track.id(), track.freeze()->media.asset_id});
    return cache.prepare(source_rate, timeline_rate, track.id(), track.freeze()->media.asset_id,
                         limits);
}

runtime::Result<AudioClipRendererProgram, AudioRendererError> compile_take_comp_segment_program(
    const timeline::TakeLane& lane, std::size_t segment_index, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits) {
    detail::AudioSampleRateConverterCache cache;
    return detail::compile_take_comp_segment_program_cached(lane, segment_index, project, tempo_map,
                                                            assets, limits, cache);
}

runtime::Result<AudioClipRendererProgram, AudioRendererError>
compile_audio_clip_program(const timeline::Clip& clip, const timeline::Project& project,
                           const timebase::CompiledTempoMap& tempo_map,
                           const DecodedAudioAssetPool& assets, const AudioRendererLimits& limits) {
    detail::AudioSampleRateConverterCache cache;
    return detail::compile_audio_clip_program_cached(clip, project, tempo_map, assets, limits,
                                                     cache);
}

runtime::Result<AudioClipRendererProgram, AudioRendererError>
detail::compile_take_comp_segment_program_cached(
    const timeline::TakeLane& lane, std::size_t segment_index, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits, AudioSampleRateConverterCache& cache) {
    const auto segments = lane.comp_segments();
    if (segment_index >= segments.size() ||
        segment_index >= std::numeric_limits<std::uint32_t>::max())
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidTakeComp, lane.id());
    const auto& segment = segments[segment_index];
    const auto* take = lane.find_take(segment.take_id);
    if (!take)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidTakeComp, lane.id(),
                                              segment.take_id);
    auto resolved = resolve_audio_media(take->id(), take->media(), project, assets, limits,
                                        AudioRendererErrorCode::InvalidTakeComp);
    if (!resolved)
        return runtime::Err(resolved.error());
    const auto source_rate = take->sample_rate().normalized();
    const auto timeline_rate = tempo_map.sample_rate().normalized();
    if (source_rate != resolved->source_rate ||
        source_rate.as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate) ||
        timeline_rate.as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate))
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::UnsupportedSampleRate,
                                              take->id(), take->media().asset_id);
    auto converter =
        cache.get(source_rate, timeline_rate, take->id(), take->media().asset_id, limits);
    if (!converter)
        return runtime::Err(converter.error());
    const auto offset =
        static_cast<std::uint64_t>(segment.range.start.value - take->placement_start().value);
    if (offset > take->media().frame_count ||
        segment.range.sample_count > take->media().frame_count - offset)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidTakeComp, lane.id(),
                                              take->id());
    const auto end =
        segment.range.start.value + static_cast<std::int64_t>(segment.range.sample_count);
    const auto projected_start =
        scale_media_position(segment.range.start.value, source_rate, timeline_rate);
    const auto projected_end = scale_media_position(end, source_rate, timeline_rate);
    if (!projected_start || !projected_end || *projected_end <= *projected_start)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::UnsupportedSampleRate,
                                              take->id(), take->media().asset_id);
    const auto timeline_frames = positive_distance(*projected_start, *projected_end);
    if (!add_fits(*projected_start, timeline_frames))
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidTakeComp, lane.id(),
                                              take->id());
    return runtime::Ok(AudioClipRendererProgram{
        take->id(),
        take->media().asset_id,
        resolved->decoded->audio,
        *projected_start,
        timeline_frames,
        resolved->source_start + offset,
        segment.range.sample_count,
        timeline_frames,
        static_cast<double>(source_rate.as_long_double() / timeline_rate.as_long_double()),
        std::make_shared<AudioClipConversionArtifact>(
            resolved->decoded->audio, resolved->source_start + offset,
            segment.range.sample_count,
            static_cast<double>(source_rate.as_long_double() / timeline_rate.as_long_double()),
            std::move(converter).value(), nullptr),
        1.0f,
        0,
        0,
        AudioClipRendererProgram::SourceKind::TakeCompSegment,
        static_cast<std::uint32_t>(segment_index + 1),
        AudioClipRendererProgram::TimeDomain::Absolute,
        {},
        {},
    });
}

runtime::Result<AudioClipRendererProgram, AudioRendererError>
detail::compile_track_freeze_program_cached(const timeline::Track& track,
                                            const timeline::Project& project,
                                            const timebase::CompiledTempoMap& tempo_map,
                                            const DecodedAudioAssetPool& assets,
                                            const AudioRendererLimits& limits,
                                            AudioSampleRateConverterCache& cache) {
    if (!track.freeze())
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidClipRange, track.id());
    const auto& freeze = *track.freeze();
    auto resolved = resolve_audio_media(track.id(), freeze.media, project, assets, limits);
    if (!resolved)
        return runtime::Err(resolved.error());
    const auto source_rate = freeze.sample_rate.normalized();
    const auto timeline_rate = tempo_map.sample_rate().normalized();
    if (source_rate != resolved->source_rate ||
        source_rate.as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate) ||
        timeline_rate.as_long_double() >
            static_cast<long double>(timebase::kMaximumCompiledSampleRate))
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::UnsupportedSampleRate,
                                              track.id(), freeze.media.asset_id);
    auto converter =
        cache.get(source_rate, timeline_rate, track.id(), freeze.media.asset_id, limits);
    if (!converter)
        return runtime::Err(converter.error());
    if (freeze.media.frame_count >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidClipRange, track.id(),
                                              freeze.media.asset_id);
    const auto signed_frames = static_cast<std::int64_t>(freeze.media.frame_count);
    if (freeze.placement_start.value > std::numeric_limits<std::int64_t>::max() - signed_frames)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidClipRange, track.id(),
                                              freeze.media.asset_id);
    const auto end = freeze.placement_start.value + signed_frames;
    const auto projected_start =
        scale_media_position(freeze.placement_start.value, source_rate, timeline_rate);
    const auto projected_end = scale_media_position(end, source_rate, timeline_rate);
    if (!projected_start || !projected_end || *projected_end <= *projected_start)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::UnsupportedSampleRate,
                                              track.id(), freeze.media.asset_id);
    const auto timeline_frames = positive_distance(*projected_start, *projected_end);
    if (!add_fits(*projected_start, timeline_frames))
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidClipRange, track.id(),
                                              freeze.media.asset_id);
    return runtime::Ok(AudioClipRendererProgram{
        track.id(),
        freeze.media.asset_id,
        resolved->decoded->audio,
        *projected_start,
        timeline_frames,
        resolved->source_start,
        freeze.media.frame_count,
        timeline_frames,
        static_cast<double>(source_rate.as_long_double() / timeline_rate.as_long_double()),
        std::make_shared<AudioClipConversionArtifact>(
            resolved->decoded->audio, resolved->source_start, freeze.media.frame_count,
            static_cast<double>(source_rate.as_long_double() / timeline_rate.as_long_double()),
            std::move(converter).value(), nullptr),
        1.0f,
        0,
        0,
        AudioClipRendererProgram::SourceKind::FrozenTrack,
        0,
        AudioClipRendererProgram::TimeDomain::Absolute,
        {},
        {},
    });
}

runtime::Result<AudioClipRendererProgram, AudioRendererError>
compile_track_freeze_program(const timeline::Track& track, const timeline::Project& project,
                             const timebase::CompiledTempoMap& tempo_map,
                             const DecodedAudioAssetPool& assets,
                             const AudioRendererLimits& limits) {
    detail::AudioSampleRateConverterCache cache;
    return detail::compile_track_freeze_program_cached(track, project, tempo_map, assets, limits,
                                                       cache);
}

runtime::Result<std::shared_ptr<const AudioTrackRendererProgram>, AudioRendererError>
link_audio_track_program(timeline::ItemId track_id, std::vector<AudioClipRendererProgram> clips,
                         const AudioRendererLimits& limits) {
    if (!track_id.valid())
        return fail<std::shared_ptr<const AudioTrackRendererProgram>>(
            AudioRendererErrorCode::InvalidIdentity, track_id);
    if (clips.size() > limits.max_clips)
        return fail<std::shared_ptr<const AudioTrackRendererProgram>>(
            AudioRendererErrorCode::CapacityExceeded, track_id, {}, clips.size(), limits.max_clips);
    using Key = std::tuple<AudioClipRendererProgram::SourceKind, std::uint64_t>;
    std::vector<Key> ids;
    ids.reserve(clips.size());
    for (const auto& clip : clips) {
        if (const auto invalid = validate_clip_program(clip, limits))
            return runtime::Err(*invalid);
        std::uint64_t source_identity = 0;
        switch (clip.source_kind) {
        case AudioClipRendererProgram::SourceKind::ArrangementClip:
            if (clip.source_ordinal != 0)
                return fail<std::shared_ptr<const AudioTrackRendererProgram>>(
                    AudioRendererErrorCode::InvalidIdentity, clip.id);
            source_identity = clip.id.value;
            break;
        case AudioClipRendererProgram::SourceKind::TakeCompSegment:
            if (clip.source_ordinal == 0)
                return fail<std::shared_ptr<const AudioTrackRendererProgram>>(
                    AudioRendererErrorCode::InvalidIdentity, clip.id);
            source_identity = clip.source_ordinal;
            break;
        case AudioClipRendererProgram::SourceKind::FrozenTrack:
            if (clip.source_ordinal != 0 || clip.id != track_id)
                return fail<std::shared_ptr<const AudioTrackRendererProgram>>(
                    AudioRendererErrorCode::InvalidIdentity, clip.id);
            source_identity = clip.id.value;
            break;
        default:
            return fail<std::shared_ptr<const AudioTrackRendererProgram>>(
                AudioRendererErrorCode::InvalidIdentity, clip.id);
        }
        ids.emplace_back(clip.source_kind, source_identity);
    }
    std::sort(ids.begin(), ids.end());
    const auto duplicate = std::adjacent_find(ids.begin(), ids.end());
    if (duplicate != ids.end())
        return fail<std::shared_ptr<const AudioTrackRendererProgram>>(
            AudioRendererErrorCode::InvalidIdentity,
            std::get<0>(*duplicate) == AudioClipRendererProgram::SourceKind::ArrangementClip
                ? timeline::ItemId{std::get<1>(*duplicate)}
                : track_id);
    std::sort(clips.begin(), clips.end(), [](const auto& lhs, const auto& rhs) {
        return std::tuple(lhs.timeline_start, lhs.source_kind, lhs.id.value, lhs.source_ordinal) <
               std::tuple(rhs.timeline_start, rhs.source_kind, rhs.id.value, rhs.source_ordinal);
    });
    return runtime::Ok(std::shared_ptr<const AudioTrackRendererProgram>(
        new AudioTrackRendererProgram(track_id, std::move(clips))));
}

} // namespace pulp::playback
