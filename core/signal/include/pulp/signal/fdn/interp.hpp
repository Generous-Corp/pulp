#pragma once

// Fractional-position reconstruction shared by the resampler, the modulated
// delay lines, and the ensemble chorus.
//
// 4-point Hermite is the engine's one interpolator. It is not band-limited on
// its own — the anti-alias / reconstruction filters carry that job — but it has
// a continuous first derivative, which is what keeps a swept read position from
// producing the zipper that linear interpolation gives on a slow sweep.

#include <pulp/signal/interpolator.hpp>

#include <array>
#include <cstddef>

namespace pulp::signal::fdn {

// y0..y3 straddle the read position; t in [0, 1) is the fraction between y1
// and y2.
//
// The arithmetic is signal::Interpolator::hermite — this is an argument-order
// adapter, not a second implementation. The shared entry point names its points
// by position relative to the sample being read (ym1, y0, y1, y2), which reads
// naturally at a call site that already has an index; a ring buffer walking
// four consecutive taps has no such index, so the callers here are clearer with
// a flat y0..y3. One curve, two spellings of the same four points.
inline double hermite4(double y0, double y1, double y2, double y3, double t) {
    return Interpolator::hermite<double>(t, y0, y1, y2, y3);
}

// A 4-sample sliding window over a source stream, for consumers that pull
// samples one at a time rather than indexing a buffer (the resampler).
class HermiteHistory {
public:
    void reset() { h_ = {0.0, 0.0, 0.0, 0.0}; }

    void push(double sample) {
        h_[0] = h_[1];
        h_[1] = h_[2];
        h_[2] = h_[3];
        h_[3] = sample;
    }

    // `t` is the fraction past h_[2] — the most recent fully-arrived sample —
    // toward h_[3], so a read never needs a sample that has not been pushed.
    double read(double t) const { return hermite4(h_[0], h_[1], h_[2], h_[3], t); }

private:
    std::array<double, 4> h_{};
};

}  // namespace pulp::signal::fdn
