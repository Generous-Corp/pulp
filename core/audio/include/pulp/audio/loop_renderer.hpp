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
    // Per-frame wrap-crossfade descriptor. The branch decision, the read
    // positions, and the blend gains depend only on (position, step) — not on
    // the channel — so render() computes this ONCE per frame and every channel
    // reuses it, instead of recomputing the equal-power cos/sin per channel.
    struct CrossfadePlan {
        double read_pos = 0.0;       // primary read position
        double blend_pos = 0.0;      // wrap-target read position (blend only)
        double primary_gain = 1.0;   // dry gain applied to read_pos
        double blend_gain = 0.0;     // wet gain applied to blend_pos
        bool blend = false;          // true => mix the two reads
        bool wrapped = false;        // whether this frame crosses the loop wrap
    };
    CrossfadePlan compute_crossfade_plan(double position, double step) const noexcept;
    float apply_crossfade_plan(BufferView<const float> source,
                               std::uint32_t output_channel,
                               const CrossfadePlan& plan) const noexcept;
    double advance_position(double position, double step, bool& wrapped) noexcept;
    void init_entry() noexcept;  // seed position + step_dir_ from the entry direction
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
    int pingpong_dir_ = 1;  // PingPong only: +1 forward, -1 backward; flips at boundaries
    // Current travel direction for OneShot / ReverseOnce / Forward / Reverse:
    // starts from the entry direction (reverse_entry) and, for Forward/Reverse
    // loops, switches to the loop's steady direction once the first pass reaches
    // the far edge (two-phase). +1 forward, -1 backward.
    int step_dir_ = 1;
};

}  // namespace pulp::audio
