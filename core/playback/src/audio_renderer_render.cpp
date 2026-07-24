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
                                             *host_source_frames_per_output_frame);
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

void render_track(const AudioTrackRendererProgram& track, const TransportRange& range,
                  audio::BufferView<float> output, bool absolute_only = false) noexcept {
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
                    source_sample(*clip, channel, output.num_channels(), source_frame, next_frame,
                                  fraction, static_cast<double>(source_position)) *
                    envelope(*clip, relative);
            }
        }
    }
}

void render_host_beat_mapped_track(const AudioTrackRendererProgram& track,
                                   const TransportRange& range,
                                   const timebase::CompiledTempoMap& tempo_map,
                                   audio::BufferView<float> output) noexcept {
    render_track(track, range, output, true);
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
            if (relative < 0.0L ||
                relative >= static_cast<long double>(clip->renderable_timeline_frames))
                continue;
            const auto source_position =
                relative * static_cast<long double>(clip->source_frames_per_timeline_frame);
            const auto next_document_position =
                host_mapped_document_sample_at_output_offset(range, tempo_map, output_frame + 1u);
            const auto source_frames_per_output_frame = static_cast<double>(
                std::abs(next_document_position - document_position) *
                static_cast<long double>(clip->source_frames_per_timeline_frame));
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
                output.channel(channel)[destination_frame] +=
                    source_sample(*clip, channel, output.num_channels(), source_frame, next_frame,
                                  fraction, static_cast<double>(source_position),
                                  source_frames_per_output_frame) *
                    envelope(*clip, relative);
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
                                       const TransportSnapshot& transport,
                                       const timebase::CompiledTempoMap& tempo_map,
                                       audio::BufferView<float> output) noexcept {
    for (std::uint8_t index = 0; index < transport.range_count; ++index) {
        const auto& range = transport.ranges[index];
        if (range.host_beat_mapping)
            render_host_beat_mapped_track(program, range, tempo_map, output);
        else
            render_track(program, range, output);
    }
    return program.clips().empty() ? AudioRenderStatus::Silent : AudioRenderStatus::Rendered;
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
        const auto clip_count = static_cast<std::uint64_t>(track->audio_program()->clips().size());
        if (const auto invalid = invalid_audio_program(*track->audio_program(), track->id(),
                                                       limits.max_clips - total_clips))
            return *invalid;
        total_clips += clip_count;
    }

    for (const auto& track : program.tracks()) {
        const auto provider = track->provider();
        if (provider.selected != ProviderKind::Arrangement ||
            !provider.available(ProviderKind::Arrangement) || !track->audio_program())
            continue;
        rendered = render_audio_program(*track->audio_program(), transport, program.tempo_map(),
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
    output.clear();

    AudioRenderStatus status = AudioRenderStatus::Silent;
    if (transport.is_playing && arrangement_selected && audio_program != nullptr)
        status = render_audio_program(*audio_program, transport, program.tempo_map(), output);

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

} // namespace pulp::playback
