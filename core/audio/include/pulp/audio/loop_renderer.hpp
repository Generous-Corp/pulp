#pragma once

#include <cstdint>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_playback_cursor.hpp>
#include <pulp/audio/loop_reader.hpp>
#include <pulp/audio/loop_types.hpp>

namespace pulp::audio {

struct LoopRenderResult {
    std::uint64_t rendered_frames = 0;
    std::uint64_t source_backed_frames = 0;
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
    bool set_interpolation_policy(SampleInterpolationPolicy policy) noexcept;
    bool set_interpolation(const PreparedSampleInterpolation& interpolation) noexcept;
    SampleInterpolationPolicy interpolation_policy() const noexcept {
        return interpolation_.policy;
    }
    const PreparedSampleInterpolation& interpolation() const noexcept {
        return interpolation_;
    }
    // Change the loop playback mode in place, preserving the current position — so a
    // sustaining voice can switch Forward<->OneShot without restarting (e.g. a LOOP
    // toggle acting on already-held notes). Does not re-arm fades or reset position.
    void set_playback_mode(LoopPlaybackMode mode) noexcept { cursor_.set_playback_mode(mode); }
    LoopPlaybackMode playback_mode() const noexcept { return cursor_.region().playback_mode; }
    void set_start_fade_frames(std::uint64_t frames) noexcept { start_fade_frames_ = frames; }
    void set_stop_fade_frames(std::uint64_t frames) noexcept { stop_fade_frames_ = frames; }

    bool active() const noexcept { return cursor_.active(); }
    double position() const noexcept { return cursor_.position(); }

    // Overwrite renderer for RT voice scratch buffers. Writes every frame up
    // to min(frames, destination.num_samples()) for every destination channel;
    // inactive, invalid-source, and fade-to-zero frames are written as silence.
    // The destination does not need to be pre-cleared, and this call never
    // accumulates into existing samples.
    LoopRenderResult render(BufferView<const float> source,
                            BufferView<float> destination,
                            std::uint64_t frames) noexcept;

private:
    float apply_crossfade_plan(BufferView<const float> source,
                               std::uint32_t output_channel,
                               const LoopFrameReadPlan& plan) const noexcept;
    float fade_gain() noexcept;

    LoopPlaybackCursor cursor_;
    PreparedSampleInterpolation interpolation_{
        .policy = SampleInterpolationPolicy::Linear};
    std::uint64_t start_fade_frames_ = 0;
    std::uint64_t stop_fade_frames_ = 0;
    std::uint64_t start_fade_position_ = 0;
    std::uint64_t stop_fade_position_ = 0;
    bool stopping_ = false;
};

}  // namespace pulp::audio
