#pragma once

#include <cstdint>

#include <pulp/audio/loop_types.hpp>

namespace pulp::audio {

struct LoopFrameReadPlan {
    double read_position = 0.0;
    double blend_position = 0.0;
    double primary_gain = 1.0;
    double blend_gain = 0.0;
    bool blend = false;
    /// A wrap-crossfade read pair is active for this frame. This can be true
    /// before traversal reaches the loop boundary.
    bool wrapped = false;
};

struct LoopPlaybackAdvanceResult {
    bool active = false;
    /// Traversal crossed or reflected at least one loop boundary.
    bool wrapped = false;
};

/// Allocation-free loop traversal state independent of sample storage.
class LoopPlaybackCursor {
public:
    bool set_region(const LoopRegion& region,
                    std::uint64_t source_frames) noexcept;
    void reset() noexcept;
    void start() noexcept;
    void stop() noexcept { active_ = false; }

    /// Sets the finite nonzero source-frame step value. Negative values are
    /// retained for compatibility with LoopRenderer's historical signed-rate API;
    /// new traversal policies should normally encode direction in LoopRegion.
    void set_playback_rate(double rate) noexcept;
    void set_playback_mode(LoopPlaybackMode mode) noexcept {
        region_.playback_mode = mode;
    }

    bool active() const noexcept { return active_; }
    double position() const noexcept { return position_; }
    double step() const noexcept;
    const LoopRegion& region() const noexcept { return region_; }

    LoopFrameReadPlan frame_read_plan() const noexcept;
    LoopPlaybackAdvanceResult advance() noexcept;

private:
    void initialize_entry() noexcept;

    LoopRegion region_;
    double position_ = 0.0;
    double playback_rate_ = 1.0;
    bool active_ = false;
    int pingpong_direction_ = 1;
    int step_direction_ = 1;
};

}  // namespace pulp::audio
