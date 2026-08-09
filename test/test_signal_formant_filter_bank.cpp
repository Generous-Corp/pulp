#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/formant_filter_bank.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Bank = pulp::signal::FormantFilterBankT<double>;
using Spec = Bank::FormantSpec;
using Status = pulp::signal::FormantConfigureStatus;

namespace {

constexpr double sample_rate = 48000.0;

double impulse_dft_magnitude(Bank bank, double frequency, std::size_t length = 32768) {
    std::complex<long double> response{};
    const long double step =
        -2.0L * std::numbers::pi_v<long double> * static_cast<long double>(frequency / sample_rate);
    for (std::size_t n = 0; n < length; ++n) {
        const long double sample = bank.process(n == 0 ? 1.0 : 0.0);
        response += sample * std::polar(1.0L, step * static_cast<long double>(n));
    }
    return static_cast<double>(std::abs(response));
}

double settled_sine_gain(Bank bank, double frequency) {
    constexpr std::size_t warmup = 24000;
    constexpr std::size_t measured = 24000;
    long double input_energy = 0.0L;
    long double output_energy = 0.0L;
    for (std::size_t n = 0; n < warmup + measured; ++n) {
        const double input =
            std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(n) / sample_rate);
        const double output = bank.process(input);
        if (n >= warmup) {
            input_energy += input * input;
            output_energy += output * output;
        }
    }
    return std::sqrt(static_cast<double>(output_energy / input_energy));
}

std::array<Spec, 3> compact_a() {
    return {{{800.0, 80.0, 0.0}, {1150.0, 90.0, -4.0}, {2900.0, 120.0, -9.0}}};
}

double digital_t60(const pulp::signal::BiquadCoefficientsT<double>& c, double rate) {
    const std::complex<double> discriminant =
        std::sqrt(std::complex<double>{c.a1 * c.a1 - 4.0 * c.a2, 0.0});
    const double radius =
        std::max(std::abs((-c.a1 + discriminant) * 0.5), std::abs((-c.a1 - discriminant) * 0.5));
    return -std::log(0.001) / (-std::log(radius) * rate);
}

} // namespace

TEST_CASE("formant recipe validation is finite ordered bounded and transactional") {
    Bank bank;
    REQUIRE(bank.prepare(sample_rate, 3));
    const auto valid = compact_a();
    REQUIRE(bank.configure(valid, 3.0) == Status::configured);
    const auto committed = bank.requested_recipe();
    const auto committed_coefficients = bank.coefficients(0);

    auto invalid = valid;
    invalid[1].frequency_hz = std::numeric_limits<double>::quiet_NaN();
    CHECK(bank.configure(invalid) == Status::non_finite);
    invalid = valid;
    std::swap(invalid[0], invalid[1]);
    CHECK(bank.configure(invalid) == Status::unordered);
    invalid = valid;
    invalid[0].bandwidth_hz = 1.0;
    CHECK(bank.configure(invalid) == Status::out_of_range);
    invalid = valid;
    invalid[0].gain_db = 25.0;
    CHECK(bank.configure(invalid) == Status::out_of_range);
    CHECK(bank.configure(std::span<const Spec>{}) == Status::invalid_count);

    const auto after = bank.requested_recipe();
    CHECK(after.count == committed.count);
    CHECK(after.formants[0].frequency_hz == committed.formants[0].frequency_hz);
    CHECK(bank.coefficients(0).b0 == committed_coefficients.b0);
    CHECK(bank.coefficients(0).a2 == committed_coefficients.a2);
}

TEST_CASE("coefficient validation rejects numerically marginal extreme rates") {
    pulp::signal::FormantFilterBank bank;
    CHECK_FALSE(bank.prepare(1.0e100, 1));
    REQUIRE(bank.prepare(1.0e38, 1));
    const std::array<pulp::signal::FormantFilterBank::FormantSpec, 1> recipe{{
        {1000.0, 100.0, 0.0},
    }};
    CHECK(bank.configure(recipe) == Status::unstable);
    CHECK(bank.requested_recipe().count == 0);
}

