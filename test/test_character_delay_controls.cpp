// Player-facing Character Delay control contracts.
//
// These tests keep API routing and audio behavior together: the lower-level
// character suite owns model fidelity, while this file proves that host-facing
// controls reach the intended signal path.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/character_delay/diffusion.hpp>

#include "support/character_delay_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
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

TEST_CASE("resonant loop tone preserves the unsaturated decay contract",
          "[character-delay][controls][feedback][tone][slow]") {
    auto release_ratio = [](Character character, float resonance, bool both_filters = false) {
        Engine delay;
        configure(delay, character, 20.0, 0.98, 0.0);
        if (both_filters) {
            delay.set_loop_low_cut_hz(1000.0f);
            delay.set_loop_low_cut_resonance(resonance);
        }
        delay.set_loop_high_cut_hz(1000.0f);
        delay.set_loop_high_cut_resonance(resonance);
        delay.reset();

        auto audio = burst_left(static_cast<int>(8.0 * kSr), 1000.0, 0.1, 0.01f);
        render(delay, audio);
        const double driven = peak(audio.left, 0, static_cast<int>(1.0 * kSr));
        const double released =
            peak(audio.left, static_cast<int>(7.0 * kSr), static_cast<int>(8.0 * kSr));
        REQUIRE(all_finite(audio.left));
        REQUIRE(driven > 1e-6);
        return std::pair{driven, released / driven};
    };

    const auto [butterworth_peak, butterworth_release] = release_ratio(Character::clean, 0.707f);
    const auto [resonant_peak, resonant_release] = release_ratio(Character::clean, 2.0f);
    const auto [diffusion_peak, diffusion_release] = release_ratio(Character::diffusion, 2.0f);
    const auto [cascade_peak, cascade_release] = release_ratio(Character::clean, 2.0f, true);
    INFO("peak/release at Q=.707: "
         << butterworth_peak << "/" << butterworth_release << ", Q=2: " << resonant_peak << "/"
         << resonant_release << ", diffusion Q=2: " << diffusion_peak << "/" << diffusion_release
         << ", HP+LP Q=2: " << cascade_peak << "/" << cascade_release);
    CHECK(butterworth_peak < 0.1);
    CHECK(butterworth_release < 1.0);
    CHECK(resonant_peak < 0.1);
    CHECK(resonant_release < 1.0);
    CHECK(diffusion_peak < 0.1);
    CHECK(diffusion_release < 1.0);
    CHECK(cascade_peak < 0.1);
    CHECK(cascade_release < 1.0);
}

TEST_CASE("loop-tone cutoff is clamped below Nyquist before prewarping",
          "[character-delay][controls][tone][sample-rate]") {
    auto render_low_rate = [](float low_cut_hz) {
        Engine delay;
        delay.set_character(Character::clean);
        delay.set_sample_rate(1000.0);
        delay.set_time_ms(1.0f);
        delay.set_feedback(0.0f);
        delay.set_character_amount(0.0f);
        delay.set_loop_low_cut_hz(low_cut_hz);
        delay.reset();

        auto audio = make_stereo(4000);
        for (int i = 0; i < 4000; ++i) {
            const float sample =
                static_cast<float>(0.2 * std::sin(2.0 * cd::kPi * 100.0 * i / 1000.0));
            audio.left[static_cast<std::size_t>(i)] = sample;
            audio.right[static_cast<std::size_t>(i)] = sample;
        }
        render(delay, audio);
        return audio;
    };

    const auto guarded = render_low_rate(450.0f);
    const auto above_nyquist = render_low_rate(2000.0f);
    CHECK(all_finite(above_nyquist.left));
    CHECK(difference_rms(guarded.left, above_nyquist.left, 1000) < 1e-9);
}

