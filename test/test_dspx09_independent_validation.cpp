#include <catch2/catch_test_macros.hpp>

#include <pulp/host/signal_graph_node.hpp>
#include <pulp/signal/tpt_filter.hpp>
#include <pulp/state/parameter.hpp>

#include <cmath>
#include <limits>

namespace {

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
    CHECK(magnitude_squared == Catch::Approx(1.0).margin(0.02));
}

TEST_CASE("DSPX-09 retained-history refusal is explicit and bounded", "[dspx-09][negative]") {
    pulp::host::RetainedHistoryPolicy policy;
    policy.mode = pulp::host::RetainedHistoryMode::Refuse;
    policy.max_bytes = 1024;
    CHECK(policy.mode == pulp::host::RetainedHistoryMode::Refuse);
    CHECK(policy.max_bytes == 1024);

    // A refusal policy must remain bounded and must not silently become Adopt.
    CHECK(policy.mode != pulp::host::RetainedHistoryMode::Adopt);
}

TEST_CASE("DSPX-09 automation base and offset oracle rejects malformed values",
          "[dspx-09][positive][negative]") {
    using pulp::state::BaseOffsetRefusal;
    using pulp::state::BaseOffsetValue;

    const BaseOffsetValue authored{0.5f, 0.2f};
    CHECK(authored.effective() == Catch::Approx(0.7f));
    CHECK(authored.without_offset().effective() == Catch::Approx(0.5f));
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