TEST_CASE("vowel recipes morph continuously in log frequency") {
    const auto a = Bank::vowel_recipe(pulp::signal::FormantVowel::a);
    const auto u = Bank::vowel_recipe(pulp::signal::FormantVowel::u);
    const auto middle = Bank::interpolate(a, u, 0.5);
    REQUIRE(a.count == 5);
    REQUIRE(u.count == 5);
    REQUIRE(middle.count == 5);
    CHECK_THAT(
        middle.formants[0].frequency_hz,
        WithinRel(std::sqrt(a.formants[0].frequency_hz * u.formants[0].frequency_hz), 1e-12));
    for (std::size_t i = 1; i < middle.count; ++i)
        CHECK(middle.formants[i - 1].frequency_hz < middle.formants[i].frequency_hz);

    Bank bank;
    REQUIRE(bank.prepare(sample_rate));
    CHECK(bank.configure_vowel_morph(pulp::signal::FormantVowel::a, pulp::signal::FormantVowel::u,
                                     0.5) == Status::configured);
    CHECK(bank.configure_vowel_morph(pulp::signal::FormantVowel::a, pulp::signal::FormantVowel::u,
                                     std::numeric_limits<double>::infinity()) ==
          Status::non_finite);
}

TEST_CASE("recipe interpolation bounds malformed public counts to fixed storage") {
    auto from = Bank::vowel_recipe(pulp::signal::FormantVowel::a);
    auto to = Bank::vowel_recipe(pulp::signal::FormantVowel::u);
    from.count = Bank::storage_capacity + 100;
    to.count = Bank::storage_capacity + 200;
    const auto result = Bank::interpolate(from, to, 0.5);
    CHECK(result.count == Bank::storage_capacity);
}

TEST_CASE("independent impulse and sine oracles locate the requested formant") {
    const std::array<Spec, 1> target{{{800.0, 80.0, 0.0}}};
    Bank bank;
    REQUIRE(bank.prepare(sample_rate, 1));
    bank.set_transition_samples(0);
    REQUIRE(bank.configure(target, 3.0) == Status::configured);

    const double reported = bank.magnitude(800.0);
    const double impulse = impulse_dft_magnitude(bank, 800.0);
    const double sine = settled_sine_gain(bank, 800.0);
    CHECK_THAT(impulse, WithinRel(reported, 2e-6));
    CHECK_THAT(sine, WithinRel(reported, 2e-6));
    CHECK_THAT(reported, WithinRel(std::pow(10.0, -3.0 / 20.0), 2e-6));

    // Negative control: the same oracle must not "find" 800 Hz when the only
    // resonator has deliberately moved more than an octave away.
    const std::array<Spec, 1> shifted{{{1800.0, 80.0, 0.0}}};
    Bank wrong;
    REQUIRE(wrong.prepare(sample_rate, 1));
    wrong.set_transition_samples(0);
    REQUIRE(wrong.configure(shifted, 3.0) == Status::configured);
    CHECK(impulse_dft_magnitude(wrong, 800.0) < impulse * 0.1);
}

TEST_CASE("normalization enforces the documented steady-state headroom bound") {
    Bank bank;
    REQUIRE(bank.prepare(sample_rate, 3));
    const auto recipe = compact_a();
    REQUIRE(bank.configure(recipe, 6.0) == Status::configured);
    const double bound = std::pow(10.0, -6.0 / 20.0);
    double maximum = 0.0;
    for (std::size_t i = 0; i <= 12000; ++i) {
        const double frequency = sample_rate * 0.5 * static_cast<double>(i) / 12000.0;
        maximum = std::max(maximum, bank.magnitude(frequency));
    }
    CHECK(maximum <= bound * (1.0 + 2e-6));
    CHECK(maximum > 0.25);
}

TEST_CASE("response curves clamp an ordinary display range to Nyquist") {
    Bank bank;
    REQUIRE(bank.prepare(32000.0, 1));
    const std::array<Spec, 1> recipe{{{800.0, 80.0, 0.0}}};
    REQUIRE(bank.configure(recipe) == Status::configured);
    std::array<double, 3> curve{};
    bank.response_curve_db(20.0, 20000.0, curve);
    CHECK_THAT(curve.front(), WithinAbs(bank.magnitude_db(40.0), 1e-12));
    CHECK_THAT(curve.back(), WithinAbs(bank.magnitude_db(16000.0), 1e-12));
    CHECK(curve.front() != curve.back());
}

