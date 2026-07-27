#pragma once

#include <pulp/signal/detail/modular_sequencing_common.hpp>
#include <pulp/signal/trigger_kit.hpp>
#include <pulp/signal/units.hpp>

#include <cmath>
#include <cstdint>

namespace pulp::signal {

// ── Transport front end ───────────────────────────────────────────────────

/// Converts a run **level** and a reset **level** into the `(run_high,
/// reset_edge)` pair the sequencers consume, and optionally detects the clock
/// edge alongside them.
///
/// It exists so that "what is a reset edge" has exactly one answer in the
/// library. It carries no musical state at all: hand it the same three signals
/// and it produces the same three booleans regardless of which sequencer is
/// downstream.
///
/// The reset input gets a **refractory window** on top of the kit's hysteresis.
/// Hysteresis alone already rejects a clock hovering at the threshold, but a
/// reset jack is often driven by a physical switch or a long cable, where the
/// signal genuinely crosses the window several times in a millisecond. A
/// refractory window is the debounce for that, and it lives here rather than in
/// `TriggerDetectT` because a *clock* must never be debounced — a debounced
/// clock silently drops fast subdivisions.
///
/// RT contract: as the header. No allocation, no locks.
template <typename SampleType = float>
class TransportEdgeT {
public:
    /// Debounce window after a reset edge, during which further reset edges are
    /// suppressed. Purely mechanical — long enough to swallow switch bounce,
    /// far shorter than any musical reset interval.
    /// [design parameter] default 0.5 ms, range 0.1 .. 5 ms.
    static constexpr double kRefractoryMs = 0.5;

    /// One sample of transport, decoded.
    struct Frame {
        bool run_high = false;
        bool reset_edge = false;
        bool clock_edge = false;
    };

    void prepare(double sample_rate) {
        if (std::isfinite(sample_rate) && sample_rate > 0.0) sample_rate_ = sample_rate;
        update();
    }

    /// Upper bound on the refractory window. Two orders of magnitude past the
    /// documented range, purely so the sample-count conversion below cannot
    /// overflow on a nonsense value from a host.
    /// [design parameter] default 1000 ms ceiling, range 10 .. 60000 ms.
    static constexpr double kMaxRefractoryMs = 1000.0;

    void set_refractory_ms(double ms) {
        refractory_ms_ = seq_detail::clamp_finite(ms, 0.0, kMaxRefractoryMs);
        update();
    }

    /// Sets the hysteresis window used for all three inputs, so a patch running
    /// at ±5 V and one running at 0/1 configure in one call.
    void set_thresholds(double high, double low) {
        run_.set_thresholds(high, low);
        reset_.set_thresholds(high, low);
        clock_.set_thresholds(high, low);
    }

    void reset() {
        run_.reset();
        reset_.reset();
        clock_.reset();
        refractory_ = 0;
    }

    static constexpr int latency_samples() { return 0; }

    /// Decodes run, reset and clock levels for one sample.
    Frame process(SampleType run, SampleType reset_in, SampleType clock) {
        Frame f{};
        run_.process(run);
        f.run_high = run_.high();

        const bool raw_reset = reset_.process(reset_in);
        if (refractory_ > 0) --refractory_;
        if (raw_reset && refractory_ == 0) {
            f.reset_edge = true;
            refractory_ = refractory_samples_;
        }

        f.clock_edge = clock_.process(clock);
        return f;
    }

    /// Run and reset only, for a caller whose clock edge already arrives as a
    /// boolean (a `ClockDividerT` output, say).
    Frame process(SampleType run, SampleType reset_in) {
        return process(run, reset_in, SampleType{0});
    }

private:
    void update() {
        refractory_samples_ = static_cast<std::int64_t>(
            std::llround(units::ms_to_samples(refractory_ms_, sample_rate_)));
        if (refractory_samples_ < 0) refractory_samples_ = 0;
    }

    double sample_rate_ = 44100.0;
    double refractory_ms_ = kRefractoryMs;
    std::int64_t refractory_samples_ = 0;
    std::int64_t refractory_ = 0;
    HystereticTriggerDetectT<SampleType> run_{};
    HystereticTriggerDetectT<SampleType> reset_{};
    HystereticTriggerDetectT<SampleType> clock_{};
};

using TransportEdge = TransportEdgeT<float>;
using TransportEdge64 = TransportEdgeT<double>;

}  // namespace pulp::signal
