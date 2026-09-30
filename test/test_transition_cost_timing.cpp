// test_transition_cost_timing.cpp — advisory CPU-time twin of the transition
// cost gate.
//
// Thread CPU time excludes preemption but still moves with cache state and
// frequency scaling, so these cases are tagged [performance]: their ctest
// registration carries the `performance` label and stays off the required
// gate. Each case takes the minimum over repeats of
// (worst block from the edge onward) / (worst steady block): an algorithmic
// spike is present in every repeat, scheduler noise is not. The negative
// control is ~23 IFFTs plus ~94k polar calls on the edge — several
// milliseconds of real work on every platform — against one FFT per steady
// block.

#include <catch2/catch_test_macros.hpp>

#include "support/bursty_processor.hpp"
#include "support/transition_scenario.hpp"

using namespace pulp::test::audio;

TEST_CASE("transition CPU gate passes a processor with no transition burst",
          "[transition-cost][timing][performance]") {
    const auto outcomes = TransitionScenario::standard(make_bursty_processor<0>)
                              .block_sizes({128})
                              .warmup_blocks(12)
                              .settle_blocks(6)
                              .run_timed(5);
    const auto check = assert_transition_cpu_ratio(outcomes, 6.0);
    INFO(check.message);
    CHECK(check.passed);
}

TEST_CASE("transition CPU gate fails a processor that bursts on an edge",
          "[transition-cost][timing][negative-control][performance]") {
    auto scenario = TransitionScenario(make_bursty_processor<23>)
                        .add({"Engage on", kBurstyEngage, 0.0f, 1.0f})
                        .add({"Engage off", kBurstyEngage, 1.0f, 0.0f})
                        .block_sizes({128})
                        .warmup_blocks(12)
                        .settle_blocks(6);
    const auto outcomes = scenario.run_timed(5);
    const auto check = assert_transition_cpu_ratio(outcomes, 6.0);
    INFO(check.message);
    CHECK_FALSE(check.passed);
    for (const auto& outcome : outcomes)
        CHECK(outcome.cpu_ratio > 6.0);
}