TEST_CASE("unprepared response inspection is a defined bypass curve") {
    Bank bank;
    std::array<double, 4> curve{1.0, 2.0, 3.0, 4.0};
    bank.response_curve_db(20.0, 20000.0, curve);
    CHECK(curve == std::array<double, 4>{});
    CHECK(bank.magnitude(1000.0) == 1.0);
    CHECK(bank.magnitude_db(1000.0) == 0.0);
}

TEST_CASE("sample block in-place and arbitrary partitions are deterministic") {
    std::array<Bank, 3> banks;
    for (auto& bank : banks) {
        REQUIRE(bank.prepare(sample_rate, 3));
        bank.set_transition_samples(0);
        REQUIRE(bank.configure(compact_a()) == Status::configured);
        bank.set_transition_samples(257);
        const std::array<Spec, 3> next{{
            {500.0, 70.0, 0.0},
            {1500.0, 100.0, -3.0},
            {2600.0, 130.0, -8.0},
        }};
        REQUIRE(bank.configure(next) == Status::configured);
    }

    std::vector<double> input(4096);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = 0.2 * std::sin(0.017 * static_cast<double>(i)) + (i % 503 == 0 ? 0.5 : 0.0);
    std::vector<double> scalar(input.size());
    std::vector<double> partitioned(input.size());
    auto inplace = input;
    for (std::size_t i = 0; i < input.size(); ++i)
        scalar[i] = banks[0].process(input[i]);
    const std::array<std::size_t, 7> chunks{1, 17, 64, 3, 511, 29, 127};
    std::size_t offset = 0;
    std::size_t chunk = 0;
    while (offset < input.size()) {
        const std::size_t count = std::min(chunks[chunk++ % chunks.size()], input.size() - offset);
        REQUIRE(banks[1].process_block(input.data() + offset, partitioned.data() + offset, count));
        offset += count;
    }
    REQUIRE(banks[2].process_block(inplace.data(), inplace.size()));
    CHECK(partitioned == scalar);
    CHECK(inplace == scalar);
}

TEST_CASE("identical recipes are no-ops while idle and mid-transition") {
    Bank bank;
    REQUIRE(bank.prepare(sample_rate, 3));
    bank.set_transition_samples(64);
    const auto recipe = compact_a();
    REQUIRE(bank.configure(recipe) == Status::configured);
    REQUIRE(bank.transition_active());
    for (int i = 0; i < 10; ++i)
        bank.process(0.1);
    REQUIRE(bank.configure(recipe) == Status::configured);
    for (int i = 0; i < 54; ++i)
        bank.process(0.1);
    CHECK_FALSE(bank.transition_active());
    REQUIRE(bank.configure(recipe) == Status::configured);
    CHECK_FALSE(bank.transition_active());
}

TEST_CASE("retunes use a click-safe transition by default") {
    Bank bank;
    REQUIRE(bank.prepare(sample_rate, 3));
    CHECK(bank.transition_samples() == Bank::default_transition_samples);
    REQUIRE(bank.configure(compact_a()) == Status::configured);
    REQUIRE(bank.transition_active());
    for (std::size_t i = 0; i < Bank::default_transition_samples; ++i)
        bank.process(0.0);
    CHECK_FALSE(bank.transition_active());
}

TEST_CASE("requesting the in-flight endpoint cancels a queued retune") {
    Bank bank;
    REQUIRE(bank.prepare(sample_rate, 3));
    bank.set_transition_samples(0);
    const auto first = compact_a();
    REQUIRE(bank.configure(first) == Status::configured);
    bank.set_transition_samples(64);
    const std::array<Spec, 3> endpoint{{
        {500.0, 70.0, 0.0},
        {1500.0, 100.0, -3.0},
        {2600.0, 130.0, -8.0},
    }};
    const std::array<Spec, 3> queued{{
        {600.0, 80.0, 0.0},
        {1700.0, 110.0, -4.0},
        {3000.0, 140.0, -9.0},
    }};
    REQUIRE(bank.configure(endpoint) == Status::configured);
    for (int i = 0; i < 10; ++i)
        bank.process(0.1);
    REQUIRE(bank.configure(queued) == Status::configured);
    REQUIRE(bank.configure(endpoint) == Status::configured);
    for (int i = 0; i < 54; ++i)
        bank.process(0.1);
    CHECK_FALSE(bank.transition_active());
    CHECK(bank.requested_recipe().formants[0].frequency_hz == endpoint[0].frequency_hz);
}

