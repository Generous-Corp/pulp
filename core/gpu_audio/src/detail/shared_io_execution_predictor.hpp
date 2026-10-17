#pragma once

#include <cstdint>
#include <limits>

namespace pulp::gpu_audio::detail {

// A calibration cell is deliberately scalar and immutable.  The caller owns
// the model/provider hash values; this private packet never opens a model or
// provider registry and therefore cannot accidentally widen the public ABI.
struct SharedIoCalibrationIdentity {
    std::uint64_t model = 0;
    std::uint32_t block_frames = 0;
    std::uint32_t lead_blocks = 0;
    std::uint32_t slots = 0;
    std::uint32_t batch_width = 0;
    std::uint64_t policy = 0;
    std::uint64_t load = 0;

    constexpr bool valid() const noexcept {
        return model != 0 && block_frames != 0 && lead_blocks != 0 && slots != 0 &&
               batch_width != 0 && policy != 0 && load != 0;
    }
    friend constexpr bool operator==(const SharedIoCalibrationIdentity&,
                                     const SharedIoCalibrationIdentity&) = default;
};

enum class SharedIoPredictionRefusal : std::uint8_t {
    None,
    Disabled,
    MissingCalibration,
    StaleCalibration,
    DeadlineExpired,
    BoundExceeded,
};

struct SharedIoCalibrationObservation {
    SharedIoCalibrationIdentity identity;
    std::uint64_t worker_ns = 0;
    std::uint64_t submitted_ns = 0;
    std::uint64_t terminal_ns = 0;
    std::uint64_t visible_ns = 0;
    std::uint64_t predicted_ns = 0;
    bool provider_authenticated = false;

    constexpr bool timestamps_synchronized() const noexcept {
        return worker_ns != 0 && submitted_ns >= worker_ns && terminal_ns >= submitted_ns &&
               visible_ns >= terminal_ns;
    }
    constexpr bool valid() const noexcept {
        return identity.valid() && provider_authenticated && timestamps_synchronized();
    }
    constexpr std::uint64_t execution_ns() const noexcept {
        return terminal_ns >= submitted_ns ? terminal_ns - submitted_ns : 0;
    }
    constexpr bool underestimates() const noexcept {
        return predicted_ns != 0 && execution_ns() > predicted_ns;
    }
};

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
        std::uint64_t underestimates = 0;
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
        underestimates_ = 0;
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

    void observe(const SharedIoCalibrationObservation& observation) noexcept {
        if (!observation.valid())
            return;
        if (observation.underestimates() && underestimates_ != UINT64_MAX)
            ++underestimates_;
        observe(observation.execution_ns());
    }

    void observe_execution(std::uint64_t elapsed_ns, std::uint64_t predicted_ns) noexcept {
        if (predicted_ns != 0 && elapsed_ns > predicted_ns && underestimates_ != UINT64_MAX)
            ++underestimates_;
        observe(elapsed_ns);
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
        out.underestimates = underestimates_;
        out.ready = config_.enabled && samples_ >= config_.minimum_samples;
        return out;
    }

    // Returns true when a new submission can fit before its explicit deadline.
    // A zero deadline is the established non-deadline/offline path. An
    // An enabled nonzero-deadline policy fails closed until enough successful
    // observations exist. Callers can use the established zero-deadline
    // offline/warm-up path to collect calibration samples.
    bool admit(std::uint64_t now_ns, std::uint64_t deadline_ns) const noexcept {
        if (!config_.enabled || deadline_ns == 0)
            return true;
        if (deadline_ns <= now_ns)
            return false;
        const auto prediction = estimate();
        if (!prediction.ready)
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
    std::uint64_t underestimates_ = 0;
};

} // namespace pulp::gpu_audio::detail
