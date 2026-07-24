#include <pulp/format/offline_render_host.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace pulp::format {
namespace {

bool valid_config(const OfflineRenderConfig& config) {
    return std::isfinite(config.sample_rate) && config.sample_rate > 0.0 &&
           std::isfinite(config.tempo_bpm) && config.tempo_bpm > 0.0 &&
           config.max_block_frames > 0 &&
           config.max_block_frames <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
           config.input_channels <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
           config.output_channels > 0 &&
           config.output_channels <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
           config.time_sig_numerator > 0 &&
           config.time_sig_denominator > 0;
}

bool midi_events_sorted(std::span<const OfflineMidiEvent> events) {
    for (std::size_t i = 1; i < events.size(); ++i) {
        if (events[i].frame < events[i - 1].frame) return false;
    }
    return true;
}

bool parameter_events_sorted(std::span<const OfflineParameterEvent> events) {
    for (std::size_t i = 1; i < events.size(); ++i) {
        if (events[i].frame < events[i - 1].frame) return false;
    }
    return true;
}

bool valid_options(const OfflineRenderOptions& options) {
    return std::isfinite(options.tempo_bpm) &&
           std::isfinite(options.start_position_beats) &&
           std::isfinite(options.loop_start_beats) &&
           std::isfinite(options.loop_end_beats);
}

std::int64_t saturating_position(std::int64_t base, std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }

    const auto signed_offset = static_cast<std::int64_t>(offset);
    if (base > std::numeric_limits<std::int64_t>::max() - signed_offset) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return base + signed_offset;
}

std::int64_t host_time_for_block(std::int64_t start_host_time_ns,
                                 std::uint64_t block_start,
                                 double sample_rate) {
    if (start_host_time_ns == 0) return 0;

    const auto seconds = static_cast<long double>(block_start) /
                         static_cast<long double>(sample_rate);
    const auto target = static_cast<long double>(start_host_time_ns) +
                        seconds * 1000000000.0L;
    const auto max_value = static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    const auto min_value = static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    if (target >= max_value) return std::numeric_limits<std::int64_t>::max();
    if (target <= min_value) return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(std::llround(target));
}

ProcessContext make_context(const OfflineRenderConfig& config,
                            const OfflineRenderOptions& options,
                            std::uint64_t block_start,
                            std::uint32_t block_frames) {
    const auto tempo = options.tempo_bpm > 0.0 ? options.tempo_bpm : config.tempo_bpm;
    const auto numerator = options.time_sig_numerator > 0
        ? options.time_sig_numerator
        : config.time_sig_numerator;
    const auto denominator = options.time_sig_denominator > 0
        ? options.time_sig_denominator
        : config.time_sig_denominator;

    ProcessContext context;
    context.sample_rate = config.sample_rate;
    context.num_samples = static_cast<int>(block_frames);
    context.is_playing = options.is_playing;
    context.is_recording = options.is_recording;
    context.tempo_bpm = tempo;
    context.position_samples = saturating_position(options.start_position_samples, block_start);
    context.position_beats = options.start_position_beats +
        (static_cast<double>(block_start) / config.sample_rate) * (tempo / 60.0);
    context.time_sig_numerator = numerator;
    context.time_sig_denominator = denominator;
    context.bar = static_cast<std::int64_t>(std::floor(
        context.position_beats * (static_cast<double>(denominator) / 4.0) /
        static_cast<double>(numerator)));
    context.is_looping = options.is_looping;
    context.loop_start_beats = options.loop_start_beats;
    context.loop_end_beats = options.loop_end_beats;
    context.host_time_ns = host_time_for_block(options.host_time_ns, block_start, config.sample_rate);
    context.transport_validity.set(TransportField::Playing);
    context.transport_validity.set(TransportField::Recording);
    context.transport_validity.set(TransportField::Looping);
    context.transport_validity.set(TransportField::Tempo);
    context.transport_validity.set(TransportField::BeatPosition);
    context.transport_validity.set(TransportField::SamplePosition);
    context.transport_validity.set(TransportField::TimeSignature);
    context.transport_validity.set(TransportField::Bar);
    if (options.is_looping) {
        context.transport_validity.set(TransportField::LoopRange);
    }
    if (options.host_time_ns != 0) {
        context.transport_validity.set(TransportField::HostTime);
    }
    return context;
}

void refresh_const_input_ptrs(const audio::Buffer<float>& buffer,
                              std::vector<const float*>& ptrs) {
    ptrs.resize(buffer.num_channels());
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        ptrs[ch] = buffer.channel(ch).data();
    }
}

} // namespace

OfflineRenderHost::OfflineRenderHost(ProcessorFactory factory)
    : host_(factory) {}

