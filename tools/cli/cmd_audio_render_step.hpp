#pragma once

// Pure, callback-driven block stepper for `pulp audio render`.
//
// This is a deliberate parallel to pulp::format::OfflineRenderHost::render():
// the slicing arithmetic (final-block shortening, input copy + silence
// padding, sample-accurate MIDI/parameter windowing with per-block offset
// rewrite) is identical in spirit. It is NOT reused directly because
// OfflineRenderHost drives a ProcessorFactory through HeadlessHost — its inner
// callback hands the processor a ProcessContext — whereas pulp::host::PluginSlot
// has no ProcessContext in its process() contract. Rather than thread a second
// process shape through the shipped, well-tested core renderer, the stepping
// logic lives here behind a process callback so it can drive a PluginSlot and
// be unit-tested with a fake callback (no real plugin bundle, no audio device).
//
// The block-partition-invariance test (test_cmd_audio_render.cpp) is the
// anti-drift guard the plan requires: a pure-passthrough callback must produce
// identical output at any block size, and an event placed exactly on a block
// boundary must land in the block that starts at that frame.

#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace pulp::cli::audio_render {

struct StepSpec {
    std::uint32_t input_channels = 2;
    std::uint32_t output_channels = 2;
    std::uint32_t max_block_frames = 512;
    std::uint64_t frame_count = 0;
    std::uint32_t block_frames = 0;  ///< 0 uses max_block_frames.
};

/// MIDI/parameter events tagged with an absolute render frame. The stepper
/// rewrites event.sample_offset relative to each process block.
struct TimedMidi {
    std::uint64_t frame = 0;
    midi::MidiEvent event;
};
struct TimedParam {
    std::uint64_t frame = 0;
    state::ParameterEvent event;
};

struct StepEvents {
    std::span<const TimedMidi> midi;     ///< sorted ascending by frame
    std::span<const TimedParam> params;  ///< sorted ascending by frame
};

struct StepStats {
    std::uint64_t frames_rendered = 0;
    std::uint32_t blocks_rendered = 0;
    std::uint32_t midi_dispatched = 0;
    std::uint32_t params_dispatched = 0;
    std::uint32_t params_dropped = 0;
    std::uint32_t midi_out_of_range = 0;
    std::uint32_t params_out_of_range = 0;
    bool input_truncated = false;
};

namespace detail {
inline bool midi_sorted(std::span<const TimedMidi> e) {
    for (std::size_t i = 1; i < e.size(); ++i)
        if (e[i].frame < e[i - 1].frame) return false;
    return true;
}
inline bool params_sorted(std::span<const TimedParam> e) {
    for (std::size_t i = 1; i < e.size(); ++i)
        if (e[i].frame < e[i - 1].frame) return false;
    return true;
}
}  // namespace detail

