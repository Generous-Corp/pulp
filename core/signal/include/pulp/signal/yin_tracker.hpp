#pragma once

/// @file yin_tracker.hpp
/// Standalone monophonic YIN pitch tracker used by the harmony engine.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace pulp::signal {

// ── Stage 1 — YIN pitch tracking ──────────────────────────────────────────

/// Monophonic fundamental-frequency estimator, after de Cheveigné & Kawahara
/// 2002 [1], steps 1–5.
///
/// RT contract: `prepare(sample_rate)` sizes the analysis buffers and is the
/// only allocating call. `process`, `set_*`, and `reset` allocate nothing, take
/// no locks, and never throw. The per-hop analysis is a burst of work on ONE
/// sample in every `kHop` — see `cost_mac_per_sample` for what that costs and
/// why you should not instantiate thirty of these.
///
/// Steps 1–5 are the paper's. Step 6 (best-local-estimate) is **omitted** and
/// replaced by a 3-tap median across hops, which is an original heuristic and
/// cheaper — it kills the single-frame octave flips that are YIN's dominant
/// failure mode at the cost of one extra hop of settling. That substitution is
/// a design choice and is NOT a claim to implement the paper's step 6.
template <typename SampleType = float>
class YinTrackerT {
public:
    /// Lowest tracked fundamental. Covers a bass/guitar low E (~82 Hz) and a
    /// low male voice. Sets the analysis window, and therefore the latency.
    /// [design parameter] default 80 Hz, range 50 .. 200 Hz.
    static constexpr double kF0MinDefault = 80.0;
    static constexpr double kF0MinLimit = 50.0;
    static constexpr double kF0MaxLimit = 2000.0;

    /// Highest tracked fundamental. Covers soprano and the guitar's upper
    /// register. [design parameter] default 1000 Hz, range 500 .. 2000 Hz.
    static constexpr double kF0MaxDefault = 1000.0;

    /// The absolute threshold on the cumulative mean normalized difference.
    /// **Published**: [1] §III.D uses 0.10. Range 0.05 .. 0.20 is the paper's
    /// own sensitivity discussion, not a free knob.
    static constexpr double kYinThreshold = 0.10;

    /// Analysis hop — the control cadence. 256 samples at 48 kHz is ~187
    /// estimates/s. [design parameter] default 256, range 128 .. 512.
    static constexpr int kHop = 256;

    /// Median taps for octave-flip rejection.
    /// [design parameter] default 3, range {1 (off), 3, 5}.
    static constexpr int kMedianTaps = 3;
    static constexpr int kMaxMedianTaps = 5;

