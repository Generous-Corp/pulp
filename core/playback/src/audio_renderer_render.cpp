#include <pulp/playback/audio_renderer.hpp>

#include "audio_renderer_internal.hpp"

#include <pulp/audio/sample_rate_conversion.hpp>
#include <pulp/playback/program.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace pulp::playback {
namespace {

bool same_rate(timebase::RationalRate lhs, timebase::RationalRate rhs) noexcept {
    return lhs.normalized() == rhs.normalized();
}

bool transport_range_add_fits(std::int64_t start, std::uint64_t count) noexcept {
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

float envelope(const AudioClipRendererProgram& clip, long double relative) noexcept {
    auto value = static_cast<long double>(clip.gain_linear);
    if (clip.fade_in_frames != 0 && relative < clip.fade_in_frames)
        value *= relative / static_cast<long double>(clip.fade_in_frames);
    const auto remaining =
        std::max(0.0L, static_cast<long double>(clip.timeline_frame_count - 1u) - relative);
    if (clip.fade_out_frames != 0 && remaining < clip.fade_out_frames)
        value *= remaining / static_cast<long double>(clip.fade_out_frames);
    return static_cast<float>(value);
}

float source_sample(
    const AudioClipRendererProgram& clip, std::size_t output_channel, std::size_t output_channels,
    std::uint64_t frame, std::uint64_t next_frame, float fraction, double source_position,
    std::optional<double> host_source_frames_per_output_frame = std::nullopt) noexcept {
    const auto& source = *clip.audio;
    const auto source_channels = source.channels.size();
    auto sample = [&](std::size_t channel) {
        const auto segment = std::span<const float>(source.channels[channel])
                                 .subspan(static_cast<std::size_t>(clip.source_start),
                                          static_cast<std::size_t>(clip.source_frame_count));
        const auto& host_rate_converter =
            clip.conversion_artifact->host_rate_converter();
        const auto& sample_rate_converter =
            clip.conversion_artifact->sample_rate_converter();
        if (host_rate_converter && host_source_frames_per_output_frame &&
            (*host_source_frames_per_output_frame > 1.0 ||
             clip.source_frames_per_timeline_frame > 1.0))
            return host_rate_converter->read(channel, source_position,
                                             *host_source_frames_per_output_frame == 0.0
                                                 ? 1.0
                                                 : *host_source_frames_per_output_frame);
        if (sample_rate_converter) {
            return sample_rate_converter->read(segment, source_position);
        }
        const auto first = source.channels[channel][frame];
        return first + (source.channels[channel][next_frame] - first) * fraction;
    };
    if (source_channels == 1)
        return sample(0);
    if (output_channels == 1) {
        float sum = 0.0f;
        for (std::size_t channel = 0; channel < source_channels; ++channel)
            sum += sample(channel);
        return sum / static_cast<float>(source_channels);
    }
    return output_channel < source_channels ? sample(output_channel) : 0.0f;
}

bool valid_transport(const TransportSnapshot& transport, std::size_t output_frames,
                     const timebase::CompiledTempoMap& tempo_map,
                     const AudioRendererLimits& limits) noexcept {
    if (!valid_transport_ranges(transport) || transport.frame_count != output_frames ||
        transport.frame_count > limits.max_block_frames || transport.tempo_map != &tempo_map ||
        !same_rate(transport.sample_rate, tempo_map.sample_rate()))
        return false;
    std::uint32_t prior_end = 0;
    for (std::uint8_t index = 0; index < transport.range_count; ++index) {
        const auto& range = transport.ranges[index];
        if (range.frame_count == 0 || range.sample_offset != prior_end ||
            range.sample_offset > transport.frame_count ||
            range.frame_count > transport.frame_count - range.sample_offset ||
            !transport_range_add_fits(range.timeline_sample_start.value, range.frame_count))
            return false;
        prior_end = range.sample_offset + range.frame_count;
    }
    return prior_end == transport.frame_count;
}

// One track's mixer resolved for a render pass. Held by value on the stack so
// the audio thread never touches shared cursor state, and skipped wholesale when
// the track was never touched — that is what keeps an untouched track's output
// bit-identical to what it was before mixer state existed.
struct TrackMixerRun {
    TrackMixerControlCursor gain;
    TrackMixerControlCursor pan;
    bool transparent = true;

    void reset(const TrackMixerProgram& mixer) noexcept {
        transparent = mixer.transparent();
        gain.reset(mixer.gain_automation, mixer.gain_linear);
        pan.reset(mixer.pan_automation, mixer.pan);
    }

    void restart(const timebase::CompiledTempoMap& map) noexcept {
        gain.restart(map);
        pan.restart(map);
    }

    float factor(timebase::SamplePosition sample, std::size_t channel,
                 std::size_t channel_count) noexcept {
        return clamped_track_gain(gain.value_at(sample)) *
               track_mixer_channel_gain(clamped_track_pan(pan.value_at(sample)), channel,
                                        channel_count);
    }

    float factor_at_tick(long double tick, std::size_t channel,
                         std::size_t channel_count) noexcept {
        return clamped_track_gain(gain.value_at_tick(tick)) *
               track_mixer_channel_gain(clamped_track_pan(pan.value_at_tick(tick)), channel,
                                        channel_count);
    }
};

long double musical_phase_source_position(const AudioClipRendererProgram& clip,
                                          long double document_tick) noexcept {
    const auto tick_start = static_cast<long double>(clip.musical_tick_start.value);
    const auto tick_span = static_cast<long double>(clip.musical_tick_end.value) - tick_start;
    const auto phase = std::clamp((document_tick - tick_start) / tick_span, 0.0L, 1.0L);
    return static_cast<long double>(clip.source_frame_offset) +
           phase * (static_cast<long double>(clip.source_frame_count) -
                    static_cast<long double>(clip.source_frame_offset));
}

struct SourceReadPoint {
    long double position = 0.0L;
    double filter_step = 0.0;
};

SourceReadPoint source_read_point(const AudioClipRendererProgram& clip,
                                  long double document_position,
                                  long double next_document_position,
                                  long double document_tick,
                                  long double next_document_tick,
                                  long double clip_start_position) noexcept {
    if (clip.source_time_mapping ==
        AudioClipRendererProgram::SourceTimeMapping::MusicalPhaseResample) {
        const auto position = musical_phase_source_position(clip, document_tick);
        return {position, static_cast<double>(
                              std::abs(musical_phase_source_position(clip, next_document_tick) -
                                       position))};
    }
    const auto position = static_cast<long double>(clip.source_frame_offset) +
                          (document_position - clip_start_position) *
                              static_cast<long double>(clip.source_frames_per_timeline_frame);
    return {position,
            static_cast<double>(std::abs(next_document_position - document_position) *
                                static_cast<long double>(
                                    clip.source_frames_per_timeline_frame))};
}

bool supported_source_step(double step) noexcept {
    return std::isfinite(step) && step >= 0.0 &&
           step <= audio::PreparedVariableRateConversion::
                       kMaximumSourceFramesPerOutputFrame;
}

bool preflight_host_source_steps(const AudioTrackRendererProgram& track,
                                 const TransportRange& range,
                                 const timebase::CompiledTempoMap& tempo_map) noexcept {
    const auto clips = track.clips();
    const auto range_tick_start =
        range.has_precise_host_ticks ? static_cast<long double>(range.host_tick_start)
                                     : static_cast<long double>(range.timeline_tick_start.value);
    const auto range_tick_end =
        range.has_precise_host_ticks ? static_cast<long double>(range.host_tick_end)
                                     : static_cast<long double>(range.timeline_tick_end.value);
    timebase::TempoTickCursor position_cursor(tempo_map);
    timebase::TempoTickCursor clip_start_cursor(tempo_map);
    auto first_clip = clips.begin();
    auto document_tick = range_tick_start;
    auto document_position = position_cursor.advance_fractional(document_tick);
    for (std::uint32_t output_frame = 0; output_frame < range.frame_count; ++output_frame) {
        const auto next_document_tick =
            range_tick_start +
            (range_tick_end - range_tick_start) *
                (static_cast<long double>(output_frame + 1u) /
                 static_cast<long double>(range.frame_count));
        const auto next_document_position =
            position_cursor.advance_fractional(next_document_tick);
        while (first_clip != clips.end()) {
            const auto ended =
                first_clip->time_domain == AudioClipRendererProgram::TimeDomain::Musical
                    ? static_cast<long double>(first_clip->musical_tick_end.value) <= document_tick
                    : static_cast<long double>(first_clip->timeline_end()) <= document_position;
            if (!ended)
                break;
            ++first_clip;
        }
        auto clip = first_clip;
        for (; clip != clips.end(); ++clip) {
            if (clip->time_domain != AudioClipRendererProgram::TimeDomain::Musical)
                continue;
            const auto clip_tick_start = static_cast<long double>(clip->musical_tick_start.value);
            const auto clip_tick_end = static_cast<long double>(clip->musical_tick_end.value);
            if (document_tick < clip_tick_start)
                break;
            if (!(document_tick < clip_tick_end))
                continue;
            const auto clip_start_position =
                clip_start_cursor.advance_fractional(clip_tick_start);
            const auto relative = document_position - clip_start_position;
            const auto musical_phase_mapping =
                clip->source_time_mapping ==
                AudioClipRendererProgram::SourceTimeMapping::MusicalPhaseResample;
            if (relative < 0.0L ||
                (!musical_phase_mapping &&
                 relative >= static_cast<long double>(clip->renderable_timeline_frames)))
                continue;
            const auto read_point =
                source_read_point(*clip, document_position, next_document_position, document_tick,
                                  next_document_tick, clip_start_position);
            if (!supported_source_step(read_point.filter_step))
                return false;
        }
        document_tick = next_document_tick;
        document_position = next_document_position;
    }
    return true;
}

bool preflight_source_steps(const AudioTrackRendererProgram& track,
                            const TransportSnapshot& transport,
                            const timebase::CompiledTempoMap& tempo_map) noexcept {
    for (std::uint8_t index = 0; index < transport.range_count; ++index) {
        const auto& range = transport.ranges[index];
        if (range.host_beat_mapping &&
            !preflight_host_source_steps(track, range, tempo_map))
            return false;
    }
    return true;
}

void render_track(const AudioTrackRendererProgram& track, const TransportRange& range,
                  const timebase::CompiledTempoMap& tempo_map, TrackMixerRun& mixer,
                  audio::BufferView<float> output, bool absolute_only = false,
                  bool host_mixer_mapping = false) noexcept {
    const auto range_start = range.timeline_sample_start.value;
    const auto range_end = range_start + static_cast<std::int64_t>(range.frame_count);
    const auto clips = track.clips();
    auto clip = std::lower_bound(clips.begin(), clips.end(), range_start,
                                 [](const AudioClipRendererProgram& candidate, std::int64_t value) {
                                     return candidate.timeline_end() <= value;
                                 });
    for (; clip != clips.end() && clip->timeline_start < range_end; ++clip) {
        if (absolute_only && clip->time_domain != AudioClipRendererProgram::TimeDomain::Absolute)
            continue;
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
        const auto musical_phase_mapping =
            clip->source_time_mapping ==
            AudioClipRendererProgram::SourceTimeMapping::MusicalPhaseResample;
        if (!mixer.transparent)
            mixer.restart(tempo_map);
        timebase::TempoCursor phase_cursor(tempo_map);
        for (std::size_t frame = 0; frame < count; ++frame) {
            const auto relative = relative_start + frame;
            SourceReadPoint read_point;
            long double source_position =
                static_cast<long double>(clip->source_frame_offset) +
                static_cast<long double>(relative) *
                    static_cast<long double>(clip->source_frames_per_timeline_frame);
            if (musical_phase_mapping) {
                const auto document_position = static_cast<long double>(
                    overlap_start + static_cast<std::int64_t>(frame));
                const auto document_tick = phase_cursor.advance_fractional(document_position);
                const auto next_document_tick =
                    phase_cursor.advance_fractional(document_position + 1.0L);
                read_point = source_read_point(
                    *clip, document_position, document_position + 1.0L, document_tick,
                    next_document_tick, static_cast<long double>(clip->timeline_start));
                source_position = read_point.position;
            }
            const auto source_offset = std::min(static_cast<std::uint64_t>(source_position),
                                                clip->source_frame_count - 1u);
            const auto source_frame = clip->source_start + source_offset;
            const auto source_last = clip->source_start + clip->source_frame_count - 1u;
            const auto next_frame = std::min(source_frame + 1u, source_last);
            const auto fraction = source_offset + 1u < clip->source_frame_count
                                      ? static_cast<float>(
                                            source_position - static_cast<long double>(source_offset))
                                      : 0.0f;
            for (std::size_t channel = 0; channel < output.num_channels(); ++channel) {
                float mixer_factor = 1.0f;
                if (!mixer.transparent) {
                    if (host_mixer_mapping) {
                        const auto output_frame =
                            static_cast<std::uint64_t>(overlap_start - range_start) + frame;
                        const auto host_fraction = static_cast<long double>(output_frame) /
                                                   static_cast<long double>(range.frame_count);
                        const auto tick_start =
                            range.has_precise_host_ticks
                                ? static_cast<long double>(range.host_tick_start)
                                : static_cast<long double>(range.timeline_tick_start.value);
                        const auto tick_end =
                            range.has_precise_host_ticks
                                ? static_cast<long double>(range.host_tick_end)
                                : static_cast<long double>(range.timeline_tick_end.value);
                        mixer_factor = mixer.factor_at_tick(tick_start + (tick_end - tick_start) *
                                                                             host_fraction,
                                                            channel, output.num_channels());
                    } else {
                        mixer_factor =
                            mixer.factor(timebase::SamplePosition{overlap_start +
                                                                  static_cast<std::int64_t>(frame)},
                                         channel, output.num_channels());
                    }
                }
                output.channel(channel)[output_start + frame] +=
                    source_sample(*clip, channel, output.num_channels(), source_frame, next_frame,
                                  fraction, static_cast<double>(source_position),
                                  musical_phase_mapping
                                      ? std::optional<double>{read_point.filter_step}
                                      : std::nullopt) *
                    envelope(*clip, relative) * mixer_factor;
            }
        }
    }
}

void render_host_beat_mapped_track(const AudioTrackRendererProgram& track,
                                   const TransportRange& range,
                                   const timebase::CompiledTempoMap& tempo_map,
                                   TrackMixerRun& mixer, audio::BufferView<float> output) noexcept {
    render_track(track, range, tempo_map, mixer, output, true, true);
    // The absolute pass above left the cursors wherever its last clip ended, and
    // this pass starts over at the head of the range.
    if (!mixer.transparent)
        mixer.restart(tempo_map);
    const auto clips = track.clips();
    for (std::uint32_t output_frame = 0; output_frame < range.frame_count; ++output_frame) {
        const auto fraction =
            static_cast<long double>(output_frame) / static_cast<long double>(range.frame_count);
        const auto range_tick_start =
            range.has_precise_host_ticks
                ? static_cast<long double>(range.host_tick_start)
                : static_cast<long double>(range.timeline_tick_start.value);
        const auto range_tick_end = range.has_precise_host_ticks
                                        ? static_cast<long double>(range.host_tick_end)
                                        : static_cast<long double>(range.timeline_tick_end.value);
        const auto document_tick =
            range_tick_start + (range_tick_end - range_tick_start) * fraction;
        const auto document_position = tempo_map.fractional_ticks_to_samples(document_tick);
        auto clip = std::lower_bound(
            clips.begin(), clips.end(), document_position,
            [&tempo_map](const AudioClipRendererProgram& candidate, long double position) {
                const auto end =
                    candidate.time_domain == AudioClipRendererProgram::TimeDomain::Musical
                        ? tempo_map.fractional_ticks_to_samples(
                              static_cast<long double>(candidate.musical_tick_end.value))
                        : static_cast<long double>(candidate.timeline_end());
                return end <= position;
            });
        for (; clip != clips.end(); ++clip) {
            if (clip->time_domain != AudioClipRendererProgram::TimeDomain::Musical)
                continue;
            const auto clip_tick_start = static_cast<long double>(clip->musical_tick_start.value);
            const auto clip_tick_end = static_cast<long double>(clip->musical_tick_end.value);
            if (document_tick < clip_tick_start)
                break;
            if (!(document_tick < clip_tick_end))
                continue;
            const auto clip_start_position = tempo_map.fractional_ticks_to_samples(clip_tick_start);
            const auto relative = document_position - clip_start_position;
            const auto musical_phase_mapping =
                clip->source_time_mapping ==
                AudioClipRendererProgram::SourceTimeMapping::MusicalPhaseResample;
            if (relative < 0.0L ||
                (!musical_phase_mapping &&
                 relative >= static_cast<long double>(clip->renderable_timeline_frames)))
                continue;
            const auto next_document_position =
                host_mapped_document_sample_at_output_offset(range, tempo_map, output_frame + 1u);
            const auto next_document_tick =
                range_tick_start +
                (range_tick_end - range_tick_start) * (static_cast<long double>(output_frame + 1u) /
                                                       static_cast<long double>(range.frame_count));
            const auto read_point =
                source_read_point(*clip, document_position, next_document_position, document_tick,
                                  next_document_tick, clip_start_position);
            const auto source_position = read_point.position;
            const auto source_offset =
                std::min(static_cast<std::uint64_t>(std::floor(source_position)),
                         clip->source_frame_count - 1u);
            const auto source_frame = clip->source_start + source_offset;
            const auto source_last = clip->source_start + clip->source_frame_count - 1u;
            const auto next_frame = std::min(source_frame + 1u, source_last);
            const auto fraction =
                source_offset + 1u < clip->source_frame_count
                    ? static_cast<float>(source_position - static_cast<long double>(source_offset))
                    : 0.0f;
            const auto destination_frame =
                static_cast<std::size_t>(range.sample_offset + output_frame);
            for (std::size_t channel = 0; channel < output.num_channels(); ++channel) {
                const auto mixer_factor =
                    mixer.transparent
                        ? 1.0f
                        : mixer.factor_at_tick(document_tick, channel, output.num_channels());
                output.channel(channel)[destination_frame] +=
                    source_sample(*clip, channel, output.num_channels(), source_frame, next_frame,
                                  fraction, static_cast<double>(source_position),
                                  read_point.filter_step) *
                    envelope(*clip, relative) * mixer_factor;
            }
        }
    }
}

AudioRendererLimits narrowed_limits(AudioRendererLimits requested,
                                    const AudioRendererLimits& compiled) noexcept {
    requested.max_channels = std::min(requested.max_channels, compiled.max_channels);
    requested.max_block_frames = std::min(requested.max_block_frames, compiled.max_block_frames);
    requested.max_asset_frames = std::min(requested.max_asset_frames, compiled.max_asset_frames);
    requested.max_tracks = std::min(requested.max_tracks, compiled.max_tracks);
    requested.max_clips = std::min(requested.max_clips, compiled.max_clips);
    return requested;
}

std::optional<AudioRenderStatus> invalid_audio_program(const AudioTrackRendererProgram& program,
                                                       timeline::ItemId expected_track,
                                                       std::uint64_t clip_capacity) noexcept {
    if (program.id() != expected_track)
        return AudioRenderStatus::InvalidProgram;
    if (program.clips().size() > clip_capacity)
        return AudioRenderStatus::CapacityExceeded;
    return std::nullopt;
}

AudioRenderStatus render_audio_program(const AudioTrackRendererProgram& program,
                                       const TrackMixerProgram& mixer,
                                       const TransportSnapshot& transport,
                                       const timebase::CompiledTempoMap& tempo_map,
                                       audio::BufferView<float> output) noexcept {
    TrackMixerRun run;
    run.reset(mixer);
    for (std::uint8_t index = 0; index < transport.range_count; ++index) {
        const auto& range = transport.ranges[index];
        if (!run.transparent)
            run.restart(tempo_map);
        if (range.host_beat_mapping)
            render_host_beat_mapped_track(program, range, tempo_map, run, output);
        else
            render_track(program, range, tempo_map, run, output);
    }
    return program.clips().empty() ? AudioRenderStatus::Silent : AudioRenderStatus::Rendered;
}

void apply_track_mixer(const TrackMixerProgram& mixer, const TransportSnapshot& transport,
                       const timebase::CompiledTempoMap& tempo_map,
                       audio::BufferView<float> output) noexcept {
    TrackMixerRun run;
    run.reset(mixer);
    if (run.transparent)
        return;
    for (std::size_t channel = 0; channel < output.num_channels(); ++channel) {
        for (std::uint8_t range_index = 0; range_index < transport.range_count; ++range_index) {
            const auto& range = transport.ranges[range_index];
            run.restart(tempo_map);
            for (std::uint32_t frame = 0; frame < range.frame_count; ++frame) {
                float factor = 1.0f;
                if (range.host_beat_mapping) {
                    const auto fraction = static_cast<long double>(frame) /
                                          static_cast<long double>(range.frame_count);
                    const auto start =
                        range.has_precise_host_ticks
                            ? static_cast<long double>(range.host_tick_start)
                            : static_cast<long double>(range.timeline_tick_start.value);
                    const auto end = range.has_precise_host_ticks
                                         ? static_cast<long double>(range.host_tick_end)
                                         : static_cast<long double>(range.timeline_tick_end.value);
                    factor = run.factor_at_tick(start + (end - start) * fraction, channel,
                                                output.num_channels());
                } else {
                    factor = run.factor(timebase::SamplePosition{
                                            range.timeline_sample_start.value +
                                            (transport.is_playing
                                                 ? static_cast<std::int64_t>(frame)
                                                 : 0)},
                                        channel, output.num_channels());
                }
                output.channel(channel)[range.sample_offset + frame] *= factor;
            }
        }
    }
}

} // namespace

AudioRenderStatus ArrangementAudioRenderer::process(const PlaybackProgram& program,
                                                    const TransportSnapshot& transport,
                                                    audio::BufferView<float> output,
                                                    AudioRendererLimits limits) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    limits = narrowed_limits(limits, program.audio_limits());
    if (output.empty())
        return AudioRenderStatus::InvalidOutput;
    if (output.num_channels() > limits.max_channels ||
        output.num_samples() > limits.max_block_frames)
        return AudioRenderStatus::CapacityExceeded;
    if (!valid_transport(transport, output.num_samples(), program.tempo_map(), limits)) {
        output.clear();
        return AudioRenderStatus::InvalidTransport;
    }

