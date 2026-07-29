// Player-facing controls added for the Character Delay plugin.
//
// These tests keep API routing and audio behavior together: the lower-level
// character suite owns model fidelity, while this file proves that host-facing
// controls reach the intended signal path.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/character_delay_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace pulp::test::character_delay;

namespace {

double difference_rms(const std::vector<float>& a, const std::vector<float>& b,
                      int from) {
    REQUIRE(a.size() == b.size());
    double energy = 0.0;
    for (std::size_t i = static_cast<std::size_t>(std::max(from, 0)); i < a.size(); ++i) {
        const double difference = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        energy += difference * difference;
    }
    const auto count = a.size() - static_cast<std::size_t>(std::max(from, 0));
    return count > 0 ? std::sqrt(energy / static_cast<double>(count)) : 0.0;
}

Stereo render_tone_with_cuts(double frequency_hz, double low_cut_hz,
                             double high_cut_hz) {
    Engine delay;
    configure(delay, Character::clean, 20.0, 0.0, 0.0);
    delay.set_loop_low_cut_hz(static_cast<float>(low_cut_hz));
    delay.set_loop_high_cut_hz(static_cast<float>(high_cut_hz));
    delay.reset();
    auto audio = sine_both(static_cast<int>(1.0 * kSr), frequency_hz, 0.2f);
    render(delay, audio);
    return audio;
}

}  // namespace

TEST_CASE("absolute right time is independent until ratio mode is restored",
          "[character-delay][controls][time]") {
    Engine delay;
    configure(delay, Character::clean, 240.0, 0.0, 0.0);

    delay.set_right_time_ms(480.0f);
    CHECK(delay.right_time_is_absolute());
    CHECK(delay.right_time_ms() == Catch::Approx(480.0));

    delay.set_time_ms(700.0f);
    CHECK(delay.right_time_ms() == Catch::Approx(480.0));

    delay.reset();
    settle(delay, 4.0 * slew_seconds(Character::clean) + 0.2);
    auto audio = make_stereo(static_cast<int>(0.8 * kSr));
    audio.left[0] = 1.0f;
    audio.right[0] = 1.0f;
    render(delay, audio);
    const int right_peak = peak_index(audio.right, 1, static_cast<int>(audio.right.size()));
    INFO("absolute right repeat at " << right_peak);
    CHECK(std::abs(right_peak - 0.480 * kSr) <= 0.001 * kSr);

    delay.set_time_offset(0.5f);
    CHECK_FALSE(delay.right_time_is_absolute());
    CHECK(delay.right_time_ms() == Catch::Approx(350.0));
}

TEST_CASE("invalid stereo-time writes do not switch modes",
          "[character-delay][api][controls][time]") {
    constexpr float bad = std::numeric_limits<float>::quiet_NaN();
    Engine delay;
    configure(delay, Character::clean, 400.0, 0.0, 0.0);

    delay.set_right_time_ms(620.0f);
    delay.set_time_offset(bad);
    CHECK(delay.right_time_is_absolute());
    CHECK(delay.right_time_ms() == Catch::Approx(620.0));

    delay.set_time_offset(1.25f);
    delay.set_right_time_ms(bad);
    CHECK_FALSE(delay.right_time_is_absolute());
    CHECK(delay.right_time_ms() == Catch::Approx(500.0));
}

TEST_CASE("loop tone is bit-exact when open and filters the wet signal when active",
          "[character-delay][controls][tone]") {
    Engine reference;
    Engine open;
    configure(reference, Character::clean, 20.0, 0.4, 0.0);
    configure(open, Character::clean, 20.0, 0.4, 0.0);
    open.set_loop_low_cut_hz(static_cast<float>(cd::kLoopToneHpMinHz));
    open.set_loop_high_cut_hz(static_cast<float>(cd::kLoopToneLpMaxHz));
    CHECK_FALSE(open.loop_tone_active());

    auto expected = sine_both(static_cast<int>(0.5 * kSr), 1000.0, 0.2f);
    auto actual = expected;
    render(reference, expected);
    render(open, actual);
    CHECK(actual.left == expected.left);
    CHECK(actual.right == expected.right);

    const auto low_reference =
        render_tone_with_cuts(100.0, cd::kLoopToneHpMinHz, cd::kLoopToneLpMaxHz);
    const auto low_cut =
        render_tone_with_cuts(100.0, 2000.0, cd::kLoopToneLpMaxHz);
    const auto high_reference =
        render_tone_with_cuts(8000.0, cd::kLoopToneHpMinHz, cd::kLoopToneLpMaxHz);
    const auto high_cut =
        render_tone_with_cuts(8000.0, cd::kLoopToneHpMinHz, 500.0);
    const int measure_from = static_cast<int>(0.5 * kSr);
    CHECK(rms(low_cut.left, measure_from, static_cast<int>(kSr)) <
          0.1 * rms(low_reference.left, measure_from, static_cast<int>(kSr)));
    CHECK(rms(high_cut.left, measure_from, static_cast<int>(kSr)) <
          0.1 * rms(high_reference.left, measure_from, static_cast<int>(kSr)));
}

TEST_CASE("cross-character diffusion adds an output cloud without materially moving the repeat",
          "[character-delay][controls][diffusion]") {
    auto render_amount = [](float amount) {
        Engine delay;
        configure(delay, Character::clean, 100.0, 0.0, 0.0);
        delay.set_diffusion_amount(amount);
        delay.reset();
        auto audio = burst_left(static_cast<int>(1.0 * kSr), 700.0, 0.01, 0.5f);
        render(delay, audio);
        return audio;
    };

    const auto dry = render_amount(0.0f);
    const auto diffused = render_amount(1.0f);
    CHECK(std::abs(onset_index(diffused.left, 0.05) -
                   onset_index(dry.left, 0.05)) <= 16);
    CHECK(difference_rms(diffused.left, dry.left, static_cast<int>(0.1 * kSr)) >
          1e-4);
    CHECK(rms(diffused.left, static_cast<int>(0.15 * kSr),
              static_cast<int>(0.6 * kSr)) >
          10.0 * rms(dry.left, static_cast<int>(0.15 * kSr),
                     static_cast<int>(0.6 * kSr)));
}

TEST_CASE("the modulation control drives the BBD bucket clock",
          "[character-delay][bbd][controls][modulation]") {
    auto render_depth = [](float depth) {
        Engine delay;
        configure(delay, Character::bbd, 100.0, 0.0, 0.5);
        delay.set_mod(0.5f, depth);
        delay.reset();
        settle(delay, 4.0 * slew_seconds(Character::bbd) + 0.2);
        auto audio = sine_both(static_cast<int>(2.0 * kSr), 800.0, 0.2f);
        render(delay, audio);
        return audio;
    };

    const auto still = render_depth(0.0f);
    const auto modulated = render_depth(1.0f);
    const double difference =
        difference_rms(modulated.left, still.left, static_cast<int>(0.5 * kSr));
    INFO("BBD modulation difference RMS " << difference);
    CHECK(difference > 1e-3);
}
