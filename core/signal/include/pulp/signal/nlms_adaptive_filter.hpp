#pragma once

/// @file nlms_adaptive_filter.hpp
/// A bounded, prepared normalized least-mean-squares adaptive FIR.
///
/// The signal convention is explicit: `reference` is r[n], `primary` is
/// d[n], `estimate` is sum(w[i] * r[n-i]), and `error` is d[n] - estimate.
/// RT contract: after prepare(), processing, reset, parameter changes, and
/// coefficient publication perform no allocation, locking, or I/O. The
/// control-side snapshot is bounded and never exposes live coefficient state.
/// One realtime writer owns processing and parameter/reset calls; at most one
/// control reader may call try_snapshot_coefficients() concurrently.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>

namespace pulp::signal {

template <typename SampleType = float>
class NlmsAdaptiveFilterT {
public:
    struct Result {
        SampleType estimate{};
        SampleType error{};
    };

    /// Allocate fixed storage. Tap capacity is recovered into [1, 8192]; an
    /// invalid sample rate leaves processing fail-closed until the next valid
    /// prepare(). Repeated prepare is supported and resets all state.
    void prepare(double sample_rate, int max_taps) {
        max_taps_ = std::clamp(max_taps, 1, kMaxTaps);
        sample_rate_ = std::isfinite(sample_rate) && sample_rate > 0.0 ? sample_rate : 0.0;
        history_.assign(static_cast<std::size_t>(max_taps_), 0.0);
        weights_.assign(static_cast<std::size_t>(max_taps_), 0.0);
        published_.assign(static_cast<std::size_t>(kPublishBuffers * max_taps_), 0.0);
        active_taps_ = max_taps_;
        write_pos_ = 0;
        publish_index_.store(0, std::memory_order_relaxed);
        reader_index_.store(-1, std::memory_order_relaxed);
        for (auto& count : published_taps_)
            count.store(0, std::memory_order_relaxed);
        reset();
    }

    bool set_active_taps(int taps) noexcept {
        if (taps < 1 || taps > max_taps_) return false;
        active_taps_ = taps;
        publish_current();
        return true;
    }

    /// Clamp finite mu into the open NLMS stability interval (0, 2). NaN/Inf
    /// recovers to 0.5.
    void set_step_size(SampleType mu) noexcept {
        const double v = finite_or(static_cast<double>(mu), 0.5);
        mu_ = std::clamp(v, kParameterFloor, 2.0 - kParameterFloor);
    }

    /// Clamp the normalization floor to a finite, positive lower bound.
    void set_denominator_floor(SampleType eps) noexcept {
        const double v = finite_or(static_cast<double>(eps), 1.0e-8);
        denominator_floor_ = std::max(v, kParameterFloor);
    }

    /// Per-sample loss in [0, 1): the complete normalized update is retained
    /// by (1 - leakage), preserving bounded behavior at legal parameter edges.
    void set_leakage(SampleType leakage) noexcept {
        const double v = finite_or(static_cast<double>(leakage), 0.0);
        leakage_ = std::clamp(v, 0.0, 1.0 - kParameterFloor);
    }

    void set_adapt_enabled(bool enabled) noexcept { adapt_enabled_ = enabled; }

    void reset() noexcept {
        std::fill(history_.begin(), history_.end(), 0.0);
        std::fill(weights_.begin(), weights_.end(), 0.0);
        write_pos_ = 0;
        // Publish through the same ownership protocol as processing. Clearing
        // every slot here would let a control reader race a reset; an old
        // stable snapshot is preferable to a torn one and is replaced by the
        // next writer-owned slot atomically.
        publish_current();
    }

    Result process_sample(SampleType primary, SampleType reference) noexcept {
        if (max_taps_ == 0 || sample_rate_ <= 0.0 ||
            !std::isfinite(static_cast<double>(primary)) ||
            !std::isfinite(static_cast<double>(reference)))
            return {};

        const double primary_value = flush_denormal(static_cast<double>(primary));
        const double reference_value = flush_denormal(static_cast<double>(reference));
        history_[static_cast<std::size_t>(write_pos_)] = reference_value;
        write_pos_ = (write_pos_ + 1) % max_taps_;
        double estimate = 0.0;
        double power = 0.0;
        for (int i = 0; i < active_taps_; ++i) {
            const double x = history_at(i);
            estimate += weights_[static_cast<std::size_t>(i)] * x;
            power += x * x;
        }
        const double error = primary_value - estimate;
        if (adapt_enabled_) {
            const double step = mu_ * error / std::max(denominator_floor_ + power,
                                                        std::numeric_limits<double>::min());
            const double retain = 1.0 - leakage_;
            for (int i = 0; i < active_taps_; ++i) {
                double& w = weights_[static_cast<std::size_t>(i)];
                // Apply leakage to the complete normalized update. Keeping
                // the update outside `retain` makes the legal corner
                // mu -> 2, leakage -> 1 an unstable gain of almost -2.
                w = retain * (w + step * history_at(i));
                if (!std::isfinite(w) || std::abs(w) <= kDenormalFloor) w = 0.0;
            }
        }
        publish_current();
        return {static_cast<SampleType>(estimate), static_cast<SampleType>(error)};
    }

