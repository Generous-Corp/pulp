#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/tpt_filter.hpp>
#include <pulp/state/parameter.hpp>

#include <cmath>
#include <limits>

namespace {

using Catch::Matchers::WithinAbs;

constexpr float kPi = 3.14159265358979323846f;

} // namespace

TEST_CASE("DSPX-09 independent allpass oracle preserves sinusoid magnitude",
          "[dspx-09][positive]") {
    pulp::signal::TptFilter filter;
    filter.prepare(48000.0f);
    filter.set_cutoff(1200.0f);

    constexpr int warmup = 512;
    constexpr int frames = 4096;
    constexpr float frequency = 300.0f;
    constexpr float sample_rate = 48000.0f;
    double input_energy = 0.0;
    double output_energy = 0.0;
    bool all_outputs_finite = true;
    for (int i = 0; i < warmup + frames; ++i) {
        const float input = std::sin(2.0f * kPi * frequency * static_cast<float>(i) / sample_rate);
        const float output = filter.process_allpass(input);
        all_outputs_finite = all_outputs_finite && std::isfinite(output);
        if (i >= warmup) {
            input_energy += static_cast<double>(input) * input;
            output_energy += static_cast<double>(output) * output;
        }
    }

    CHECK(all_outputs_finite);
    REQUIRE(input_energy > 0.0);
    const double magnitude_squared = output_energy / input_energy;
    // Independent frequency-domain oracle: an allpass has unity magnitude.
    CHECK_THAT(magnitude_squared, WithinAbs(1.0, 0.02));
}

TEST_CASE("DSPX-09 automation base and offset oracle rejects malformed values",
          "[dspx-09][positive][negative]") {
    using pulp::state::BaseOffsetRefusal;
    using pulp::state::BaseOffsetValue;

    const BaseOffsetValue authored{0.5f, 0.2f};
    CHECK_THAT(authored.effective(), WithinAbs(0.7f, 0.0001f));
    CHECK_THAT(authored.without_offset().effective(), WithinAbs(0.5f, 0.0001f));
    CHECK(pulp::state::validate_base_offset(authored, 0.0f, 1.0f) == BaseOffsetRefusal::None);

    const BaseOffsetValue nan_base{std::numeric_limits<float>::quiet_NaN(), 0.0f};
    const BaseOffsetValue nan_offset{0.5f, std::numeric_limits<float>::quiet_NaN()};
    const BaseOffsetValue out_of_range{0.9f, 0.2f};
    CHECK(pulp::state::validate_base_offset(nan_base, 0.0f, 1.0f) ==
          BaseOffsetRefusal::NonFiniteBase);
    CHECK(pulp::state::validate_base_offset(nan_offset, 0.0f, 1.0f) ==
          BaseOffsetRefusal::NonFiniteOffset);
    CHECK(pulp::state::validate_base_offset(out_of_range, 0.0f, 1.0f) ==
          BaseOffsetRefusal::OutOfRange);
}
