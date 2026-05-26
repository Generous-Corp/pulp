#pragma once

/// @file source.hpp
/// Audio source hierarchy — abstract bases for streaming audio
/// playback graphs.
///
/// `AudioSource` is the minimal interface a host calls per audio
/// block. `PositionableAudioSource` extends it with frame-level seek
/// and looping.
///
/// Concrete sources live in companion headers:
///   - `format_reader_source.hpp` — `AudioFormatReaderSource` wraps a
///     `MemoryMappedAudioReader` so a decoded audio file becomes a
///     seekable `PositionableAudioSource`.
///   - `transport_source.hpp` — `AudioTransportSource` adds play /
///     stop / gain-ramp control around any `PositionableAudioSource`.
///   - `source_player.hpp` — `AudioSourcePlayer` is the bridge from
///     an `AudioSource` into a host audio callback.

#include <pulp/audio/buffer.hpp>

#include <cstdint>

namespace pulp::audio {

/// Abstract base — host-callable audio source.
///
/// Lifecycle: `prepare_to_play(block_size, sample_rate)` is called
/// once before audio starts; `release_resources()` once after audio
/// stops. Between them, `get_next_audio_block()` is called per
/// audio block. All three must be safe to call from the audio
/// thread (no allocations after `prepare_to_play`).
class AudioSource {
public:
    virtual ~AudioSource() = default;

    /// Called before playback begins. `samples_per_block_expected` is
    /// the maximum block length the host will ever ask for;
    /// implementations may allocate scratch buffers of that size
    /// here.
    virtual void prepare_to_play(int samples_per_block_expected,
                                  double sample_rate) = 0;

    /// Called when playback is finished. Implementations should free
    /// any buffers allocated in `prepare_to_play`.
    virtual void release_resources() = 0;

    /// Fill `num_samples` of `out`, starting at `start_sample` within
    /// `out`. The source writes (not adds) into the range so callers
    /// don't need to pre-zero.
    virtual void get_next_audio_block(BufferView<float> out,
                                       int start_sample,
                                       int num_samples) = 0;
};

/// AudioSource with a notion of position. Subclasses expose a
/// monotonic frame counter that can be reset (`set_next_read_position`),
/// queried (`get_next_read_position`), and an optional total length
/// for finite sources (returns 0 for infinite streams).
class PositionableAudioSource : public AudioSource {
public:
    /// Move the read head to `new_position` (frames from the start
    /// of the source). For finite sources, positions past
    /// `get_total_length()` are caller-defined behavior — most
    /// implementations clamp to total length or wrap on the next
    /// read.
    virtual void set_next_read_position(uint64_t new_position) = 0;

    /// Current frame position the next `get_next_audio_block` call
    /// will read from.
    virtual uint64_t get_next_read_position() const = 0;

    /// Total length in frames. 0 means "unknown" or "infinite".
    virtual uint64_t get_total_length() const = 0;

    /// True if the source should wrap back to 0 when it reaches the
    /// end. Looping infinite sources are no-op.
    virtual bool is_looping() const = 0;

    /// Enable / disable looping. Default no-op for sources that
    /// don't support it; concrete sources override.
    virtual void set_looping(bool /*should_loop*/) {}
};

} // namespace pulp::audio
