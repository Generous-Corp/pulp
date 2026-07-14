#pragma once

/// @file sinc_resampler.hpp
/// Fractional-delay resampling by a Kaiser-windowed sinc kernel.
///
/// Reading a buffer at an arbitrary fractional position is an
/// interpolation problem. Cubic (Catmull-Rom) interpolation is cheap but
/// its stopband rejection is poor, so resampling a signal — e.g. the
/// resample step of phase-vocoder pitch shifting — folds high-frequency
/// energy back as audible aliasing, worst on large shifts and bright
/// material. A windowed-sinc kernel is the band-limited reconstruction
/// filter: an ideal sinc (the perfect interpolator) truncated to a finite
/// half-width and tapered by a Kaiser window to trade transition width
/// against stopband depth (Smith, "Digital Audio Resampling," CCRMA;
/// Kaiser & Schafer 1980 for the β/attenuation relation).
///
/// Implementation: a precomputed table of the windowed sinc, oversampled
/// in the fractional-phase dimension and read with linear interpolation
/// between phase entries (the standard table-driven resampler). For a
/// fractional source position `p`, the output is the sum over `2*half`
/// neighboring input samples weighted by the kernel evaluated at their
/// distance from `p`. Real-time-safe after build(); no allocation in
/// `read()`.
///
/// RT contract: build() allocates the kernel table and is not audio-thread
/// safe. apply(), read(), and accessors are allocation-free after build().

#include <cmath>
#include <cstddef>
#include <vector>

namespace pulp::signal {

template <typename SampleType = float>
class SincResamplerT {
public:
    /// Build the kernel. `half_width` taps each side (quality vs cost; 16 is
    /// a good default), `phases` sub-sample resolution (table rows; 512 is
    /// transparent with linear phase interpolation), `beta` the Kaiser shape
    /// (≈ 8–10 for deep stopband; higher = more rejection, wider transition).
    void build(int half_width = 16, int phases = 512, double beta = 9.0) {
        half_ = half_width;
        phases_ = phases;
        const int taps = 2 * half_;
        // table_[phase][tap]; phase in [0, phases_] inclusive (the extra row
        // lets read() linearly interpolate up to phase == phases_).
        table_.assign(static_cast<size_t>(phases_ + 1) * taps, SampleType{0.0f});
        const double i0_beta = bessel_i0(beta);
        for (int ph = 0; ph <= phases_; ++ph) {
            const double frac = static_cast<double>(ph) / phases_;
            for (int t = 0; t < taps; ++t) {
                // Distance from the interpolation point to tap t. Taps span
                // [-half+1 .. half], so the sample at index (i0 + t - half + 1)
                // sits at offset (t - half + 1 - frac) from the read point.
                const double x = static_cast<double>(t - half_ + 1) - frac;
                const double s = sinc(x);
                // Kaiser window over the kernel support [-half, half].
                const double wn = x / half_;
                const double w =
                    (wn > -1.0 && wn < 1.0)
                        ? bessel_i0(beta * std::sqrt(1.0 - wn * wn)) / i0_beta
                        : 0.0;
                table_[static_cast<size_t>(ph) * taps + t] =
                    static_cast<SampleType>(s * w);
            }
        }
    }

    int half_width() const { return half_; }
    int taps() const { return 2 * half_; }
    bool ready() const { return !table_.empty(); }

    /// Apply the kernel at fractional phase `frac` (in [0,1)) to exactly
    /// `taps()` contiguous samples, ordered from (i0-half+1) to (i0+half)
    /// where the read point is at i0+frac. Lets a caller with its own buffer
    /// layout (e.g. a power-of-two ring) gather the neighbourhood itself and
    /// reuse the kernel. RT-safe.
    SampleType apply(const SampleType* samples, double frac) const {
        const int taps = 2 * half_;
        const double ph = frac * phases_;
        const int p0 = static_cast<int>(ph);
        const SampleType a = static_cast<SampleType>(ph - p0);
        const SampleType* row0 = table_.data() + static_cast<size_t>(p0) * taps;
        const SampleType* row1 = table_.data() + static_cast<size_t>(p0 + 1) * taps;
        SampleType acc = SampleType{0.0f};
        for (int t = 0; t < taps; ++t)
            acc += (row0[t] + a * (row1[t] - row0[t])) * samples[t];
        return acc;
    }

    /// Read `src` at fractional position `pos` (in samples). The caller
    /// guarantees the kernel support `[floor(pos)-half+1, floor(pos)+half]`
    /// lies within `[0, len)`; out-of-range taps are clamped to the edge so
    /// boundary reads degrade gracefully rather than read out of bounds.
    SampleType read(const SampleType* src, int len, double pos) const {
        const int taps = 2 * half_;
        const long i0 = static_cast<long>(std::floor(pos));
        const double frac = pos - static_cast<double>(i0);
        // Phase table lookup with linear interpolation between rows.
        const double ph = frac * phases_;
        const int p0 = static_cast<int>(ph);
        const SampleType a = static_cast<SampleType>(ph - p0);
        const SampleType* row0 = table_.data() + static_cast<size_t>(p0) * taps;
        const SampleType* row1 = table_.data() + static_cast<size_t>(p0 + 1) * taps;
        SampleType acc = SampleType{0.0f};
        for (int t = 0; t < taps; ++t) {
            long idx = i0 + t - half_ + 1;
            if (idx < 0) idx = 0;
            else if (idx >= len) idx = len - 1;
            const SampleType k = row0[t] + a * (row1[t] - row0[t]);
            acc += k * src[idx];
        }
        return acc;
    }

private:
    static double sinc(double x) {
        if (std::abs(x) < 1e-9) return 1.0;
        const double px = 3.14159265358979323846 * x;
        return std::sin(px) / px;
    }
    // Modified Bessel function of the first kind, order 0 (series).
    static double bessel_i0(double x) {
        double sum = 1.0, term = 1.0;
        const double half_x = x * 0.5;
        for (int k = 1; k < 40; ++k) {
            term *= (half_x / k) * (half_x / k);
            sum += term;
            if (term < 1e-12 * sum) break;
        }
        return sum;
    }

    int half_ = 0;
    int phases_ = 0;
    std::vector<SampleType> table_;
};

using SincResampler = SincResamplerT<float>;
using SincResampler64 = SincResamplerT<double>;

} // namespace pulp::signal
