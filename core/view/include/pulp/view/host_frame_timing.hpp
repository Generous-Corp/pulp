// host_frame_timing.hpp — bounded per-frame timing a window host keeps so a
// finite run can report how its frames and its GPU bring-up performed.
//
// Main-thread only: a host records from the frame it dispatches and reads back
// from the same thread (the headless screenshot capture runs inside that
// frame's idle pump). Nothing here allocates after the first reserve.
#pragma once

#include <chrono>
#include <cstddef>
#include <vector>

namespace pulp::view {

using HostClock = std::chrono::steady_clock;

/// Milliseconds elapsed since `since` on the monotonic host clock.
inline double host_elapsed_ms(HostClock::time_point since) noexcept {
    return std::chrono::duration<double, std::milli>(HostClock::now() - since).count();
}

/// Keeps the first kMaxSamples frame durations. A live window runs for hours,
/// so the record stops growing rather than tracking every frame forever; a
/// headless capture finishes long before the cap.
class HostFrameTimeRecorder {
  public:
    static constexpr std::size_t kMaxSamples = 4096;

    void record(double frame_ms) {
        if (samples_.size() >= kMaxSamples)
            return;
        if (samples_.capacity() == 0)
            samples_.reserve(256);
        samples_.push_back(frame_ms);
    }

    const std::vector<double>& samples() const noexcept { return samples_; }

  private:
    std::vector<double> samples_;
};

} // namespace pulp::view