    void prepare(double sample_rate) {
        sample_rate_ = (std::isfinite(sample_rate) && sample_rate > 0.0)
                           ? sample_rate
                           : 48000.0;
        // W, tau_min and tau_max are DERIVED here, never literals.
        tau_min_ = static_cast<int>(std::ceil(sample_rate_ / f0_max_));
        tau_max_ = static_cast<int>(std::ceil(sample_rate_ / f0_min_));
        tau_min_ = std::max(tau_min_, 2);
        tau_max_ = std::max(tau_max_, tau_min_ + 1);
        window_ = 2 * tau_max_;

        // The integration length is what makes the buffer self-consistent: the
        // difference function reads `x[j + tau]` for j < integration_ and
        // tau <= tau_max_, so the largest index touched is
        // integration_ - 1 + tau_max_ = window_ - 1 — the last sample in the
        // window. Integrating over the FULL window instead (as the difference
        // function is often written) would read tau_max_ samples past the end
        // of the very window whose length the latency is derived from.
        integration_ = window_ - tau_max_;

        ring_.assign(static_cast<std::size_t>(window_), 0.0);
        scratch_.assign(static_cast<std::size_t>(window_), 0.0);
        diff_.assign(static_cast<std::size_t>(tau_max_) + 1, 0.0);
        cmnd_.assign(static_cast<std::size_t>(tau_max_) + 1, 0.0);
        reset();
    }

    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.0);
        valid_samples_ = 0;
        write_ = 0;
        hop_counter_ = 0;
        median_count_ = 0;
        median_index_ = 0;
        f0_hz_ = 0.0;
        tau_ = 0.0;
        voiced_ = false;
        min_cmnd_ = 1.0;
    }

    /// Constant-time logical reset for audio-thread fault recovery.
    void discard_history() noexcept {
        write_ = 0;
        valid_samples_ = 0;
        hop_counter_ = 0;
        median_count_ = 0;
        median_index_ = 0;
        f0_hz_ = 0.0;
        tau_ = 0.0;
        voiced_ = false;
        min_cmnd_ = 1.0;
    }

    /// Sets the tracked range. Takes effect at the next `prepare`, because the
    /// window length — and therefore the reported latency and the buffer sizes
    /// — are derived from it. Changing latency mid-stream would be a worse
    /// surprise than requiring a re-prepare.
    void set_f0_range(double min_hz, double max_hz) {
        if (!std::isfinite(min_hz) || !std::isfinite(max_hz)) return;
        const double lo = std::clamp(min_hz, kF0MinLimit, kF0MaxLimit);
        const double hi = std::clamp(max_hz, kF0MinLimit, kF0MaxLimit);
        if (hi <= lo) return;
        f0_min_ = lo;
        f0_max_ = hi;
    }

    double f0_min_hz() const { return f0_min_; }
    double f0_max_hz() const { return f0_max_; }

    /// Feeds one sample. Returns true on the samples where a NEW estimate
    /// landed, so a caller can re-run its control chain only on those.
    bool process(SampleType x) {
        if (window_ <= 0) return false;
        if (!std::isfinite(static_cast<double>(x))) {
            discard_history();
            return false;
        }
        ring_[static_cast<std::size_t>(write_)] = static_cast<double>(x);
        write_ = (write_ + 1) % window_;
        valid_samples_ = std::min(valid_samples_ + 1, window_);
        if (++hop_counter_ < kHop) return false;
        hop_counter_ = 0;
        analyse();
        return true;
    }

    /// The current estimate. Held through unvoiced frames rather than dropping
    /// to zero, so a caller that ignores `voiced()` still gets a sane number.
    double f0_hz() const { return f0_hz_; }

    /// The refined period in samples, before the Hz conversion.
    double tau_samples() const { return tau_; }

    /// False when the frame carried no stable period (`min d' > kYinThreshold`).
    bool voiced() const { return voiced_; }

    /// The frame's minimum CMND — the tracker's own confidence reading.
    double min_cmnd() const { return min_cmnd_; }

    /// Series law 5: the estimate for a frame needs the whole window, so this
    /// is `W`. There is no zero-latency variant of a period-length
    /// autocorrelation. Report it; do not hide it.
    int latency_samples() const { return window_; }

    int window_samples() const { return window_; }
    int integration_samples() const { return integration_; }
    int tau_min() const { return tau_min_; }
    int tau_max() const { return tau_max_; }
    static constexpr int hop_samples() { return kHop; }

    /// Amortized analysis cost, so a caller can see before profiling that this
    /// is a one-per-track block rather than a per-voice one.
    double cost_mac_per_sample() const {
        return static_cast<double>(tau_max_) * static_cast<double>(integration_) /
               static_cast<double>(kHop);
    }

