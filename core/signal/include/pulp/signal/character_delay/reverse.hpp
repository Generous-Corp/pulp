#pragma once

// Reverse segmenter — double-buffered capture/playback, one per channel.
//
// The standard reverse-delay construction: one buffer is written forward while
// the other is read backward, and the two swap when the write head reaches the
// end of a segment. Segment length is the current SLEWED delay time, so the
// reversed playback lines up with what the delay time claims and a time change
// still glides rather than jumping.
//
// Two details make it usable rather than merely correct:
//
//   * A raised-cosine window over the first and last kReverseFadeSamples of
//     each segment. Reversal splices the newest sample of one segment against
//     the oldest of the next; without the fade that discontinuity is an
//     audible tick on every segment boundary, and inside a feedback loop it is
//     a tick that recirculates.
//   * A FRACTIONAL read position. Tape's wow and flutter modulate the read
//     head, and they have to keep doing that when the transport is reversed —
//     otherwise engaging reverse silently switches the tape character's
//     defining instability off.

#include <pulp/signal/character_delay/primitives.hpp>
#include <pulp/signal/character_delay/tables.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace pulp::signal::chardelay {

class ReverseSegmenter {
public:
    /// Allocates both buffers. Control thread only.
    void prepare(std::size_t capacity_samples) {
        capacity_ = std::max<std::size_t>(capacity_samples, 8u);
        capture_.assign(capacity_, 0.0f);
        playback_.assign(capacity_, 0.0f);
        reset();
    }

    void reset() noexcept {
        std::fill(capture_.begin(), capture_.end(), 0.0f);
        std::fill(playback_.begin(), playback_.end(), 0.0f);
        position_ = 0;
        capture_length_ = min_segment();
        playback_length_ = min_segment();
    }

    /// One sample of capture-forward / play-backward.
    /// `segment_samples` is the requested segment length; it is latched at the
    /// next boundary so a slewing delay time never tears the segment in flight.
    /// `mod_offset_samples` displaces the read head (tape instability).
    double process(double x, double segment_samples, double mod_offset_samples) noexcept {
        const auto requested = static_cast<std::size_t>(
            std::clamp(segment_samples, static_cast<double>(min_segment()),
                       static_cast<double>(capacity_)));

        capture_[position_] = static_cast<float>(x);

        const double backward =
            static_cast<double>(playback_length_ - 1u) - static_cast<double>(position_);
        const double read_position = std::clamp(
            backward + mod_offset_samples, 0.0, static_cast<double>(playback_length_ - 1u));
        const double sample = interpolate(playback_, playback_length_, read_position);
        const double windowed = sample * boundary_gain();

        if (++position_ >= capture_length_) {
            capture_.swap(playback_);
            playback_length_ = capture_length_;
            std::fill(capture_.begin(), capture_.end(), 0.0f);
            capture_length_ = requested;
            position_ = 0;
        }
        return windowed;
    }

private:
    std::size_t min_segment() const noexcept {
        return static_cast<std::size_t>(2 * kReverseFadeSamples);
    }

    /// Raised-cosine ramp over the fade zone at each end of the segment.
    double boundary_gain() const noexcept {
        const auto fade = static_cast<std::size_t>(kReverseFadeSamples);
        const std::size_t from_end = capture_length_ - 1u - position_;
        const std::size_t nearest = std::min(position_, from_end);
        if (nearest >= fade) return 1.0;
        const double t = static_cast<double>(nearest) / static_cast<double>(fade);
        return 0.5 * (1.0 - std::cos(kPi * t));
    }

    /// 4-point Lagrange over a linear (non-circular) buffer, index-clamped at
    /// the ends. The clamp costs nothing audible: the fade window has already
    /// taken the signal to zero wherever the kernel would run off the segment.
    static double interpolate(const std::vector<float>& buffer, std::size_t length,
                              double position) noexcept {
        const auto base = static_cast<long long>(std::floor(position));
        const double frac = position - static_cast<double>(base);
        const auto last = static_cast<long long>(length) - 1;

        auto at = [&](long long index) -> double {
            const long long clamped = std::clamp(index, 0LL, last);
            return static_cast<double>(buffer[static_cast<std::size_t>(clamped)]);
        };

        const double s0 = at(base - 1);
        const double s1 = at(base);
        const double s2 = at(base + 1);
        const double s3 = at(base + 2);

        const double d0 = frac + 1.0;
        const double d1 = frac;
        const double d2 = frac - 1.0;
        const double d3 = frac - 2.0;

        return (-d1 * d2 * d3 / 6.0) * s0 + (d0 * d2 * d3 * 0.5) * s1 +
               (-d0 * d1 * d3 * 0.5) * s2 + (d0 * d1 * d2 / 6.0) * s3;
    }

    std::vector<float> capture_;
    std::vector<float> playback_;
    std::size_t capacity_ = 8;
    std::size_t position_ = 0;
    std::size_t capture_length_ = 1;
    std::size_t playback_length_ = 1;
};

}  // namespace pulp::signal::chardelay