bool OfflineRenderHost::prepare(const OfflineRenderConfig& config) {
    if (!valid_config(config) || !host_.valid()) {
        prepared_ = false;
        return false;
    }

    config_ = config;
    try {
        input_block_.resize(config_.input_channels, config_.max_block_frames);
        output_block_.resize(config_.output_channels, config_.max_block_frames);
        host_.prepare(config_.sample_rate,
                      static_cast<int>(config_.max_block_frames),
                      static_cast<int>(config_.input_channels),
                      static_cast<int>(config_.output_channels));
    } catch (...) {
        prepared_ = false;
        return false;
    }

    prepared_ = true;
    return true;
}

OfflineRenderResult OfflineRenderHost::render(const OfflineRenderOptions& options) {
    OfflineRenderResult result;
    if (!prepared_ || !valid_options(options)) return result;

    if (options.frame_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return result;
    }

    const auto block_frames = options.block_frames == 0
        ? config_.max_block_frames
        : options.block_frames;
    if (block_frames == 0 || block_frames > config_.max_block_frames) {
        return result;
    }

    if (!midi_events_sorted(options.midi_events) ||
        !parameter_events_sorted(options.parameter_events)) {
        result.stats.event_order_invalid = true;
        return result;
    }

    try {
        result.audio.resize(config_.output_channels, static_cast<std::size_t>(options.frame_count));
        result.audio.clear();

        std::vector<const float*> input_ptrs;
        refresh_const_input_ptrs(input_block_, input_ptrs);

        std::size_t midi_index = 0;
        std::size_t parameter_index = 0;
        midi::MidiBuffer midi_in;
        midi::MidiBuffer midi_out;
        state::ParameterEventQueue parameter_events;

        for (std::uint64_t frame = 0; frame < options.frame_count; frame += block_frames) {
            const auto frames_remaining = options.frame_count - frame;
            const auto frames_this_block = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(block_frames, frames_remaining));

            input_block_.clear();
            output_block_.clear();

            if (!options.input.empty()) {
                if (frame >= options.input.num_samples()) {
                    result.stats.input_truncated = true;
                } else {
                    const auto frame_index = static_cast<std::size_t>(frame);
                    const auto source_remaining = options.input.num_samples() - frame_index;
                    const auto frames_to_copy = std::min<std::size_t>(frames_this_block,
                                                                      source_remaining);
                    const auto channels_to_copy = std::min<std::size_t>(
                        config_.input_channels, options.input.num_channels());
                    for (std::size_t ch = 0; ch < channels_to_copy; ++ch) {
                        const auto source = options.input.channel(ch).subspan(frame_index,
                                                                              frames_to_copy);
                        auto dest = input_block_.channel(ch).subspan(0, frames_to_copy);
                        std::copy(source.begin(), source.end(), dest.begin());
                    }
                    if (frames_to_copy < frames_this_block) {
                        result.stats.input_truncated = true;
                    }
                }
            }

            midi_in.clear();
            midi_out.clear();
            parameter_events.clear();

            while (midi_index < options.midi_events.size() &&
                   options.midi_events[midi_index].frame < frame) {
                ++midi_index;
            }
            while (midi_index < options.midi_events.size() &&
                   options.midi_events[midi_index].frame < frame + frames_this_block) {
                auto event = options.midi_events[midi_index].event;
                event.sample_offset = static_cast<std::int32_t>(
                    options.midi_events[midi_index].frame - frame);
                midi_in.add(event);
                ++result.stats.midi_events_dispatched;
                ++midi_index;
            }

            while (parameter_index < options.parameter_events.size() &&
                   options.parameter_events[parameter_index].frame < frame) {
                ++parameter_index;
            }
            while (parameter_index < options.parameter_events.size() &&
                   options.parameter_events[parameter_index].frame < frame + frames_this_block) {
                auto event = options.parameter_events[parameter_index].event;
                event.sample_offset = static_cast<std::int32_t>(
                    options.parameter_events[parameter_index].frame - frame);
                if (parameter_events.push(event)) {
                    ++result.stats.parameter_events_dispatched;
                } else {
                    ++result.stats.parameter_events_dropped;
                }
                ++parameter_index;
            }
            parameter_events.sort();

            const audio::BufferView<const float> input_view(
                input_ptrs.data(), config_.input_channels, config_.max_block_frames);
            auto input_block_view = input_view.slice(0, frames_this_block);
            auto output_view = output_block_.view();
            auto output_block_view = output_view.slice(0, frames_this_block);
            auto context = make_context(config_, options, frame, frames_this_block);

            host_.process(output_block_view, input_block_view,
                          midi_in, midi_out, parameter_events, context);

            for (std::size_t ch = 0; ch < config_.output_channels; ++ch) {
                const auto source = output_block_.channel(ch).subspan(0, frames_this_block);
                auto dest = result.audio.channel(ch).subspan(static_cast<std::size_t>(frame),
                                                            frames_this_block);
                std::copy(source.begin(), source.end(), dest.begin());
            }

            result.stats.frames_rendered += frames_this_block;
            ++result.stats.blocks_rendered;
        }
    } catch (...) {
        result.ok = false;
        return result;
    }

    result.ok = result.stats.parameter_events_dropped == 0;
    return result;
}

} // namespace pulp::format
