#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/realtime_pitch_time_processor.hpp>

#include <algorithm>
#include <array>
#include <cmath>

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