    if (program.tracks().size() > limits.max_tracks) {
        output.clear();
        return AudioRenderStatus::CapacityExceeded;
    }

    std::uint64_t total_clips = 0;
    for (const auto& track : program.tracks()) {
        if (!track->audio_program())
            continue;
        const auto clip_count = static_cast<std::uint64_t>(track->audio_program()->clips().size());
        if (const auto invalid = invalid_audio_program(*track->audio_program(), track->id(),
                                                       limits.max_clips - total_clips)) {
            output.clear();
            return *invalid;
        }
        total_clips += clip_count;
    }

    if (!transport.is_playing) {
        output.clear();
        return AudioRenderStatus::Silent;
    }
    for (const auto& track : program.tracks()) {
        const auto provider = track->provider();
        if (provider.selected == ProviderKind::Arrangement &&
            provider.available(ProviderKind::Arrangement) && track->audio_program() &&
            !preflight_source_steps(*track->audio_program(), transport, program.tempo_map()))
            return AudioRenderStatus::InvalidTransport;
    }
    output.clear();

    bool rendered = false;
    for (const auto& track : program.tracks()) {
        const auto provider = track->provider();
        if (provider.selected != ProviderKind::Arrangement ||
            !provider.available(ProviderKind::Arrangement) || !track->audio_program())
            continue;
        rendered = render_audio_program(*track->audio_program(), track->mixer(), transport,
                                        program.tempo_map(),
                                        output) == AudioRenderStatus::Rendered ||
                   rendered;
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
        if (view.adoption == ShellAdoptionResult::Rejected)
            return AudioRenderStatus::InvalidProgram;
        output.clear();
        return AudioRenderStatus::Silent;
    }
    const auto& program = *block.program();
    limits = narrowed_limits(limits, program.audio_limits());
    if (output.num_channels() > limits.max_channels ||
        output.num_samples() > limits.max_block_frames)
        return AudioRenderStatus::CapacityExceeded;
    const auto provider = view.program->provider();
    const auto* audio_program = view.program->audio_program();
    const bool arrangement_selected = provider.selected == ProviderKind::Arrangement &&
                                      provider.available(ProviderKind::Arrangement);
    if (transport.is_playing && arrangement_selected && audio_program != nullptr) {
        if (const auto invalid =
                invalid_audio_program(*audio_program, view.program->id(), limits.max_clips))
            return *invalid;
    }
    if (!valid_transport(transport, output.num_samples(), program.tempo_map(), limits)) {
        output.clear();
        return AudioRenderStatus::InvalidTransport;
    }
    if (transport.is_playing && arrangement_selected && audio_program != nullptr &&
        !preflight_source_steps(*audio_program, transport, program.tempo_map()))
        return AudioRenderStatus::InvalidTransport;
    output.clear();

