#pragma once

// A Schmitt trigger: the only honest way to read a gate off a voltage.
//
// A modular's gate output slews, and a bare comparator on a slewed edge with any
// noise on it fires a handful of times on the way up. Two thresholds a little way
// apart cost one bool and remove the whole class of bug — so nothing in this suite
// reads a threshold with `>` and hopes.

#include <cstdint>

namespace pulp::examples::brew {

/// A latching comparator with separate rising and falling thresholds.
///
/// `high` must exceed `low`; a caller that hands it the same number gets a plain
/// comparator, which is what it asked for. The state is the only thing that makes
/// this more than a function: whether we are currently above.
class SchmittGate {
public:
    /// Forget the current level, so the next sample above `high` is an edge.
    void reset() noexcept { high_ = false; }

    [[nodiscard]] bool is_high() const noexcept { return high_; }

    /// Advance one sample. Returns true only on the sample the input rises through
    /// `high` — never again until it has fallen back below `low`.
    [[nodiscard]] bool process(float v, float high, float low) noexcept {
        if (!high_ && v >= high) {
            high_ = true;
            return true;
        }
        if (high_ && v < low) high_ = false;
        return false;
    }

private:
    bool high_ = false;
};

/// How far below the rising threshold the falling one sits, in normalized full
/// scale. Wide enough to ride out the noise on a slewed edge, narrow enough that a
/// slow ramp still releases where the user expects.
inline constexpr float kGateHysteresis = 0.25f;

}  // namespace pulp::examples::brew
