#include "test_pulp_sampler_heritage_support.hpp"

TEST_CASE("PulpSampler heritage render is bitwise callback-partition invariant",
          "[audio][sampler][heritage][partition]") {
    const auto profile = two_leg_profile();
    auto sample = make_sine(48000);
    HeritageFixture whole(1024, &profile);
    HeritageFixture split(1024, &profile);
    whole.load(sample);
    split.load(sample);
    constexpr std::array one{std::size_t{1024}};
    constexpr std::array many{std::size_t{1}, std::size_t{31},
                              std::size_t{128}, std::size_t{7},
                              std::size_t{333}, std::size_t{524}};
    REQUIRE(std::accumulate(many.begin(), many.end(), std::size_t{0}) == 1024);
    REQUIRE(render(whole, one) == render(split, many));
}

TEST_CASE("PulpSampler heritage clock ratio raises sampler pitch",
          "[audio][sampler][heritage][pitch]") {
    const auto profile = clock_profile(2.0);
    auto sample = make_sine(48000);
    HeritageFixture fixture(4096, &profile);
    fixture.load(sample);
    constexpr std::array block{std::size_t{4096}};
    const auto output = render(fixture, block);
    const auto measured = std::span<const float>(output).subspan(256);
    REQUIRE(tone_projection(measured, 880.0, 48000.0) > 0.8);
    REQUIRE(tone_projection(measured, 880.0, 48000.0) >
            tone_projection(measured, 440.0, 48000.0) * 8.0);
}

TEST_CASE("PulpSampler heritage preserves host-offset MIDI causality",
          "[audio][sampler][heritage][midi]") {
    const auto profile = clock_profile(2.0);
    auto sample = make_sine(48000);
    HeritageFixture fixture(256, &profile);
    fixture.load(sample);
    constexpr std::array block{std::size_t{256}};
    const auto output = render(fixture, block, 64);
    REQUIRE(std::all_of(output.begin(), output.begin() + 64,
                        [](float sample_value) { return sample_value == 0.0f; }));
    REQUIRE(std::any_of(output.begin() + 64, output.end(),
                        [](float sample_value) {
                            return std::abs(sample_value) > 0.01f;
                        }));
}

TEST_CASE("PulpSampler heritage reports and renders causal impulse latency",
          "[audio][sampler][heritage][latency]") {
    const auto profile = clock_profile(2.0);
    std::vector<float> impulse(512, 0.0f);
    impulse[0] = 1.0f;
    HeritageFixture fixture(128, &profile);
    fixture.load(impulse);
    constexpr std::array block{std::size_t{128}};
    const auto output = render(fixture, block);
    const auto peak = static_cast<std::size_t>(std::distance(
        output.begin(), std::max_element(output.begin(), output.end(),
            [](float left, float right) {
                return std::abs(left) < std::abs(right);
            })));
    REQUIRE(fixture.processor.latency_samples() == 12);
    REQUIRE(peak == 12);
    REQUIRE(fixture.processor.descriptor().tail_samples == -1);
}

TEST_CASE("PulpSampler all-bypassed heritage uses the exact legacy render path",
          "[audio][sampler][heritage][bypass]") {
    const auto profile = clock_profile(2.0, true);
    auto sample = make_sine(48000);
    HeritageFixture disabled(512);
    HeritageFixture bypassed(512, &profile);
    disabled.load(sample);
    bypassed.load(sample);
    constexpr std::array blocks{std::size_t{17}, std::size_t{63},
                                std::size_t{128}, std::size_t{304}};
    REQUIRE(render(disabled, blocks) == render(bypassed, blocks));
    REQUIRE(bypassed.processor.latency_samples() == 0);
}
