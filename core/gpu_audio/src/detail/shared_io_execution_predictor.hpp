#pragma once

#include <cstdint>
#include <limits>

namespace pulp::gpu_audio::detail {

// Private, allocation-free admission policy for a prepared shared-I/O plan.
// This is deliberately a conservative wall-clock predictor: until an
// explicitly enabled policy has enough observations, it never changes the
// established admission behavior. Observations are provider/plan-local and
// must not be treated as authenticated GPU timestamps.
class SharedIoExecutionPredictor final {
  public:
    struct Config {
        bool enabled = false;
        std::uint32_t minimum_samples = 8;
        std::uint64_t safety_margin_ns = 0;
    };

    struct Estimate {
        bool enabled = false;
        std::uint32_t samples = 0;
        std::uint64_t mean_ns = 0;
        std::uint64_t deviation_ns = 0;
        std::uint64_t conservative_ns = 0;
        bool ready = false;
    };

    void configure(Config config) noexcept {
        config_ = config;
        if (config_.minimum_samples == 0)
            config_.minimum_samples = 1;
    }

    void reset() noexcept {
        samples_ = 0;
        mean_ns_ = 0;
        deviation_ns_ = 0;
        peak_ns_ = 0;
    }

    void observe(std::uint64_t elapsed_ns) noexcept {
        if (!config_.enabled || elapsed_ns == 0)
            return;
        if (elapsed_ns > peak_ns_)
            peak_ns_ = elapsed_ns;
        if (samples_ == 0) {
            mean_ns_ = elapsed_ns;
            deviation_ns_ = 0;
        } else {
            // A bounded EWMA keeps this suitable for the serialized
            // dispatcher and responds to thermal/contention changes without
            // retaining an unbounded sample history.
            if (elapsed_ns >= mean_ns_)
                mean_ns_ += (elapsed_ns - mean_ns_) / 8;
            else
                mean_ns_ -= (mean_ns_ - elapsed_ns) / 8;
            const auto delta =
                mean_ns_ > elapsed_ns ? mean_ns_ - elapsed_ns : elapsed_ns - mean_ns_;
            if (delta >= deviation_ns_)
                deviation_ns_ += (delta - deviation_ns_) / 8;
            else
                deviation_ns_ -= (deviation_ns_ - delta) / 8;
        }
        if (samples_ != UINT32_MAX)
            ++samples_;
    }

    Estimate estimate() const noexcept {
        Estimate out;
        out.enabled = config_.enabled;
        out.samples = samples_;
        out.mean_ns = mean_ns_;
        out.deviation_ns = deviation_ns_;
        const auto max_value = std::numeric_limits<std::uint64_t>::max();
        const auto uncertainty = deviation_ns_ > max_value / 3 ? max_value : deviation_ns_ * 3;
        const auto bounded =
            mean_ns_ > max_value - uncertainty ? max_value : mean_ns_ + uncertainty;
        // Retain the largest observed wall-clock duration as a hard floor for
        // the estimate. This prevents a sudden contention/thermal sample from
        // being hidden by the EWMA and is still only a local admission hint.
        const auto conservative = bounded < peak_ns_ ? peak_ns_ : bounded;
        out.conservative_ns = conservative > max_value - config_.safety_margin_ns
                                  ? max_value
                                  : conservative + config_.safety_margin_ns;
        out.ready = config_.enabled && samples_ >= config_.minimum_samples;
        return out;
    }

    // Returns true when a new submission can fit before its explicit deadline.
    // A zero deadline is the established non-deadline/offline path.
    bool admit(std::uint64_t now_ns, std::uint64_t deadline_ns) const noexcept {
        if (!config_.enabled || deadline_ns == 0)
            return true;
        const auto prediction = estimate();
        if (!prediction.ready)
            return true;
        if (deadline_ns <= now_ns)
            return false;
        return prediction.conservative_ns <= deadline_ns - now_ns;
    }

    Config config() const noexcept {
        return config_;
    }

  private:
    Config config_;
    std::uint32_t samples_ = 0;
    std::uint64_t mean_ns_ = 0;
    std::uint64_t deviation_ns_ = 0;
    std::uint64_t peak_ns_ = 0;
};

} // namespace pulp::gpu_audio::detail