    void process_block(const SampleType* primary, const SampleType* reference,
                       Result* out, int n) noexcept {
        if (n <= 0 || primary == nullptr || reference == nullptr || out == nullptr) return;
        for (int i = 0; i < n; ++i) out[i] = process_sample(primary[i], reference[i]);
    }

    bool try_snapshot_coefficients(SampleType* dest, int capacity,
                                   int& active_taps_out) const noexcept {
        if (dest == nullptr || capacity < 0) return false;
        for (int attempt = 0; attempt < kSnapshotAttempts; ++attempt) {
            const int index = publish_index_.load(std::memory_order_acquire);
            if (index < 0 || index >= kPublishBuffers) continue;
            reader_index_.store(index, std::memory_order_release);
            if (publish_index_.load(std::memory_order_acquire) != index) {
                reader_index_.store(-1, std::memory_order_release);
                continue;
            }
            const int count = published_taps_[static_cast<std::size_t>(index)].load(
                std::memory_order_acquire);
            if (count < 0 || count > capacity) {
                reader_index_.store(-1, std::memory_order_release);
                return false;
            }
            const double* source = published_.data() + static_cast<std::size_t>(index * max_taps_);
            for (int i = 0; i < count; ++i) dest[i] = static_cast<SampleType>(source[i]);
            reader_index_.store(-1, std::memory_order_release);
            active_taps_out = count;
            return true;
        }
        return false;
    }

    int active_taps() const noexcept { return active_taps_; }
    int max_taps() const noexcept { return max_taps_; }
    double sample_rate() const noexcept { return sample_rate_; }
    int latency_samples() const noexcept { return 0; }
    std::size_t retained_bytes() const noexcept {
        // Three publication copies plus the live history and weights. The
        // vectors themselves are handles; this reports their actual backing
        // storage, which is double precision regardless of SampleType.
        return static_cast<std::size_t>(kPublishBuffers + 2) *
               static_cast<std::size_t>(max_taps_) * sizeof(double);
    }

private:
    static constexpr int kMaxTaps = 8192;
    static constexpr int kPublishBuffers = 3;
    static constexpr int kSnapshotAttempts = 4;
    static constexpr double kParameterFloor = 1.0e-12;
    static constexpr double kDenormalFloor = 1.0e-18;

    static double finite_or(double v, double fallback) noexcept {
        return std::isfinite(v) ? v : fallback;
    }

    static double flush_denormal(double v) noexcept {
        return std::abs(v) <= kDenormalFloor ? 0.0 : v;
    }

    double history_at(int delay) const noexcept {
        int index = write_pos_ - 1 - delay;
        if (index < 0) index += max_taps_ * ((-index / max_taps_) + 1);
        return history_[static_cast<std::size_t>(index % max_taps_)];
    }

    void publish_current() noexcept {
        if (max_taps_ == 0) return;
        const int current = publish_index_.load(std::memory_order_relaxed);
        const int reader = reader_index_.load(std::memory_order_acquire);
        int target = (current + 1) % kPublishBuffers;
        if (target == reader) target = (target + 1) % kPublishBuffers;
        double* destination = published_.data() + static_cast<std::size_t>(target * max_taps_);
        std::memcpy(destination, weights_.data(),
                    static_cast<std::size_t>(active_taps_) * sizeof(double));
        if (active_taps_ < max_taps_)
            std::fill(destination + active_taps_, destination + max_taps_, 0.0);
        published_taps_[static_cast<std::size_t>(target)].store(
            active_taps_, std::memory_order_release);
        publish_index_.store(target, std::memory_order_release);
    }

    double sample_rate_ = 0.0;
    int max_taps_ = 0;
    int active_taps_ = 0;
    int write_pos_ = 0;
    double mu_ = 0.5;
    double denominator_floor_ = 1.0e-8;
    double leakage_ = 0.0;
    bool adapt_enabled_ = true;
    std::vector<double> history_;
    std::vector<double> weights_;
    std::vector<double> published_;
    mutable std::atomic<int> publish_index_{0};
    mutable std::atomic<int> reader_index_{-1};
    mutable std::array<std::atomic<int>, kPublishBuffers> published_taps_{};
};

using NlmsAdaptiveFilter = NlmsAdaptiveFilterT<float>;
using NlmsAdaptiveFilter64 = NlmsAdaptiveFilterT<double>;

} // namespace pulp::signal
