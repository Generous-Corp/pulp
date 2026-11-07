#pragma once

/// @file pull_rate_detector.hpp
/// Tells whether a host is pulling audio faster than real time.

#include <array>
#include <chrono>
#include <cstddef>

namespace pulp::audio {

/// Detects a host that calls the audio callback faster than real time.
///
/// A plug-in that renders on a worker thread (an emulator, offloaded DSP)
/// never waits for the worker on a live callback; it plays whatever the
/// worker has finished. A host that bounces or freezes faster than real time
/// without declaring an offline render therefore gets silence, because the
/// calls arrive back to back and the worker cannot keep up. Feed every
/// callback to observe(); while it returns true the callback may wait for the
/// worker as it would for a declared offline render.
///
/// The decision covers the last kWindow calls, so a host that splits one
/// device cycle into several back-to-back calls still averages to real time
/// across cycles. Hysteresis keeps a bounce from flapping: the detector
/// enters above kEnter times real time and leaves below kLeave times.
///
/// Realtime safe: arithmetic on a fixed ring, no allocation, no lock. One
/// thread (the audio thread) calls observe().
class PullRateDetector {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kWindow = 8;
    static constexpr double kEnter = 2.0;
    static constexpr double kLeave = 1.25;

    /// Records one callback of `frames` at `sample_rate` that started at `now`
    /// and returns whether the host is currently pulling faster than real time.
    bool observe(std::size_t frames, double sample_rate, Clock::time_point now) noexcept {
        calls_[next_ % kWindow] = {now, frames};
        ++next_;
        if (next_ < kWindow || !(sample_rate > 0.0))
            return fast_;
        // The oldest call in the window starts the wall-clock span; the audio
        // of the newer calls is what was delivered within that span.
        const auto& oldest = calls_[next_ % kWindow];
        std::size_t newer_frames = 0;
        for (std::size_t i = 1; i < kWindow; ++i)
            newer_frames += calls_[(next_ + i) % kWindow].frames;
        const double audio_s = static_cast<double>(newer_frames) / sample_rate;
        const double wall_s = std::chrono::duration<double>(now - oldest.time).count();
        speed_ = wall_s > 0.0 ? audio_s / wall_s : kEnter * 1000.0;
        fast_ = fast_ ? speed_ >= kLeave : speed_ > kEnter;
        return fast_;
    }

    bool fast() const noexcept {
        return fast_;
    }
    /// Audio seconds delivered per wall-clock second over the last full
    /// window; 0 until kWindow calls were observed.
    double speed() const noexcept {
        return speed_;
    }

    /// Forget every observation (a new stream: prepare, transport restart).
    void reset() noexcept {
        next_ = 0;
        fast_ = false;
        speed_ = 0.0;
    }

  private:
    struct Call {
        Clock::time_point time{};
        std::size_t frames = 0;
    };
    std::array<Call, kWindow> calls_{};
    std::size_t next_ = 0;
    bool fast_ = false;
    double speed_ = 0.0;
};

} // namespace pulp::audio
