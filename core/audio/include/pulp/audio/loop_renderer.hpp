#pragma once

#include <cstdint>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_reader.hpp>
#include <pulp/audio/loop_types.hpp>

namespace pulp::audio {

struct LoopRenderResult {
    std::uint64_t rendered_frames = 0;
    std::uint64_t silent_frames = 0;
    bool active = false;
    bool wrapped = false;
    float max_sample_delta = 0.0f;
};

class LoopRenderer {
public:
    bool set_region(const LoopRegion& region,
                    std::uint64_t source_frames) noexcept;
    void reset() noexcept;
    void start() noexcept;
    void stop() noexcept;

    void set_playback_rate(double rate) noexcept;
    // Change the loop playback mode in place, preserving the current position — so a
    // sustaining voice can switch Forward<->OneShot without restarting (e.g. a LOOP
    // toggle acting on already-held notes). Does not re-arm fades or reset position.
    void set_playback_mode(LoopPlaybackMode mode) noexcept { region_.playback_mode = mode; }
    LoopPlaybackMode playback_mode() const noexcept { return region_.playback_mode; }
    void set_start_fade_frames(std::uint64_t frames) noexcept { start_fade_frames_ = frames; }
    void set_stop_fade_frames(std::uint64_t frames) noexcept { stop_fade_frames_ = frames; }

    bool active() const noexcept { return active_; }
    double position() const noexcept { return position_; }

    // Overwrite renderer for RT voice scratch buffers. Writes every frame up
    // to min(frames, destination.num_samples()) for every destination channel;
    // inactive, invalid-source, and fade-to-zero frames are written as silence.
    // The destination does not need to be pre-cleared, and this call never
    // accumulates into existing samples.
    LoopRenderResult render(BufferView<const float> source,
                            BufferView<float> destination,
                            std::uint64_t frames) noexcept;

private:
    float sample_with_crossfade(BufferView<const float> source,
                                std::uint32_t output_channel,
                                double position,
                                double step,
                                bool& wrapped) const noexcept;
    double advance_position(double position, double step, bool& wrapped) const noexcept;
    float fade_gain() noexcept;
    double effective_step() const noexcept;

    LoopRegion region_;
    double position_ = 0.0;
    double playback_rate_ = 1.0;
    std::uint64_t source_frames_ = 0;
    std::uint64_t start_fade_frames_ = 0;
    std::uint64_t stop_fade_frames_ = 0;
    std::uint64_t start_fade_position_ = 0;
    std::uint64_t stop_fade_position_ = 0;
    bool active_ = false;
    bool stopping_ = false;
};

}  // namespace pulp::audio
