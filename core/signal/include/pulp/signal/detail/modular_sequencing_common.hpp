#pragma once

namespace pulp::signal {

namespace seq_detail {

/// Clamps into `[lo, hi]` and maps a non-finite input onto `lo`.
///
/// `std::clamp` propagates NaN, and these values go on to index arrays, size
/// windows and get cast to `int` — where a NaN or an out-of-range double is
/// undefined behaviour rather than a wrong note. Every setter that accepts a
/// number from outside the library goes through here, so a broken upstream
/// block produces a bounded output instead of a crash.
inline double clamp_finite(double x, double lo, double hi) {
    if (!(x >= lo)) return lo;  // false for NaN and for anything below lo
    if (!(x <= hi)) return hi;
    return x;
}

}  // namespace seq_detail

}  // namespace pulp::signal
