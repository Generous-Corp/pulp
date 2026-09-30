#pragma once

/// @file transition_scenario.hpp
/// Per-callback cost gates at state transitions.
///
/// Invariant: **the cost of a callback that contains a state transition is
/// bounded by the same budget as a steady-state callback.** Transition work
/// (a latch, an engage, a mode switch) is amortised across the following
/// blocks or precomputed in prepare(); it is never burst into one callback.
///
/// An offline render cannot see a violation — it has no deadline — which is
/// how a freeze engage that ran ~23 IFFTs in one callback shipped with every
/// offline click test green and then dropped a buffer in every DAW. A
/// TransitionScenario renders each transition through HeadlessHost with the
/// per-block cost probe on and compares the blocks from the edge onward
/// against the steady blocks before it:
///
/// - `assert_transition_ops_bounded` — deterministic operation counts
///   (FFT executions, counted transcendental calls). Required-lane eligible;
///   needs a build with `PULP_RT_WORK_COUNTERS` (tests builds have it).
/// - `assert_transition_cpu_ratio` — thread CPU time, minimum over repeats.
///   Advisory: register it under the `performance` label.
///
/// Both compare against the steady **maximum**, not the median: an STFT
/// processor whose hop exceeds the block size is itself bursty in steady
/// state (one block in eight runs the FFT), and that is not a defect. Choose
/// `warmup_blocks` long enough to contain at least one full hop.
///
/// @code
/// auto outcomes = TransitionScenario::standard(my_factory)
///                     .block_sizes({128, 32})
///                     .run();
/// REQUIRE(assert_transition_ops_bounded(outcomes, {.fft = 0}).passed);
/// @endcode
///
/// Test/tool layer only.

#include "render_scenario.hpp"

#include <pulp/format/processor.hpp>
#include <pulp/signal/rt_work_counter.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::test::audio {

/// One parameter edge: the render starts with `id` at `from` and steps it to
/// `to` at the first frame of the transition block.
struct TransitionCase {
    std::string name;
    pulp::state::ParamID id = 0;
    float from = 0.0f;
    float to = 0.0f;
};

/// Extra counted work a transition block may do over the steady maximum.
/// A processor that legitimately does bounded extra work on an edge (one
/// more IFFT per channel, say) declares it here rather than loosening the
/// gate for everyone.
struct TransitionAllowance {
    std::uint64_t fft = 0;
    std::uint64_t trig = 0;
    std::uint64_t bins = 0;
};

/// Result of rendering one case at one block size.
struct TransitionOutcome {
    TransitionCase transition;
    int block_size = 0;
    /// Index of the block that received the parameter step.
    std::size_t transition_block = 0;
    /// Per-counter maximum over the steady blocks before the edge
    /// (block 0 excluded: it carries first-touch cold-start work).
    pulp::signal::rt::RtWorkCounts steady_ops_max;
    /// Per-counter maximum over the blocks from the edge to the end.
    pulp::signal::rt::RtWorkCounts transition_ops_max;
    /// Block index (render order) holding the largest FFT count after the edge.
    std::size_t transition_ops_worst_block = 0;
    /// Minimum over repeats of (worst CPU after the edge) / (worst steady
    /// CPU). 0 when timing was not measured.
    double cpu_ratio = 0.0;
    /// Worst steady / after-edge CPU of the repeat that produced cpu_ratio.
    std::int64_t steady_cpu_max = 0;
    std::int64_t transition_cpu_max = 0;
};

class TransitionScenario {
public:
    explicit TransitionScenario(pulp::format::ProcessorFactory factory);

    /// The standard catalog for a processor, enumerated from its own
    /// define_parameters(): every discrete (toggle / enum / integer) and
    /// continuous parameter stepped min→max and max→min while loud
    /// broadband material plays. Trigger parameters are stepped from their
    /// rest value to the other end of their range. Add plugin-specific cases
    /// with add().
    static TransitionScenario standard(pulp::format::ProcessorFactory factory);

    TransitionScenario& add(TransitionCase transition);
    TransitionScenario& sample_rate(double hz);
    TransitionScenario& block_sizes(std::vector<int> sizes);
    TransitionScenario& channels(int inputs, int outputs);
    /// Steady blocks rendered before the edge (block 0 included). Must be at
    /// least 2 so there is one steady block after the cold-start block.
    TransitionScenario& warmup_blocks(int blocks);
    /// Blocks rendered from the edge onward. Staged work must complete within
    /// this window for the gate to see all of it.
    TransitionScenario& settle_blocks(int blocks);
    /// Stimulus generator; defaults to a loud seeded broadband signal.
    TransitionScenario& input(RenderScenario::InputGenerator generator);

    const std::vector<TransitionCase>& cases() const { return cases_; }

    /// Render every case at every block size once and collect operation
    /// counts. Deterministic.
    std::vector<TransitionOutcome> run() const;

    /// Render every case at every block size `repeats` times and also collect
    /// the CPU ratio (minimum over repeats: an algorithmic spike is present in
    /// every repeat, scheduler noise is not).
    std::vector<TransitionOutcome> run_timed(int repeats = 5) const;

private:
    TransitionOutcome render_case(const TransitionCase& transition,
                                  int block_size, int repeats) const;

    pulp::format::ProcessorFactory factory_ = nullptr;
    std::vector<TransitionCase> cases_;
    double sample_rate_ = 48000.0;
    std::vector<int> block_sizes_{128};
    int input_channels_ = 2;
    int output_channels_ = 2;
    int warmup_blocks_ = 24;
    int settle_blocks_ = 24;
    RenderScenario::InputGenerator input_;
};

/// Enumerate the standard transition cases from a processor's parameters.
std::vector<TransitionCase> standard_transition_cases(
    pulp::format::ProcessorFactory factory);

/// Deterministic gate: for every outcome, every counter's worst block from
/// the edge onward is at most the steady maximum plus the allowance. Fails
/// (rather than passing vacuously) when the build does not count operations.
CheckResult assert_transition_ops_bounded(
    const std::vector<TransitionOutcome>& outcomes,
    TransitionAllowance allowance = {});

/// Advisory timing gate: every outcome's cpu_ratio is at most `max_ratio`.
/// Requires outcomes from run_timed().
CheckResult assert_transition_cpu_ratio(
    const std::vector<TransitionOutcome>& outcomes, double max_ratio = 6.0);

} // namespace pulp::test::audio
