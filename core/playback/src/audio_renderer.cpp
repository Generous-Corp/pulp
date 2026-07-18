#include <pulp/playback/audio_renderer.hpp>

#include <pulp/playback/program.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace pulp::playback {
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

bool same_rate(timebase::RationalRate lhs, timebase::RationalRate rhs) noexcept {
    return lhs.normalized() == rhs.normalized();
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

float envelope(const AudioClipRendererProgram& clip, std::uint64_t relative) noexcept {
    float value = clip.gain_linear;
    if (clip.fade_in_frames != 0 && relative < clip.fade_in_frames)
        value *= static_cast<float>(relative) / static_cast<float>(clip.fade_in_frames);
    const auto remaining = clip.timeline_frame_count - 1u - relative;
    if (clip.fade_out_frames != 0 && remaining < clip.fade_out_frames)
        value *= static_cast<float>(remaining) / static_cast<float>(clip.fade_out_frames);
    return value;
}

float source_sample(const audio::AudioFileData& source, std::size_t output_channel,
                    std::size_t output_channels, std::uint64_t frame, std::uint64_t next_frame,
                    float fraction) noexcept {
    const auto source_channels = source.channels.size();
    auto interpolate = [&](std::size_t channel) {
        const auto first = source.channels[channel][frame];
        return first + (source.channels[channel][next_frame] - first) * fraction;
    };
    if (source_channels == 1)
        return interpolate(0);
    if (output_channels == 1) {
        float sum = 0.0f;
        for (std::size_t channel = 0; channel < source_channels; ++channel)
            sum += interpolate(channel);
        return sum / static_cast<float>(source_channels);
    }
    return output_channel < source_channels ? interpolate(output_channel) : 0.0f;
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

std::optional<std::uint64_t> scale_duration_ceil(std::uint64_t value, timebase::RationalRate from,
                                                 timebase::RationalRate to) noexcept {
    const auto scaled =
        static_cast<long double>(value) * to.as_long_double() / from.as_long_double();
    if (!std::isfinite(scaled) || scaled <= 0.0L ||
        scaled > static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
        return std::nullopt;
    return static_cast<std::uint64_t>(std::ceil(scaled));
}

bool valid_transport(const TransportSnapshot& transport, std::size_t output_frames,
                     const timebase::CompiledTempoMap& tempo_map,
                     const AudioRendererLimits& limits) noexcept {
    if (transport.frame_count != output_frames || transport.frame_count == 0 ||
        transport.frame_count > limits.max_block_frames || transport.range_count == 0 ||
        transport.range_count > transport.ranges.size() || transport.tempo_map != &tempo_map ||
        !same_rate(transport.sample_rate, tempo_map.sample_rate()))
        return false;
    std::uint32_t prior_end = 0;
    for (std::uint8_t index = 0; index < transport.range_count; ++index) {
        const auto& range = transport.ranges[index];
        if (range.frame_count == 0 || range.sample_offset != prior_end ||
            range.sample_offset > transport.frame_count ||
            range.frame_count > transport.frame_count - range.sample_offset ||
            !add_fits(range.timeline_sample_start.value, range.frame_count))
            return false;
        prior_end = range.sample_offset + range.frame_count;
    }
    return prior_end == transport.frame_count;
}

void render_track(const AudioTrackRendererProgram& track, const TransportRange& range,
                  audio::BufferView<float> output) noexcept {
    const auto range_start = range.timeline_sample_start.value;
    const auto range_end = range_start + static_cast<std::int64_t>(range.frame_count);
    const auto clips = track.clips();
    auto clip = std::lower_bound(clips.begin(), clips.end(), range_start,
                                 [](const AudioClipRendererProgram& candidate, std::int64_t value) {
                                     return candidate.timeline_end() <= value;
                                 });
    for (; clip != clips.end() && clip->timeline_start < range_end; ++clip) {
        const auto media_end = clip->timeline_start +
                               static_cast<std::int64_t>(std::min(
                                   clip->timeline_frame_count, clip->renderable_timeline_frames));
        const auto overlap_start = std::max(range_start, clip->timeline_start);
        const auto overlap_end = std::min(range_end, media_end);
        if (overlap_end <= overlap_start)
            continue;
        const auto relative_start =
            static_cast<std::uint64_t>(overlap_start - clip->timeline_start);
        const auto output_start = static_cast<std::size_t>(range.sample_offset) +
                                  static_cast<std::size_t>(overlap_start - range_start);
        const auto count = static_cast<std::size_t>(overlap_end - overlap_start);
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel) {
            auto destination = output.channel(channel);
            for (std::size_t frame = 0; frame < count; ++frame) {
                const auto relative = relative_start + frame;
                const auto source_position =
                    static_cast<long double>(relative) *
                    static_cast<long double>(clip->source_frames_per_timeline_frame);
                const auto source_offset = std::min(static_cast<std::uint64_t>(source_position),
                                                    clip->source_frame_count - 1u);
                const auto source_frame = clip->source_start + source_offset;
                const auto source_last = clip->source_start + clip->source_frame_count - 1u;
                const auto next_frame = std::min(source_frame + 1u, source_last);
                const auto fraction =
                    source_offset + 1u < clip->source_frame_count
                        ? static_cast<float>(source_position -
                                             static_cast<long double>(source_offset))
                        : 0.0f;
                destination[output_start + frame] +=
                    source_sample(*clip->audio, channel, output.num_channels(), source_frame,
                                  next_frame, fraction) *
                    envelope(*clip, relative);
            }
        }
    }
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
compile_audio_clip_program(const timeline::Clip& clip, const timeline::Project& project,
                           const timebase::CompiledTempoMap& tempo_map,
                           const DecodedAudioAssetPool& assets, const AudioRendererLimits& limits) {
    const auto* media = std::get_if<timeline::MediaRef>(&clip.content());
    if (!media)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidClipRange, clip.id());
    const auto* metadata = project.find_asset(media->asset_id);
    const auto* decoded = assets.find(media->asset_id);
    if (!metadata || !decoded)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::MissingDecodedAsset,
                                              clip.id(), media->asset_id);
    if (!valid_audio(*decoded->audio, limits))
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidAsset, clip.id(),
                                              media->asset_id);
    if (metadata->frame_count != decoded->audio->num_frames() ||
        metadata->sample_rate.denominator != 1 ||
        metadata->sample_rate.numerator != decoded->audio->sample_rate)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::AssetMetadataMismatch,
                                              clip.id(), media->asset_id);
    if (decoded->audio->sample_rate > 768'000u ||
        tempo_map.sample_rate().as_long_double() > 768'000.0L)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::UnsupportedSampleRate,
                                              clip.id(), media->asset_id);
    const auto source_start = static_cast<std::uint64_t>(media->source_start.value);
    if (source_start > decoded->audio->num_frames() ||
        media->frame_count > decoded->audio->num_frames() - source_start)
        return fail<AudioClipRendererProgram>(AudioRendererErrorCode::InvalidClipRange, clip.id(),
                                              media->asset_id);

    std::int64_t timeline_start = 0;
    std::uint64_t timeline_frames = 0;
    std::uint64_t fade_in_frames = 0;
    std::uint64_t fade_out_frames = 0;
    const auto source_rate = metadata->sample_rate.normalized();
    const auto timeline_rate = tempo_map.sample_rate().normalized();
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
        if (clip.absolute_sample_rate().as_long_double() > 768'000.0L)
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
    return runtime::Ok(AudioClipRendererProgram{
        clip.id(), media->asset_id, decoded->audio, timeline_start, timeline_frames, source_start,
        media->frame_count, *renderable_frames,
        static_cast<double>(source_rate.as_long_double() / timeline_rate.as_long_double()),
        playback.gain_linear, fade_in_frames, fade_out_frames});
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
    std::vector<timeline::ItemId> ids;
    ids.reserve(clips.size());
    for (const auto& clip : clips)
        ids.push_back(clip.id);
    std::sort(ids.begin(), ids.end());
    const auto duplicate = std::adjacent_find(ids.begin(), ids.end());
    if (duplicate != ids.end())
        return fail<std::shared_ptr<const AudioTrackRendererProgram>>(
            AudioRendererErrorCode::InvalidIdentity, *duplicate);
    std::sort(clips.begin(), clips.end(), [](const auto& lhs, const auto& rhs) {
        return std::pair(lhs.timeline_start, lhs.id.value) <
               std::pair(rhs.timeline_start, rhs.id.value);
    });
    return runtime::Ok(std::shared_ptr<const AudioTrackRendererProgram>(
        new AudioTrackRendererProgram(track_id, std::move(clips))));
}

