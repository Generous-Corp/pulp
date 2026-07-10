#pragma once

#include <pulp/signal/denormal.hpp>

namespace pulp::signal {

/// One-pole DC blocker (first-order high-pass).
///
/// Removes DC offset and very-low-frequency drift from an audio stream
/// without measurably altering the audio band.
///
/// RT contract: `set_pole()`, `reset()`, and process paths are scalar-only and
/// allocate no memory.
///
/// Transfer function: H(z) = (1 - z^-1) / (1 - p * z^-1)
/// where p is the pole position (default 0.995 → ~3.5 Hz corner at 44.1 kHz).
///
/// @code
/// pulp::signal::DcBlocker blocker;
/// blocker.set_pole(0.995f);
/// for (int i = 0; i < num_samples; ++i)
///     output[i] = blocker.process(input[i]);
/// @endcode
///
/// For a more aggressive corner frequency, use a higher-order HPF biquad
/// instead — this primitive is intentionally minimal.
template<typename T = float>
class DcBlocker {
public:
    DcBlocker() = default;

    /// Set the pole position (typically 0.99..0.999).
    /// Higher values give a lower corner frequency.
    /// At 44.1 kHz: 0.995 ≈ 3.5 Hz, 0.999 ≈ 0.7 Hz.
    void set_pole(T pole) { pole_ = pole; }

    /// Clear filter state. Call between unrelated streams.
    void reset() { last_in_ = T(0); last_out_ = T(0); }

    /// Process one sample.
    T process(T x) {
        // Snap the recursive output state: with a pole near 1 the tail decays
        // slowly toward zero and stalls in denormals with no FTZ guard. This is
        // exactly the pattern denormal.hpp documents. No-op above 1e-15.
        const T y = snap_to_zero(x - last_in_ + pole_ * last_out_);
        last_in_ = x;
        last_out_ = y;
        return y;
    }

    /// Process a block in-place.
    void process(T* samples, int n) {
        for (int i = 0; i < n; ++i)
            samples[i] = process(samples[i]);
    }

private:
    T pole_ = T(0.995);
    T last_in_ = T(0);
    T last_out_ = T(0);
};

} // namespace pulp::signal
