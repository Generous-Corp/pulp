#pragma once

/// @file chaos.hpp
/// Deterministic chaos as a control source.
///
/// RT contract: scalar state, no owned memory. `set_r()`, `seed()`, `next()`,
/// and `reset()` allocate nothing and are audio-thread safe.
///
/// USE: one cheap deterministic source that runs the whole way from periodic to
/// chaotic on a single control, for generative patches. Unlike an LFO it never
/// repeats; unlike noise its trajectory is continuous and its character changes
/// with the control rather than only its amplitude. Step it at control rate —
/// at audio rate it is broadband noise with extra steps.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

/// Logistic map: `x <- r * x * (1 - x)` (May, "Simple mathematical models with
/// very complicated dynamics", Nature 261, 1976).
///
/// The useful range of `r` is [3.5, 4.0]. At 3.5 the orbit is a clean 4-cycle;
/// by 3.57 it has gone chaotic through period doubling; at 4.0 it fills the
/// unit interval. Sweeping `r` across that range is the "one knob from
/// rhythmic to unpredictable" gesture the map is worth having for.
///
/// The state is clamped away from the 0 and 1 fixed points. At `r = 4` an
/// unlucky value lands exactly on one of them and the map sticks there forever;
/// the clamp costs nothing and turns a silent death into a continued orbit.
template <typename SampleType = float>
class LogisticMapT {
public:
    static constexpr double kMinR = 3.5;
    static constexpr double kMaxR = 4.0;

    /// Distance kept from the fixed points at 0 and 1.
    static constexpr double kEpsilon = 1.0e-6;

    void set_r(double r) { r_ = std::clamp(r, kMinR, kMaxR); }
    double r() const { return r_; }

    /// Initial condition, in (0, 1). Two instances with the same seed and the
    /// same `r` produce identical orbits forever.
    void seed(double x0) { seed_ = std::clamp(x0, kEpsilon, 1.0 - kEpsilon); }

    void reset() { x_ = seed_; }

    /// Advance and return the state in [0, 1].
    SampleType next() {
        x_ = std::clamp(r_ * x_ * (1.0 - x_), kEpsilon, 1.0 - kEpsilon);
        return static_cast<SampleType>(x_);
    }

    /// Advance and return the state mapped to [-1, 1].
    SampleType next_bipolar() { return static_cast<SampleType>(2.0) * next() - static_cast<SampleType>(1); }

    SampleType current() const { return static_cast<SampleType>(x_); }

private:
    double r_ = 3.9;
    double x_ = 0.5;
    double seed_ = 0.5;
};

using LogisticMap = LogisticMapT<float>;
using LogisticMap64 = LogisticMapT<double>;

} // namespace pulp::signal
