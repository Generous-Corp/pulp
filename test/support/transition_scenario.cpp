// transition_scenario.cpp — per-callback cost gates at state transitions.

#include "transition_scenario.hpp"

#include <pulp/format/headless.hpp>

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace pulp::test::audio {

namespace {

pulp::audio::Buffer<float> default_stimulus(double, int channels,
                                            std::int64_t frames) {
    // Loud seeded broadband: every bin of an analysis carries energy, so a
    // processor whose work scales with content does its full steady work.
    return make_white_noise(channels, static_cast<int>(frames), 0x7A11u, 0.5f);
}

std::string format_ops(pulp::signal::rt::RtWorkCounts ops) {
    std::ostringstream out;
    out << "fft=" << ops.fft << " trig=" << ops.trig << " bins=" << ops.bins;
    return out.str();
}

std::string describe(const TransitionOutcome& outcome) {
    std::ostringstream out;
    out << "'" << outcome.transition.name << "' (param " << outcome.transition.id
        << ' ' << outcome.transition.from << "->" << outcome.transition.to
        << ", block=" << outcome.block_size << ")";
    return out.str();
}

} // namespace

TransitionScenario::TransitionScenario(pulp::format::ProcessorFactory factory)
    : factory_(factory), input_(default_stimulus) {}

TransitionScenario TransitionScenario::standard(
    pulp::format::ProcessorFactory factory) {
    TransitionScenario scenario(factory);
    for (auto& transition : standard_transition_cases(factory))
        scenario.add(std::move(transition));
    return scenario;
}

TransitionScenario& TransitionScenario::add(TransitionCase transition) {
    cases_.push_back(std::move(transition));
    return *this;
}

TransitionScenario& TransitionScenario::sample_rate(double hz) {
    sample_rate_ = hz;
    return *this;
}

TransitionScenario& TransitionScenario::block_sizes(std::vector<int> sizes) {
    block_sizes_ = std::move(sizes);
    return *this;
}

TransitionScenario& TransitionScenario::channels(int inputs, int outputs) {
    input_channels_ = inputs;
    output_channels_ = outputs;
    return *this;
}

TransitionScenario& TransitionScenario::warmup_blocks(int blocks) {
    warmup_blocks_ = blocks;
    return *this;
}

TransitionScenario& TransitionScenario::settle_blocks(int blocks) {
    settle_blocks_ = blocks;
    return *this;
}

TransitionScenario& TransitionScenario::input(
    RenderScenario::InputGenerator generator) {
    input_ = std::move(generator);
    return *this;
}

std::vector<TransitionCase> standard_transition_cases(
    pulp::format::ProcessorFactory factory) {
    if (factory == nullptr)
        throw std::invalid_argument("standard_transition_cases: no factory");
    pulp::format::HeadlessHost host(factory);
    std::vector<TransitionCase> cases;
    for (const auto& info : host.state().all_params()) {
        const float lo = info.range.min;
        const float hi = info.range.max;
        if (!(hi > lo))
            continue;
        if (info.is_trigger) {
            const float rest = info.range.default_value;
            const float fire = rest == lo ? hi : lo;
            cases.push_back({info.name + " fire", info.id, rest, fire});
            continue;
        }
        cases.push_back({info.name + " min->max", info.id, lo, hi});
        cases.push_back({info.name + " max->min", info.id, hi, lo});
    }
    return cases;
}

TransitionOutcome TransitionScenario::render_case(
    const TransitionCase& transition, int block_size, int repeats) const {
    if (warmup_blocks_ < 2 || settle_blocks_ < 1)
        throw std::invalid_argument(
            "TransitionScenario: warmup_blocks must be >= 2 and "
            "settle_blocks >= 1");
    const auto edge_block = static_cast<std::size_t>(warmup_blocks_);
    const std::int64_t edge_frame =
        static_cast<std::int64_t>(warmup_blocks_) * block_size;
    const std::int64_t total =
        static_cast<std::int64_t>(warmup_blocks_ + settle_blocks_) * block_size;

    auto scenario = RenderScenario(factory_)
                        .name("transition." + transition.name)
                        .sample_rate(sample_rate_)
                        .block_size(block_size)
                        .channels(input_channels_, output_channels_)
                        .duration_frames(total)
                        .set_param(transition.id, transition.from)
                        .automate(ParamStep{transition.id, edge_frame,
                                            transition.to});
    if (input_channels_ > 0)
        scenario.input(input_);

    TransitionOutcome outcome;
    outcome.transition = transition;
    outcome.block_size = block_size;
    outcome.transition_block = edge_block;
    double best_ratio = std::numeric_limits<double>::infinity();

    for (int repeat = 0; repeat < std::max(repeats, 1); ++repeat) {
        const auto result = scenario.render();
        const std::span<const BlockCost> costs(result.block_costs);
        if (costs.size() <= edge_block)
            throw std::logic_error("TransitionScenario: render shorter than "
                                   "its warmup");
        const auto steady = costs.subspan(1, edge_block - 1);
        const auto after = costs.subspan(edge_block);

        if (repeat == 0) {
            outcome.steady_ops_max = max_ops(steady);
            outcome.transition_ops_max = max_ops(after);
            std::uint64_t worst_fft = 0;
            outcome.transition_ops_worst_block = edge_block;
            for (std::size_t i = 0; i < after.size(); ++i) {
                if (after[i].ops.fft > worst_fft) {
                    worst_fft = after[i].ops.fft;
                    outcome.transition_ops_worst_block = edge_block + i;
                }
            }
        }
        if (repeats <= 0)
            break;

        const auto steady_cpu = summarize_cpu(steady).max;
        const auto after_cpu = summarize_cpu(after).max;
        const double ratio =
            steady_cpu > 0 ? static_cast<double>(after_cpu) /
                                 static_cast<double>(steady_cpu)
                           : std::numeric_limits<double>::infinity();
        if (ratio < best_ratio) {
            best_ratio = ratio;
            outcome.steady_cpu_max = steady_cpu;
            outcome.transition_cpu_max = after_cpu;
        }
    }
    outcome.cpu_ratio = repeats > 0 ? best_ratio : 0.0;
    return outcome;
}

std::vector<TransitionOutcome> TransitionScenario::run() const {
    std::vector<TransitionOutcome> outcomes;
    for (int block : block_sizes_)
        for (const auto& transition : cases_)
            outcomes.push_back(render_case(transition, block, 0));
    return outcomes;
}

std::vector<TransitionOutcome> TransitionScenario::run_timed(int repeats) const {
    if (repeats < 1)
        throw std::invalid_argument("TransitionScenario: repeats must be >= 1");
    std::vector<TransitionOutcome> outcomes;
    for (int block : block_sizes_)
        for (const auto& transition : cases_)
            outcomes.push_back(render_case(transition, block, repeats));
    return outcomes;
}

CheckResult assert_transition_ops_bounded(
    const std::vector<TransitionOutcome>& outcomes,
    TransitionAllowance allowance) {
    if (!pulp::signal::rt::kWorkCountersEnabled)
        return {false,
                "operation counters are compiled out (configure with "
                "-DPULP_RT_WORK_COUNTERS=ON); refusing to report a transition "
                "gate that cannot see any work"};
    if (outcomes.empty())
        return {false, "no transition outcomes to check"};

    std::ostringstream failures;
    std::size_t failed = 0;
    for (const auto& outcome : outcomes) {
        const auto& steady = outcome.steady_ops_max;
        const auto& worst = outcome.transition_ops_max;
        if (worst.fft > steady.fft + allowance.fft ||
            worst.trig > steady.trig + allowance.trig ||
            worst.bins > steady.bins + allowance.bins) {
            ++failed;
            failures << "\n  " << describe(outcome) << ": worst block "
                     << outcome.transition_ops_worst_block << " did "
                     << format_ops(worst) << " against steady max "
                     << format_ops(steady) << " + allowance "
                     << format_ops({allowance.fft, allowance.trig, allowance.bins});
        }
    }
    std::ostringstream msg;
    if (failed > 0) {
        msg << failed << " of " << outcomes.size()
            << " transitions burst work into one callback:" << failures.str();
        return {false, msg.str()};
    }
    msg << outcomes.size() << " transitions stay within the steady per-block "
        << "operation count + allowance";
    return {true, msg.str()};
}

CheckResult assert_transition_cpu_ratio(
    const std::vector<TransitionOutcome>& outcomes, double max_ratio) {
    if (outcomes.empty())
        return {false, "no transition outcomes to check"};
    std::ostringstream failures;
    std::size_t failed = 0;
    for (const auto& outcome : outcomes) {
        if (!(outcome.cpu_ratio > 0.0) || outcome.cpu_ratio > max_ratio) {
            ++failed;
            failures << "\n  " << describe(outcome) << ": min-over-repeats "
                     << "worst/steady CPU ratio " << outcome.cpu_ratio << " ("
                     << outcome.transition_cpu_max << " vs "
                     << outcome.steady_cpu_max << ")";
        }
    }
    std::ostringstream msg;
    if (failed > 0) {
        msg << failed << " of " << outcomes.size()
            << " transitions exceed CPU ratio " << max_ratio << ':'
            << failures.str();
        return {false, msg.str()};
    }
    msg << outcomes.size() << " transitions within CPU ratio " << max_ratio;
    return {true, msg.str()};
}

} // namespace pulp::test::audio
