#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/sample_starvation_envelope.hpp>

#include "harness/rt_allocation_probe.hpp"

using Catch::Matchers::WithinAbs;
using pulp::audio::SampleStarvationEnvelope;
using pulp::audio::SampleStarvationMode;

TEST_CASE("Sample starvation envelope fades valid gains to silence and back",
          "[audio][sampler][starvation]") {
    SampleStarvationEnvelope envelope;
    REQUIRE(envelope.prepare({.fade_out_frames = 5, .recovery_frames = 5}));

    envelope.begin_predicted_fade(5);
    REQUIRE(envelope.mode() == SampleStarvationMode::FadingOut);
    float fade_out[5]{};
    for (auto& gain : fade_out) gain = envelope.next_valid_gain();
    REQUIRE_THAT(fade_out[0], WithinAbs(1.0f, 1.0e-7f));
    REQUIRE_THAT(fade_out[2], WithinAbs(0.70710678f, 1.0e-6f));
    REQUIRE_THAT(fade_out[4], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE(envelope.mode() == SampleStarvationMode::Silent);

    envelope.mark_starved(19);
    REQUIRE_THAT(envelope.next_valid_gain(), WithinAbs(0.0f, 1.0e-6f));
    envelope.begin_recovery();
    REQUIRE(envelope.mode() == SampleStarvationMode::Recovering);
    float recovery[5]{};
    for (auto& gain : recovery) gain = envelope.next_valid_gain();
    REQUIRE_THAT(recovery[0], WithinAbs(0.0f, 1.0e-7f));
    REQUIRE_THAT(recovery[2], WithinAbs(0.70710678f, 1.0e-6f));
    REQUIRE_THAT(recovery[4], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE(envelope.mode() == SampleStarvationMode::Ready);
    REQUIRE(envelope.next_valid_gain() == 1.0f);

    const auto stats = envelope.stats();
    REQUIRE(stats.predicted_events == 1);
    REQUIRE(stats.emergency_events == 0);
    REQUIRE(stats.starved_frames == 19);
    REQUIRE(stats.recovery_events == 1);
}

TEST_CASE("Sample starvation envelope counts unexpected misses and interrupted recovery",
          "[audio][sampler][starvation][telemetry]") {
    SampleStarvationEnvelope envelope;
    REQUIRE(envelope.prepare({.fade_out_frames = 8, .recovery_frames = 8}));

    envelope.mark_starved(3);
    REQUIRE(envelope.stats().emergency_events == 1);
    envelope.begin_recovery();
    REQUIRE(envelope.next_valid_gain() == 0.0f);
    envelope.mark_starved(2);
    REQUIRE(envelope.mode() == SampleStarvationMode::Silent);
    REQUIRE(envelope.stats().emergency_events == 2);
    REQUIRE(envelope.stats().starved_frames == 5);

    envelope.begin_predicted_fade(4);
    REQUIRE(envelope.mode() == SampleStarvationMode::Silent);
}

TEST_CASE("Sample starvation envelope cancels an averted prediction without muting",
          "[audio][sampler][starvation][recovery]") {
    SampleStarvationEnvelope envelope;
    REQUIRE_FALSE(envelope.prepare({.fade_out_frames = 1, .recovery_frames = 8}));
    REQUIRE(envelope.prepare({.fade_out_frames = 8, .recovery_frames = 8}));

    envelope.begin_predicted_fade(8);
    const float first = envelope.next_valid_gain();
    const float second = envelope.next_valid_gain();
    REQUIRE(first == 1.0f);
    REQUIRE(second < first);
    REQUIRE(second > 0.0f);
    envelope.cancel_predicted_fade();
    REQUIRE(envelope.mode() == SampleStarvationMode::Recovering);
    REQUIRE_THAT(envelope.next_valid_gain(), WithinAbs(second, 1.0e-6f));
    float recovered = 0.0f;
    for (int frame = 1; frame < 8; ++frame)
        recovered = envelope.next_valid_gain();
    REQUIRE_THAT(recovered, WithinAbs(1.0f, 1.0e-6f));
    REQUIRE(envelope.mode() == SampleStarvationMode::Ready);
    REQUIRE(envelope.stats().emergency_events == 0);
    REQUIRE(envelope.stats().recovery_events == 1);

    envelope.begin_predicted_fade(1);
    REQUIRE(envelope.mode() == SampleStarvationMode::FadingOut);
    REQUIRE(envelope.stats().insufficient_lead_events == 1);
    REQUIRE_THAT(envelope.next_valid_gain(), WithinAbs(0.0f, 1.0e-6f));
    REQUIRE(envelope.mode() == SampleStarvationMode::Silent);
    envelope.mark_starved(1);
    REQUIRE(envelope.stats().emergency_events == 1);
}

TEST_CASE("Sample starvation envelope reverses an interrupted recovery without a gain jump",
          "[audio][sampler][starvation][recovery]") {
    SampleStarvationEnvelope envelope;
    REQUIRE(envelope.prepare({.fade_out_frames = 4, .recovery_frames = 5}));

    envelope.mark_starved(4);
    envelope.begin_recovery();
    REQUIRE(envelope.next_valid_gain() == 0.0f);
    const auto recovered_gain = envelope.next_valid_gain();
    REQUIRE(recovered_gain > 0.0f);

    envelope.begin_predicted_fade(3);
    REQUIRE(envelope.mode() == SampleStarvationMode::FadingOut);
    REQUIRE_THAT(envelope.next_valid_gain(), WithinAbs(recovered_gain, 1.0e-6f));
    REQUIRE(envelope.next_valid_gain() < recovered_gain);
    REQUIRE_THAT(envelope.next_valid_gain(), WithinAbs(0.0f, 1.0e-6f));
    REQUIRE(envelope.mode() == SampleStarvationMode::Silent);
    REQUIRE(envelope.stats().predicted_events == 1);
    REQUIRE(envelope.stats().emergency_events == 1);
}

TEST_CASE("Prepared sample starvation envelope hot operations do not allocate",
          "[audio][sampler][starvation][rt]") {
    SampleStarvationEnvelope envelope;
    REQUIRE(envelope.prepare({.fade_out_frames = 16, .recovery_frames = 16}));

    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int iteration = 0; iteration < 10000; ++iteration) {
            envelope.begin_predicted_fade(16);
            for (int frame = 0; frame < 16; ++frame)
                (void) envelope.next_valid_gain();
            envelope.mark_starved(1);
            envelope.begin_recovery();
            for (int frame = 0; frame < 16; ++frame)
                (void) envelope.next_valid_gain();
        }
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
}
