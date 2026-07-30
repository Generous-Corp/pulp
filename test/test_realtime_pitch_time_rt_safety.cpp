#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/realtime_pitch_time_processor.hpp>
#include <pulp/signal/finite_stretch_builder.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace pulp::signal;

TEST_CASE("Prepared finite pitch-time stream is allocation-free through EOF",
          "[signal][pitch-time][streaming][rt-safety]") {
    RealtimePitchTimeConfig config;
    config.mode = PitchTimeMode::time_stretch;
    config.quality = PitchTimeQuality::low_latency;
    config.channels = 2;
    config.max_block = 256;
    config.max_time_ratio = 1.5f;

    RealtimePitchTimeProcessor stretch;
    REQUIRE(stretch.prepare(48000.0, config) == PitchTimePrepareStatus::prepared);
    stretch.set_time_ratio(1.25f);

    std::array<float, 256> in_l {};
    std::array<float, 256> in_r {};
    std::array<float, 256> out_l {};
    std::array<float, 256> out_r {};
    for (std::size_t i = 0; i < in_l.size(); ++i) {
        in_l[i] = std::sin(static_cast<float>(i) * 0.025f);
        in_r[i] = std::cos(static_cast<float>(i) * 0.031f);
    }
    const float* inputs[] = {in_l.data(), in_r.data()};
    float* outputs[] = {out_l.data(), out_r.data()};

    PitchTimeStreamFeedStatus feed_status = PitchTimeStreamFeedStatus::invalid_request;
    PitchTimeStreamFinalizeStatus finalize_status = PitchTimeStreamFinalizeStatus::invalid_mode;
    int finalize_calls = 0;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int block = 0; block < 6; ++block)
            feed_status = stretch.feed(inputs, static_cast<int>(in_l.size()));
        while (finalize_calls < 64) {
            const int available = stretch.available_stretched();
            stretch.read_stretched(outputs,
                                   std::min(available, static_cast<int>(out_l.size())));
            finalize_status = stretch.finalize();
            ++finalize_calls;
            if (finalize_status == PitchTimeStreamFinalizeStatus::complete) break;
        }
        (void)stretch.output_free_space();
        (void)stretch.input_priming_samples();
        (void)stretch.output_alignment_samples();
        allocations = probe.allocation_count();
    }

    REQUIRE(allocations == 0);
    REQUIRE(feed_status == PitchTimeStreamFeedStatus::accepted);
    REQUIRE(finalize_status == PitchTimeStreamFinalizeStatus::complete);
    REQUIRE(finalize_calls < 64);
}

TEST_CASE("Prepared finite stretch builder steps allocate nothing",
          "[signal][pitch-time][finite-stretch][rt-safety]") {
    constexpr std::size_t input_frames = 8192;
    constexpr std::size_t output_frames = 10240;
    std::array<float, input_frames> input_left {};
    std::array<float, input_frames> input_right {};
    std::array<float, output_frames> output_left {};
    std::array<float, output_frames> output_right {};
    for (std::size_t i = 0; i < input_left.size(); ++i) {
        input_left[i] = std::sin(static_cast<float>(i) * 0.025f);
        input_right[i] = std::cos(static_cast<float>(i) * 0.031f);
    }
    const float* inputs[] = {input_left.data(), input_right.data()};
    float* outputs[] = {output_left.data(), output_right.data()};

    FiniteStretchConfig config;
    config.processor.mode = PitchTimeMode::time_stretch;
    config.processor.quality = PitchTimeQuality::low_latency;
    config.processor.channels = 2;
    config.processor.max_block = 73;
    config.processor.max_time_ratio = 1.5f;
    config.input = inputs;
    config.input_frames = input_left.size();
    config.output = outputs;
    config.output_capacity_frames = output_left.size();
    config.target_frames = output_left.size();
    config.constant_time_ratio = 1.25f;

    FiniteStretchBuilder builder;
    REQUIRE(builder.prepare(config) == FiniteStretchPrepareStatus::prepared);

    FiniteStretchConfig empty_config;
    empty_config.processor = config.processor;
    FiniteStretchBuilder empty;
    REQUIRE(empty.prepare(empty_config) == FiniteStretchPrepareStatus::prepared);

    auto invalid_config = config;
    invalid_config.ratio_at_input_frame =
        +[](void*, std::uint64_t) noexcept { return std::numeric_limits<float>::quiet_NaN(); };
    FiniteStretchBuilder invalid;
    REQUIRE(invalid.prepare(invalid_config) == FiniteStretchPrepareStatus::prepared);
    FiniteStretchStepStatus status = FiniteStretchStepStatus::progress;
    FiniteStretchStepStatus empty_status = FiniteStretchStepStatus::progress;
    FiniteStretchStepStatus invalid_status = FiniteStretchStepStatus::progress;
    bool saw_backpressure = false;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int guard = 0; guard < 10000 && status == FiniteStretchStepStatus::progress;
            ++guard)
        {
            status = builder.step();
            saw_backpressure = saw_backpressure
                            || builder.last_work_outcome()
                                   == FiniteStretchWorkOutcome::backpressure;
        }
        for (int guard = 0;
             guard < 1000 && empty_status == FiniteStretchStepStatus::progress; ++guard)
            empty_status = empty.step();
        for (int guard = 0;
             guard < 1000 && invalid_status == FiniteStretchStepStatus::progress; ++guard)
            invalid_status = invalid.step();
        allocations = probe.allocation_count();
    }

    REQUIRE(allocations == 0);
    REQUIRE(status == FiniteStretchStepStatus::complete);
    REQUIRE(empty_status == FiniteStretchStepStatus::complete);
    REQUIRE(invalid_status == FiniteStretchStepStatus::failed);
    REQUIRE(invalid.failure() == FiniteStretchFailure::invalid_ratio);
    REQUIRE(saw_backpressure);
    REQUIRE(builder.failure() == FiniteStretchFailure::none);
    REQUIRE(builder.input_frames_consumed() == input_left.size());
    REQUIRE(builder.output_frames_written() == output_left.size());
}