AudioRenderStatus ArrangementAudioRenderer::process(const PlaybackProgram& program,
                                                    const TransportSnapshot& transport,
                                                    audio::BufferView<float> output,
                                                    AudioRendererLimits limits) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    const auto& compiled_limits = program.audio_limits();
    limits.max_channels = std::min(limits.max_channels, compiled_limits.max_channels);
    limits.max_block_frames = std::min(limits.max_block_frames, compiled_limits.max_block_frames);
    limits.max_asset_frames = std::min(limits.max_asset_frames, compiled_limits.max_asset_frames);
    limits.max_tracks = std::min(limits.max_tracks, compiled_limits.max_tracks);
    limits.max_clips = std::min(limits.max_clips, compiled_limits.max_clips);
    if (output.empty())
        return AudioRenderStatus::InvalidOutput;
    if (output.num_channels() > limits.max_channels ||
        output.num_samples() > limits.max_block_frames)
        return AudioRenderStatus::CapacityExceeded;
    output.clear();
    if (!valid_transport(transport, output.num_samples(), program.tempo_map(), limits))
        return AudioRenderStatus::InvalidTransport;
    if (!transport.is_playing)
        return AudioRenderStatus::Silent;

    bool rendered = false;
    if (program.tracks().size() > limits.max_tracks)
        return AudioRenderStatus::CapacityExceeded;

    std::uint64_t total_clips = 0;
    for (const auto& track : program.tracks()) {
        if (!track->audio_program())
            continue;
        if (track->audio_program()->id() != track->id())
            return AudioRenderStatus::InvalidProgram;
        const auto clip_count = static_cast<std::uint64_t>(track->audio_program()->clips().size());
        if (clip_count > limits.max_clips - total_clips)
            return AudioRenderStatus::CapacityExceeded;
        total_clips += clip_count;
    }

    for (const auto& track : program.tracks()) {
        const auto provider = track->provider();
        if (provider.selected != ProviderKind::Arrangement ||
            !provider.available(ProviderKind::Arrangement) || !track->audio_program())
            continue;
        for (std::uint8_t index = 0; index < transport.range_count; ++index)
            render_track(*track->audio_program(), transport.ranges[index], output);
        rendered = rendered || !track->audio_program()->clips().empty();
    }
    return rendered ? AudioRenderStatus::Rendered : AudioRenderStatus::Silent;
}