/// Drive `process` block-by-block over [0, frame_count), accumulating output
/// into `out_accum` (resized to output_channels x frame_count and zero-filled).
///
/// `process` must be callable as:
///   void(audio::BufferView<float>& out,
///        const audio::BufferView<const float>& in,
///        const midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
///        const state::ParameterEventQueue& params, int num_samples)
/// — i.e. exactly pulp::host::PluginSlot::process minus the slot.
///
/// Returns false on an invalid spec or unsorted event input (out_accum is then
/// left empty); true otherwise. `process` exceptions are not caught here.
template <typename ProcessFn>
bool render_blocks(const StepSpec& spec,
                   const audio::BufferView<const float>& input,
                   const StepEvents& events,
                   audio::Buffer<float>& out_accum,
                   StepStats& stats,
                   ProcessFn&& process) {
    out_accum = audio::Buffer<float>{};
    stats = StepStats{};

    if (spec.output_channels == 0) return false;
    if (spec.max_block_frames == 0) return false;
    const std::uint32_t block_frames =
        spec.block_frames == 0 ? spec.max_block_frames : spec.block_frames;
    if (block_frames == 0 || block_frames > spec.max_block_frames) return false;
    if (spec.frame_count >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    if (!detail::midi_sorted(events.midi) || !detail::params_sorted(events.params))
        return false;

    out_accum.resize(spec.output_channels,
                     static_cast<std::size_t>(spec.frame_count));
    out_accum.clear();

    audio::Buffer<float> input_block(spec.input_channels, spec.max_block_frames);
    audio::Buffer<float> output_block(spec.output_channels, spec.max_block_frames);

    std::vector<const float*> input_ptrs(spec.input_channels);
    for (std::uint32_t ch = 0; ch < spec.input_channels; ++ch)
        input_ptrs[ch] = input_block.channel(ch).data();

    std::size_t midi_index = 0;
    std::size_t param_index = 0;
    midi::MidiBuffer midi_in, midi_out;
    state::ParameterEventQueue params;

    for (std::uint64_t frame = 0; frame < spec.frame_count; frame += block_frames) {
        const auto remaining = spec.frame_count - frame;
        const auto n = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(block_frames, remaining));

        input_block.clear();
        output_block.clear();

        if (!input.empty()) {
            if (frame >= input.num_samples()) {
                stats.input_truncated = true;
            } else {
                const auto src_index = static_cast<std::size_t>(frame);
                const auto src_remaining = input.num_samples() - src_index;
                const auto to_copy = std::min<std::size_t>(n, src_remaining);
                const auto chans =
                    std::min<std::size_t>(spec.input_channels, input.num_channels());
                for (std::size_t ch = 0; ch < chans; ++ch) {
                    const auto source = input.channel(ch).subspan(src_index, to_copy);
                    auto dest = input_block.channel(ch).subspan(0, to_copy);
                    std::copy(source.begin(), source.end(), dest.begin());
                }
                if (to_copy < n) stats.input_truncated = true;
            }
        }

        midi_in.clear();
        midi_out.clear();
        params.clear();

        while (midi_index < events.midi.size() &&
               events.midi[midi_index].frame < frame)
            ++midi_index;
        while (midi_index < events.midi.size() &&
               events.midi[midi_index].frame < frame + n) {
            auto e = events.midi[midi_index].event;
            e.sample_offset =
                static_cast<std::int32_t>(events.midi[midi_index].frame - frame);
            midi_in.add(e);
            ++stats.midi_dispatched;
            ++midi_index;
        }

        while (param_index < events.params.size() &&
               events.params[param_index].frame < frame)
            ++param_index;
        while (param_index < events.params.size() &&
               events.params[param_index].frame < frame + n) {
            auto e = events.params[param_index].event;
            e.sample_offset =
                static_cast<std::int32_t>(events.params[param_index].frame - frame);
            if (params.push(e))
                ++stats.params_dispatched;
            else
                ++stats.params_dropped;
            ++param_index;
        }
        params.sort();

        const audio::BufferView<const float> input_view(
            input_ptrs.data(), spec.input_channels, spec.max_block_frames);
        auto in_slice = input_view.slice(0, n);
        auto out_view = output_block.view();
        auto out_slice = out_view.slice(0, n);

        process(out_slice, in_slice, midi_in, midi_out, params,
                static_cast<int>(n));

        for (std::uint32_t ch = 0; ch < spec.output_channels; ++ch) {
            const auto source = output_block.channel(ch).subspan(0, n);
            auto dest = out_accum.channel(ch).subspan(
                static_cast<std::size_t>(frame), n);
            std::copy(source.begin(), source.end(), dest.begin());
        }

        stats.frames_rendered += n;
        ++stats.blocks_rendered;
    }

    stats.midi_out_of_range = static_cast<std::uint32_t>(
        std::min<std::size_t>(events.midi.size() - midi_index,
                              std::numeric_limits<std::uint32_t>::max()));
    stats.params_out_of_range = static_cast<std::uint32_t>(
        std::min<std::size_t>(events.params.size() - param_index,
                              std::numeric_limits<std::uint32_t>::max()));

    return true;
}

}  // namespace pulp::cli::audio_render