TEST_CASE("reset and nonfinite input recover to the same finite impulse response") {
    Bank recovered;
    Bank reference;
    for (Bank* bank : {&recovered, &reference}) {
        REQUIRE(bank->prepare(sample_rate, 3));
        bank->set_transition_samples(0);
        REQUIRE(bank->configure(compact_a()) == Status::configured);
    }
    for (int i = 0; i < 1000; ++i)
        recovered.process(0.5);
    CHECK(recovered.process(std::numeric_limits<double>::infinity()) == 0.0);
    CHECK(recovered.process(std::numeric_limits<double>::quiet_NaN()) == 0.0);
    reference.reset();
    for (int i = 0; i < 2048; ++i) {
        const double input = i == 0 ? 1.0 : 0.0;
        const double actual = recovered.process(input);
        CHECK(std::isfinite(actual));
        CHECK(actual == reference.process(input));
    }
}

TEST_CASE("latency tail and realtime allocation contracts are explicit") {
    Bank bank;
    REQUIRE(bank.prepare(sample_rate, 3));
    auto recipe = compact_a();
    {
        pulp::test::RtAllocationProbe probe;
        REQUIRE(bank.configure(recipe) == Status::configured);
        std::array<double, 128> samples{};
        samples[0] = 1.0;
        REQUIRE(bank.process_block(samples.data(), samples.size()));
        bank.reset();
        CHECK_FALSE(probe.saw_allocation());
    }
    CHECK(Bank::latency_samples() == 0);
    CHECK_THAT(bank.tail_seconds(),
               WithinRel(digital_t60(bank.coefficients(0), sample_rate), 1e-12));
}

TEST_CASE("tail reporting includes active in-flight and queued recipes") {
    Bank bank;
    REQUIRE(bank.prepare(sample_rate, 1));
    bank.set_transition_samples(64);
    const std::array<Spec, 1> first{{{800.0, 160.0, 0.0}}};
    const std::array<Spec, 1> long_tail{{{800.0, 20.0, 0.0}}};
    const std::array<Spec, 1> queued_short{{{800.0, 100.0, 0.0}}};
    REQUIRE(bank.configure(first) == Status::configured);
    for (int i = 0; i < 64; ++i)
        bank.process(0.0);
    REQUIRE(bank.configure(long_tail) == Status::configured);
    const auto long_tail_coefficients = bank.coefficients(0);
    bank.process(0.0);
    REQUIRE(bank.configure(queued_short) == Status::configured);
    CHECK_THAT(bank.tail_seconds(),
               WithinRel(digital_t60(long_tail_coefficients, sample_rate), 1e-12));
}

TEST_CASE("queued retunes retain a safe fade when immediate mode is selected") {
    Bank bank;
    REQUIRE(bank.prepare(sample_rate, 1));
    bank.set_transition_samples(0);
    const std::array<Spec, 1> first{{{500.0, 50.0, 0.0}}};
    const std::array<Spec, 1> second{{{1000.0, 70.0, 0.0}}};
    const std::array<Spec, 1> third{{{2000.0, 100.0, 0.0}}};
    REQUIRE(bank.configure(first) == Status::configured);
    bank.set_transition_samples(64);
    REQUIRE(bank.configure(second) == Status::configured);
    for (int i = 0; i < 10; ++i)
        bank.process(0.1);
    bank.set_transition_samples(0);
    REQUIRE(bank.configure(third) == Status::configured);
    for (int i = 0; i < 54; ++i)
        bank.process(0.1);
    REQUIRE(bank.transition_active());
    bank.process(0.1);
    CHECK(bank.transition_active());
}