AudioRenderStatus ArrangementAudioTrackRenderer::process(const PlaybackProgramBlock& block,
                                                         const TransportSnapshot& transport,
                                                         audio::BufferView<float> output,
                                                         AudioRendererLimits limits) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    if (output.empty())
        return AudioRenderStatus::InvalidOutput;
    // Reject caller-bounded shapes before touching memory. The compiled program
    // can only narrow these bounds; that second check below is also performed
    // before clear so CapacityExceeded always leaves the caller's sentinel data
    // intact.
    if (output.num_channels() > limits.max_channels ||
        output.num_samples() > limits.max_block_frames)
        return AudioRenderStatus::CapacityExceeded;

    const auto view = shell_.begin_block(block);
    if (!view.program) {
        output.clear();
        return view.adoption == ShellAdoptionResult::Rejected ? AudioRenderStatus::InvalidProgram
                                                              : AudioRenderStatus::Silent;
    }
    const auto& program = *block.program();
    const auto& compiled_limits = program.audio_limits();
    limits.max_channels = std::min(limits.max_channels, compiled_limits.max_channels);
    limits.max_block_frames = std::min(limits.max_block_frames, compiled_limits.max_block_frames);
    limits.max_asset_frames = std::min(limits.max_asset_frames, compiled_limits.max_asset_frames);
    limits.max_tracks = std::min(limits.max_tracks, compiled_limits.max_tracks);
    limits.max_clips = std::min(limits.max_clips, compiled_limits.max_clips);
    if (output.num_channels() > limits.max_channels ||
        output.num_samples() > limits.max_block_frames)
        return AudioRenderStatus::CapacityExceeded;
    if (!valid_transport(transport, output.num_samples(), program.tempo_map(), limits)) {
        output.clear();
        return AudioRenderStatus::InvalidTransport;
    }
    output.clear();

    const auto provider = view.program->provider();
    const auto* audio_program = view.program->audio_program();
    const bool arrangement_selected = provider.selected == ProviderKind::Arrangement &&
                                      provider.available(ProviderKind::Arrangement);
    AudioRenderStatus status = AudioRenderStatus::Silent;
    if (transport.is_playing && arrangement_selected && audio_program != nullptr) {
        if (audio_program->id() != view.program->id())
            return AudioRenderStatus::InvalidProgram;
        if (audio_program->clips().size() > limits.max_clips)
            return AudioRenderStatus::CapacityExceeded;
        for (std::uint8_t index = 0; index < transport.range_count; ++index)
            render_track(*audio_program, transport.ranges[index], output);
        if (!audio_program->clips().empty())
            status = AudioRenderStatus::Rendered;
    }

    RendererCarryState carry = shell_.state_snapshot();
    carry.key = shell_.active_key();
    carry.active_provider = provider.selected;
    carry.valid = true;
    if (transport.range_count != 0) {
        const auto& last = transport.ranges[transport.range_count - 1];
        carry.source_sample = {last.timeline_sample_start.value +
                               static_cast<std::int64_t>(last.frame_count)};
        carry.timeline_tick = last.timeline_tick_end;
        carry.loop_iteration += transport.range_count > 1 ? 1u : 0u;
    }
    if (!shell_.end_block(carry))
        return AudioRenderStatus::InvalidProgram;
    return status;
}

} // namespace pulp::playback
