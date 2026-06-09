#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <cstdint>
#include <span>

namespace pulp::format {

/// Prepare-time configuration for deterministic offline rendering.
///
/// OfflineRenderHost is a control-thread/test harness. It owns staging buffers
/// and rendered output, then feeds bounded blocks into HeadlessHost so the
/// processor still observes normal ProcessContext, MIDI, and parameter-event
/// contracts.
struct OfflineRenderConfig {
    double sample_rate = 48000.0;
    std::uint32_t max_block_frames = 512;
    std::uint32_t input_channels = 2;
    std::uint32_t output_channels = 2;
    double tempo_bpm = 120.0;
    int time_sig_numerator = 4;
    int time_sig_denominator = 4;
};

struct OfflineMidiEvent {
    std::uint64_t frame = 0; ///< Absolute render frame; event.sample_offset is overwritten per block.
    midi::MidiEvent event;
};

struct OfflineParameterEvent {
    std::uint64_t frame = 0; ///< Absolute render frame; event.sample_offset is overwritten per block.
    state::ParameterEvent event;
};

struct OfflineRenderOptions {
    std::uint64_t frame_count = 0;
    std::uint32_t block_frames = 0; ///< 0 uses OfflineRenderConfig::max_block_frames.
    audio::BufferView<const float> input;
    std::span<const OfflineMidiEvent> midi_events;
    std::span<const OfflineParameterEvent> parameter_events;

    bool is_playing = true;
    bool is_recording = false;
    bool is_looping = false;
    double loop_start_beats = 0.0;
    double loop_end_beats = 0.0;

    /// Non-positive values use the prepared config defaults.
    double tempo_bpm = 0.0;
    int time_sig_numerator = 0;
    int time_sig_denominator = 0;

    std::int64_t start_position_samples = 0;
    double start_position_beats = 0.0;
    std::int64_t host_time_ns = 0;
};

struct OfflineRenderStats {
    std::uint64_t frames_rendered = 0;
    std::uint32_t blocks_rendered = 0;
    std::uint32_t midi_events_dispatched = 0;
    std::uint32_t parameter_events_dispatched = 0;
    std::uint32_t parameter_events_dropped = 0;
    bool input_truncated = false;
    bool event_order_invalid = false;
};

struct OfflineRenderResult {
    bool ok = false;
    audio::Buffer<float> audio;
    OfflineRenderStats stats;
};

/// General-purpose offline renderer over a ProcessorFactory.
///
/// This is intentionally layered above HeadlessHost rather than replacing it:
/// HeadlessHost remains the one-block seam, while OfflineRenderHost owns
/// multi-block stepping, final-block shortening, input silence padding, and
/// sample-accurate event slicing. Callers must pass MIDI and parameter events
/// sorted by absolute frame. Nested event sample offsets are ignored and
/// rewritten relative to each process block.
class OfflineRenderHost {
public:
    explicit OfflineRenderHost(ProcessorFactory factory);

    bool prepare(const OfflineRenderConfig& config);
    bool prepared() const noexcept { return prepared_; }
    const OfflineRenderConfig& config() const noexcept { return config_; }

    OfflineRenderResult render(const OfflineRenderOptions& options);

    HeadlessHost& headless() noexcept { return host_; }
    const HeadlessHost& headless() const noexcept { return host_; }

private:
    HeadlessHost host_;
    OfflineRenderConfig config_;
    bool prepared_ = false;

    audio::Buffer<float> input_block_;
    audio::Buffer<float> output_block_;
};

} // namespace pulp::format
