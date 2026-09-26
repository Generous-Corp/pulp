#pragma once

/// @file fir_filter.hpp
/// Finite Impulse Response filter with configurable order and coefficients.

#include <pulp/simd/simd.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numeric>
#include <vector>

namespace pulp::signal {

/// FIR filter with arbitrary coefficients.
///
/// History is kept linear (oldest to newest, contiguous) rather than in a
/// wrapping ring, so every output is one contiguous dot product and a block of
/// outputs is one pulp::simd::correlate() call, which the SIMD backends
/// vectorize across outputs. Appending past the end of the history buffer
/// moves the last `taps - 1` samples back to the front, once per
/// kBlockCapacity samples.
///
/// Numerics: taps accumulate oldest-first in the backend's order, so output
/// matches a textbook sequential sum to rounding, not to the bit.
///
/// RT contract: `set_coefficients()` allocates and must run off the audio
/// thread. `process()` and `reset()` allocate no memory after coefficients are
/// installed.
///
/// @code
/// FirFilter fir;
/// fir.set_coefficients({0.25f, 0.5f, 0.25f}); // 3-tap averaging
/// float out = fir.process(in);
/// @endcode
template <typename SampleType = float>
class FirFilterT {
public:
  /// Samples appended between history compactions. A block longer than this
  /// is processed in slices of at most this many outputs.
  static constexpr std::size_t kBlockCapacity = 512;

  /// Per-sample calls with at most this many taps sum inline instead of
  /// calling the dot kernel, whose call overhead would dominate.
  static constexpr std::size_t kInlineTaps = 8;

  FirFilterT() = default;

  /// Set filter coefficients. The number of taps equals coefficients.size().
  void set_coefficients(std::vector<SampleType> coeffs) {
      coefficients_ = std::move(coeffs);
      reversed_.assign(coefficients_.rbegin(), coefficients_.rend());
      const std::size_t taps = coefficients_.size();
      history_.assign(taps == 0 ? 0 : (taps - 1) + kBlockCapacity, SampleType{0});
      head_ = taps == 0 ? 0 : taps - 1;
  }

    /// Get current coefficient count (filter order + 1).
    int order() const { return static_cast<int>(coefficients_.size()); }

    /// The taps in time order (h[0] multiplies the newest input).
    const std::vector<SampleType>& coefficients() const noexcept {
        return coefficients_;
    }

    /// Process a single sample.
    SampleType process(SampleType input) {
        const std::size_t taps = coefficients_.size();
        if (taps == 0)
            return input;
        if (head_ == history_.size())
            compact();
        history_[head_] = input;
        const SampleType* window = history_.data() + head_ + 1 - taps;
        ++head_;
        if (taps <= kInlineTaps) {
            SampleType output = SampleType{0};
            for (std::size_t k = 0; k < taps; ++k)
                output += window[k] * reversed_[k];
            return output;
        }
        return pulp::simd::dot(window, reversed_.data(), taps);
    }

    /// Process a buffer of samples in-place.
    void process(SampleType* data, int num_samples) {
        const std::size_t taps = coefficients_.size();
        if (taps == 0 || num_samples <= 0)
            return;
        const auto total = static_cast<std::size_t>(num_samples);
        std::size_t done = 0;
        while (done < total) {
            if (head_ == history_.size())
                compact();
            const std::size_t count = std::min(history_.size() - head_, total - done);
            std::copy_n(data + done, count, history_.data() + head_);
            pulp::simd::correlate(history_.data() + head_ + 1 - taps, reversed_.data(), data + done,
                                  count, taps);
            head_ += count;
            done += count;
        }
    }

    /// Reset the internal delay buffer to zero.
    void reset() {
        std::fill(history_.begin(), history_.end(), SampleType{0.0f});
        head_ = coefficients_.empty() ? 0 : coefficients_.size() - 1;
    }

    // ── Coefficient generators ──────────────────────────────────────────

    /// Generate lowpass FIR coefficients using windowed-sinc method.
    /// @param num_taps Number of filter taps (odd recommended).
    /// @param cutoff_hz Cutoff frequency in Hz.
    /// @param sample_rate Sample rate in Hz.
    static std::vector<SampleType> lowpass(
        int num_taps, SampleType cutoff_hz, SampleType sample_rate) {
        std::vector<SampleType> h(static_cast<size_t>(num_taps));
        SampleType fc = cutoff_hz / sample_rate;
        int m = num_taps - 1;
        constexpr SampleType pi = SampleType{3.14159265358979323846f};

        for (int i = 0; i <= m; ++i) {
            SampleType n = static_cast<SampleType>(i) -
                           static_cast<SampleType>(m) / SampleType{2.0f};
            if (std::abs(n) < SampleType{1e-6f}) {
                h[static_cast<size_t>(i)] = SampleType{2.0f} * fc;
            } else {
                h[static_cast<size_t>(i)] =
                    std::sin(SampleType{2.0f} * pi * fc * n) / (pi * n);
            }
            // Hamming window
            SampleType w = SampleType{0.54f} -
                SampleType{0.46f} *
                    std::cos(SampleType{2.0f} * pi * static_cast<SampleType>(i) /
                             static_cast<SampleType>(m));
            h[static_cast<size_t>(i)] *= w;
        }

        // Normalize
        SampleType sum = std::accumulate(h.begin(), h.end(), SampleType{0.0f});
        if (sum > SampleType{0.0f}) {
            for (auto& v : h) v /= sum;
        }
        return h;
    }

    /// Generate highpass FIR coefficients using spectral inversion.
    static std::vector<SampleType> highpass(
        int num_taps, SampleType cutoff_hz, SampleType sample_rate) {
        auto h = lowpass(num_taps, cutoff_hz, sample_rate);
        for (auto& v : h) v = -v;
        h[static_cast<size_t>(num_taps / 2)] += SampleType{1.0f};
        return h;
    }

private:
  // Keeps the newest taps - 1 inputs and makes room at the end.
  void compact() noexcept {
      const std::size_t keep = coefficients_.size() - 1;
      std::copy(history_.begin() + static_cast<std::ptrdiff_t>(head_ - keep),
                history_.begin() + static_cast<std::ptrdiff_t>(head_), history_.begin());
      head_ = keep;
  }

    std::vector<SampleType> coefficients_;
    std::vector<SampleType> reversed_;
    // [0, head_) holds past input, oldest first; head_ >= taps - 1 always.
    std::vector<SampleType> history_;
    std::size_t head_ = 0;
};

using FirFilter = FirFilterT<float>;
using FirFilter64 = FirFilterT<double>;

} // namespace pulp::signal