    AudioRenderStatus status = AudioRenderStatus::Silent;
    if (transport.is_playing && arrangement_selected && audio_program != nullptr)
        status = render_audio_program(
            *audio_program, apply_track_mixer_ ? view.program->mixer() : TrackMixerProgram{},
            transport, program.tempo_map(), output);

    RendererCarryState carry = shell_.state_snapshot();
    carry.key = shell_.active_key();
    carry.active_provider = provider.selected;
    carry.valid = true;
    if (transport.range_count != 0) {
        const auto& last = transport.ranges[transport.range_count - 1];
        carry.source_sample =
            last.host_beat_mapping
                ? program.tempo_map().ticks_to_samples(last.timeline_tick_end)
                : timebase::SamplePosition{last.timeline_sample_start.value +
                                           static_cast<std::int64_t>(last.frame_count)};
        carry.timeline_tick = last.timeline_tick_end;
        carry.loop_iteration += transport.range_count > 1 ? 1u : 0u;
    }
    if (!shell_.end_block(carry))
        return AudioRenderStatus::InvalidProgram;
    return status;
}

AudioRenderStatus TrackMixerTrackRenderer::process(const PlaybackProgramBlock& block,
                                                   const TransportSnapshot& transport,
                                                   audio::BufferView<float> output,
                                                   const audio::BufferView<const float>& input,
                                                   AudioRendererLimits limits) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    if (output.empty() || input.empty() || output.num_channels() != input.num_channels() ||
        output.num_samples() != input.num_samples())
        return AudioRenderStatus::InvalidOutput;
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
    limits = narrowed_limits(limits, program.audio_limits());
    if (output.num_channels() > limits.max_channels ||
        output.num_samples() > limits.max_block_frames)
        return AudioRenderStatus::CapacityExceeded;
    if (!valid_transport(transport, output.num_samples(), program.tempo_map(), limits))
        return AudioRenderStatus::InvalidTransport;

    for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
        std::copy(input.channel(channel).begin(), input.channel(channel).end(),
                  output.channel(channel).begin());
    apply_track_mixer(view.program->mixer(), transport, program.tempo_map(), output);

    RendererCarryState carry = shell_.state_snapshot();
    carry.key = shell_.active_key();
    carry.active_provider = view.program->provider().selected;
    carry.valid = true;
    if (transport.range_count != 0) {
        const auto& last = transport.ranges[transport.range_count - 1];
        carry.source_sample =
            last.host_beat_mapping
                ? program.tempo_map().ticks_to_samples(last.timeline_tick_end)
                : timebase::SamplePosition{last.timeline_sample_start.value +
                                           static_cast<std::int64_t>(last.frame_count)};
        carry.timeline_tick = last.timeline_tick_end;
        carry.loop_iteration += transport.range_count > 1 ? 1u : 0u;
    }
    if (!shell_.end_block(carry))
        return AudioRenderStatus::InvalidProgram;
    return AudioRenderStatus::Rendered;
}

} // namespace pulp::playback
