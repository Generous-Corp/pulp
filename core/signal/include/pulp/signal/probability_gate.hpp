#pragma once

#include <pulp/signal/detail/modular_sequencing_common.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/trigger_kit.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

/// Passes each incoming trigger with a stated probability, deterministically.
///
/// The draw is a seeded `Xorshift32` (series law 2), so a probabilistic groove
/// can be auditioned, bounced and reloaded and give *the same* performance every
/// time. Changing the seed is how you roll again; the seed is a config value and
/// never an automation lane.
///
/// **A draw is consumed on every trigger regardless of `p`.** `p` gates the
/// *result*, never *whether we draw*. If a `p` of 1 skipped the draw, automating
/// `p` during a take would shift the stream position and every later decision
/// with it — the same pattern would render differently depending on the
/// automation that preceded it, which is exactly the reproducibility law 2
/// exists to protect.
///
/// A transport reset edge clears the edge-detector latch but **does not** rewind
/// the stream: a live reset jack that rewound randomness would make every reset
/// sound identical.
///
/// **USE.** *Generative drums* — on a hi-hat clock for humanized density;
/// automate `p` for a build-up that is still bit-reproducible. *Trigger
/// thinning* — drop hits from a `BurstGenT` (kit) ratchet for un-mechanical
/// rolls. *Sometimes-accent* — `GateLogicT(and)` of a probability gate with a
/// downbeat clock.
///
/// RT contract: as the header. One generator call per trigger.
template <typename SampleType = float>
class ProbGateT {
public:
    /// Pass probability.
    /// [design parameter] default 0.5, range 0 .. 1.
    static constexpr double kDefaultProbability = 0.5;

    /// [design parameter] default 0x1234567, range 0 .. 2^31 − 1.
    static constexpr std::uint32_t kProbSeed = 0x1234567u;

    ProbGateT() { rng_.set_seed(kProbSeed); }

    void prepare(double) {}

    void set_probability(double p) {
        if (std::isfinite(p)) probability_ = std::clamp(p, 0.0, 1.0);
    }
    double probability() const { return probability_; }

    void set_seed(std::uint32_t seed) { rng_.set_seed(seed); }

    /// Verb 2: clears the edge-detector latch only. The RNG stream is untouched.
    void apply_reset_edge() { detector_.reset(); }

    /// Verb 1: clears the latch and rewinds the stream to the seed.
    void reset() {
        detector_.reset();
        rng_.reset();
        draws_ = 0;
    }

    static constexpr int latency_samples() { return 0; }

    /// Number of draws consumed since the last `reset()` — the stream position,
    /// exposed so a test can assert it advances once per trigger whatever `p` is.
    std::uint32_t draw_count() const { return draws_; }

    /// Decides one already-detected trigger edge.
    bool process_edge(bool trigger_edge) {
        if (!trigger_edge) return false;
        ++draws_;
        return rng_.next_unit<double>() < probability_;
    }

    /// Detects the edge from a trigger signal and decides it, so this block can
    /// sit directly on a clock output without a separate detector.
    bool process(SampleType trigger) { return process_edge(detector_.process(trigger)); }

private:
    double probability_ = kDefaultProbability;
    std::uint32_t draws_ = 0;
    Xorshift32 rng_{kProbSeed};
    HystereticTriggerDetectT<SampleType> detector_{};
};

using ProbGate = ProbGateT<float>;
using ProbGate64 = ProbGateT<double>;

}  // namespace pulp::signal