private:
    /// One hop of analysis: difference function, CMND, absolute threshold,
    /// parabolic refinement, median.
    void analyse() {
        // Unroll the ring into a linear window so the difference function is a
        // flat scan rather than `tau_max * integration` modulo operations.
        const int missing = window_ - valid_samples_;
        for (int i = 0; i < window_; ++i) {
            scratch_[static_cast<std::size_t>(i)] =
                i < missing ? 0.0
                            : ring_[static_cast<std::size_t>((write_ + i) % window_)];
        }

        // [1] Eq. 6 — the difference function. Computed from tau = 1, not from
        // tau_min: the CMND denominator below is a running mean over ALL lags
        // from 1, so starting at tau_min would normalise against a truncated
        // sum and shift every d' value.
        diff_[0] = 0.0;
        for (int tau = 1; tau <= tau_max_; ++tau) {
            double sum = 0.0;
            for (int j = 0; j < integration_; ++j) {
                const double d = scratch_[static_cast<std::size_t>(j)] -
                                 scratch_[static_cast<std::size_t>(j + tau)];
                sum += d * d;
            }
            diff_[static_cast<std::size_t>(tau)] = sum;
        }

        // [1] Eq. 8 — cumulative mean normalized difference. This is YIN's key
        // step: it removes plain autocorrelation's "zero lag always wins" bias
        // and leaves a ~1.0 baseline with dips at the period and its multiples.
        cmnd_[0] = 1.0;
        double running = 0.0;
        for (int tau = 1; tau <= tau_max_; ++tau) {
            running += diff_[static_cast<std::size_t>(tau)];
            cmnd_[static_cast<std::size_t>(tau)] =
                running > 0.0 ? diff_[static_cast<std::size_t>(tau)] *
                                    static_cast<double>(tau) / running
                              : 1.0;
        }

        // [1] Eq. 9 — absolute threshold. Take the FIRST dip below the
        // threshold and descend to its bottom, rather than the global minimum,
        // which is what stops a period multiple from winning.
        int best = -1;
        for (int tau = tau_min_; tau <= tau_max_; ++tau) {
            if (cmnd_[static_cast<std::size_t>(tau)] < kYinThreshold) {
                while (tau + 1 <= tau_max_ &&
                       cmnd_[static_cast<std::size_t>(tau + 1)] <
                           cmnd_[static_cast<std::size_t>(tau)])
                    ++tau;
                best = tau;
                break;
            }
        }

        double lowest = cmnd_[static_cast<std::size_t>(tau_min_)];
        if (best < 0) {
            int arg = tau_min_;
            for (int tau = tau_min_ + 1; tau <= tau_max_; ++tau) {
                if (cmnd_[static_cast<std::size_t>(tau)] < lowest) {
                    lowest = cmnd_[static_cast<std::size_t>(tau)];
                    arg = tau;
                }
            }
            best = arg;
        } else {
            lowest = cmnd_[static_cast<std::size_t>(best)];
        }
        min_cmnd_ = lowest;

        // §3.5 voicing gate.
        voiced_ = lowest <= kYinThreshold;
        if (!voiced_) return;  // hold the last f0

        // [1] step 5 — parabolic interpolation on the CMND around the dip.
        const double refined = parabolic(best);

        // The octave-error median (ORIGINAL heuristic, not [1] step 6).
        median_buffer_[static_cast<std::size_t>(median_index_)] = refined;
        median_index_ = (median_index_ + 1) % kMedianTaps;
        if (median_count_ < kMedianTaps) ++median_count_;

        tau_ = median_of_buffer();
        f0_hz_ = tau_ > 0.0 ? sample_rate_ / tau_ : 0.0;
    }

    /// Sub-sample refinement of the dip at `tau`. Guards both the window edges
    /// and a flat/degenerate parabola, either of which would otherwise produce
    /// a divide-by-zero or an offset outside the bracketing samples.
    double parabolic(int tau) const {
        if (tau <= tau_min_ || tau >= tau_max_) return static_cast<double>(tau);
        const double a = cmnd_[static_cast<std::size_t>(tau - 1)];
        const double b = cmnd_[static_cast<std::size_t>(tau)];
        const double c = cmnd_[static_cast<std::size_t>(tau + 1)];
        const double denom = a - 2.0 * b + c;
        if (!(std::abs(denom) > 1e-18)) return static_cast<double>(tau);
        const double offset = 0.5 * (a - c) / denom;
        // A correct parabolic vertex on a genuine minimum lands inside ±0.5;
        // anything further means the three points were not a dip.
        if (!(std::abs(offset) <= 1.0)) return static_cast<double>(tau);
        return static_cast<double>(tau) + offset;
    }

    double median_of_buffer() const {
        double sorted[kMaxMedianTaps];
        const int n = median_count_;
        for (int i = 0; i < n; ++i)
            sorted[i] = median_buffer_[static_cast<std::size_t>(i)];
        std::sort(sorted, sorted + n);
        return sorted[n / 2];
    }

    double sample_rate_ = 48000.0;
    double f0_min_ = kF0MinDefault;
    double f0_max_ = kF0MaxDefault;

    int tau_min_ = 0;
    int tau_max_ = 0;
    int window_ = 0;
    int integration_ = 0;

    std::vector<double> ring_{};
    std::vector<double> scratch_{};
    std::vector<double> diff_{};
    std::vector<double> cmnd_{};

    int write_ = 0;
    int valid_samples_ = 0;
    int hop_counter_ = 0;

    double median_buffer_[kMaxMedianTaps] = {};
    int median_index_ = 0;
    int median_count_ = 0;

    double f0_hz_ = 0.0;
    double tau_ = 0.0;
    double min_cmnd_ = 1.0;
    bool voiced_ = false;
};

using YinTracker = YinTrackerT<float>;
using YinTracker64 = YinTrackerT<double>;

}  // namespace pulp::signal