TEST_CASE("each loop-tone filter clears frozen state at its own bypass boundary",
          "[character-delay][controls][tone][transition]") {
    const auto peak_after_low_pass_reenable = [](bool keep_high_pass_active) {
        Engine delay;
        configure(delay, Character::clean, 20.0, 0.0, 0.0);
        if (keep_high_pass_active) delay.set_loop_low_cut_hz(200.0f);
        delay.set_loop_high_cut_hz(500.0f);
        delay.set_loop_high_cut_resonance(2.0f);
        delay.reset();

        auto charge = sine_both(static_cast<int>(0.5 * kSr), 500.0, 0.2f);
        render(delay, charge);
        delay.set_loop_high_cut_hz(static_cast<float>(cd::kLoopToneLpMaxHz));
        auto clear = make_stereo(static_cast<int>(0.2 * kSr));
        render(delay, clear);
        delay.set_loop_high_cut_hz(500.0f);
        auto after = make_stereo(static_cast<int>(0.1 * kSr));
        render(delay, after);
        return peak(after.left, 0, static_cast<int>(after.left.size()));
    };

    const double stage_boundary_peak = peak_after_low_pass_reenable(false);
    const double individual_boundary_peak = peak_after_low_pass_reenable(true);
    INFO("peak after LP re-enable on silence, full-stage/HP-still-active: "
         << stage_boundary_peak << "/" << individual_boundary_peak);
    CHECK(stage_boundary_peak < 1e-8);
    CHECK(individual_boundary_peak < 1e-8);
}

TEST_CASE("live diffusion amount automation does not step tank tap positions",
          "[character-delay][controls][diffusion][automation]") {
    using Diffusion = pulp::signal::chardelay::DiffusionChain;
    auto render_chain = [](int update_interval) {
        Diffusion chain;
        chain.prepare(kSr);
        chain.update(update_interval > 0 ? 0.0 : 0.5);
        chain.reset();

        std::vector<float> output(static_cast<std::size_t>(2.0 * kSr));
        for (std::size_t i = 0; i < output.size(); ++i) {
            if (update_interval > 0 && i % static_cast<std::size_t>(update_interval) == 0) {
                const double amount =
                    std::min(1.0, static_cast<double>(i) / static_cast<double>(kSr));
                chain.update(amount);
            }
            const double input =
                0.05 * std::sin(2.0 * cd::kPi * 700.0 * static_cast<double>(i) / kSr);
            output[i] = static_cast<float>(chain.process_cloud(input, 1.0));
            chain.tick_modulation();
        }
        return output;
    };

    const auto fixed = render_chain(0);
    const auto automated = render_chain(cd::kControlInterval);
    const auto smooth_control = render_chain(1);
    const double fixed_step =
        max_step(fixed, static_cast<int>(0.2 * kSr), static_cast<int>(1.0 * kSr));
    const double automated_step =
        max_step(automated, static_cast<int>(0.2 * kSr), static_cast<int>(1.0 * kSr));
    double automated_boundary_step = 0.0;
    double automated_between_step = 0.0;
    double error_boundary_step = 0.0;
    double error_between_step = 0.0;
    double error_boundary_energy = 0.0;
    double error_between_energy = 0.0;
    std::size_t error_boundary_count = 0;
    std::size_t error_between_count = 0;
    for (std::size_t i = static_cast<std::size_t>(0.2 * kSr);
         i < static_cast<std::size_t>(1.0 * kSr); ++i) {
        const double step =
            std::abs(static_cast<double>(automated[i]) - static_cast<double>(automated[i - 1]));
        const double error = static_cast<double>(automated[i]) - smooth_control[i];
        const double previous_error = static_cast<double>(automated[i - 1]) - smooth_control[i - 1];
        const double error_step = std::abs(error - previous_error);
        if (i % static_cast<std::size_t>(cd::kControlInterval) == 0) {
            automated_boundary_step = std::max(automated_boundary_step, step);
            error_boundary_step = std::max(error_boundary_step, error_step);
            error_boundary_energy += error_step * error_step;
            ++error_boundary_count;
        } else {
            automated_between_step = std::max(automated_between_step, step);
            error_between_step = std::max(error_between_step, error_step);
            error_between_energy += error_step * error_step;
            ++error_between_count;
        }
    }
    const double error_boundary_rms =
        std::sqrt(error_boundary_energy / static_cast<double>(error_boundary_count));
    const double error_between_rms =
        std::sqrt(error_between_energy / static_cast<double>(error_between_count));
    INFO("fixed max step " << fixed_step << ", automated max step " << automated_step
                           << ", boundary " << automated_boundary_step << ", between "
                           << automated_between_step << ", oracle-error boundary "
                           << error_boundary_step << ", between " << error_between_step
                           << ", RMS boundary " << error_boundary_rms << ", between "
                           << error_between_rms);
    CHECK(automated_step < 2.0 * fixed_step);
    CHECK(automated_boundary_step < 1.2 * automated_between_step);
    CHECK(error_boundary_step < 1.2 * error_between_step);
    CHECK(error_boundary_rms < 1.2 * error_between_rms);
}
