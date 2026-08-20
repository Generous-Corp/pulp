#include <pulp/signal/commuted_string_excitation.hpp>
#include <pulp/signal/karplus_strong.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace {

using pulp::signal::CommutedStringExcitationPrepareStatus;

template <typename SampleType>
std::vector<SampleType> direct_convolution(std::span<const SampleType> signal,
                                           std::span<const SampleType> impulse_response) {
    std::vector<SampleType> output(signal.size() + impulse_response.size() - 1, SampleType{0});
    for (std::size_t signal_index = 0; signal_index < signal.size(); ++signal_index) {
        for (std::size_t impulse_index = 0; impulse_index < impulse_response.size();
             ++impulse_index) {
            output[signal_index + impulse_index] +=
                signal[signal_index] * impulse_response[impulse_index];
        }
    }
    return output;
}

std::vector<double> render_string(std::span<const double> excitation, std::size_t output_samples) {
    pulp::signal::KarplusStrongT<double> string;
    string.prepare(48000.0, 40.0);
    string.set_frequency(100.0);
    string.set_decay_seconds(4.0);
    string.set_damping(0.0);
    string.set_stiffness(0.0);
    string.set_pluck_position(0.0);
    string.set_dynamic_bandwidth_hz(20000.0);
    string.set_pick_direction(0.0);
    string.reset();
    string.pluck();

    std::vector<double> output(output_samples, 0.0);
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto input = index < excitation.size() ? excitation[index] : 0.0;
        output[index] = string.process(input);
    }
    return output;
}

} // namespace

TEST_CASE("Commuted string excitation matches direct finite convolution",
          "[signal][string][commuted-excitation]") {
    constexpr std::array base{0.5, -0.25, 0.75, 0.125};
    constexpr std::array body{0.8, -0.3, 0.1};
    const auto expected = direct_convolution<double>(base, body);

    pulp::signal::CommutedStringExcitationProfileT<double, 16> profile;
    REQUIRE(profile.prepare(base, body) == CommutedStringExcitationPrepareStatus::ok);
    REQUIRE(profile.retained_samples() == expected.size());
    REQUIRE(profile.maximum_retained_samples == 16);
    REQUIRE(std::equal(profile.samples().begin(), profile.samples().end(), expected.begin(),
                       expected.end()));
}

TEST_CASE("Commuted string excitation handles identity and silent bodies",
          "[signal][string][commuted-excitation]") {
    constexpr std::array base{0.25f, -0.5f, 1.0f};
    constexpr std::array delta{1.0f};
    constexpr std::array zero{0.0f, 0.0f, 0.0f};

    pulp::signal::CommutedStringExcitationProfileT<float, 8> profile;
    REQUIRE(profile.prepare(base, delta) == CommutedStringExcitationPrepareStatus::ok);
    REQUIRE(
        std::equal(profile.samples().begin(), profile.samples().end(), base.begin(), base.end()));

    REQUIRE(profile.prepare(base, zero) == CommutedStringExcitationPrepareStatus::ok);
    for (const auto sample : profile.samples())
        REQUIRE(sample == 0.0f);
}

TEST_CASE("Commuted string excitation rejects invalid preparation transactionally",
          "[signal][string][commuted-excitation]") {
    constexpr std::array initial_base{0.5f, -0.25f};
    constexpr std::array initial_body{1.0f, 0.5f};
    constexpr std::array too_long_base{1.0f, 2.0f, 3.0f, 4.0f};
    constexpr std::array too_long_body{1.0f, 2.0f};
    const std::array non_finite_body{1.0f, std::numeric_limits<float>::quiet_NaN()};
    const std::array overflowing_body{2.0f};

    pulp::signal::CommutedStringExcitationProfileT<float, 4> profile;
    REQUIRE(profile.prepare(initial_base, initial_body) ==
            CommutedStringExcitationPrepareStatus::ok);
    const std::vector<float> preserved(profile.samples().begin(), profile.samples().end());

    REQUIRE(profile.prepare(too_long_base, too_long_body) ==
            CommutedStringExcitationPrepareStatus::retained_sample_limit_exceeded);
    REQUIRE(std::equal(profile.samples().begin(), profile.samples().end(), preserved.begin(),
                       preserved.end()));

    constexpr std::array maximum_base{std::numeric_limits<float>::max()};
    REQUIRE(profile.prepare(maximum_base, overflowing_body) ==
            CommutedStringExcitationPrepareStatus::non_finite_output);
    REQUIRE(std::equal(profile.samples().begin(), profile.samples().end(), preserved.begin(),
                       preserved.end()));

    REQUIRE(profile.prepare(profile.samples(), initial_body) ==
            CommutedStringExcitationPrepareStatus::input_aliases_profile);
    REQUIRE(std::equal(profile.samples().begin(), profile.samples().end(), preserved.begin(),
                       preserved.end()));

    REQUIRE(profile.prepare(initial_base, non_finite_body) ==
            CommutedStringExcitationPrepareStatus::non_finite_input);
    REQUIRE(std::equal(profile.samples().begin(), profile.samples().end(), preserved.begin(),
                       preserved.end()));
}

TEST_CASE("Commuted string excitation cursors are independent and retriggerable",
          "[signal][string][commuted-excitation]") {
    constexpr std::array base{1.0f, 0.5f, -0.25f};
    constexpr std::array body{1.0f};
    pulp::signal::CommutedStringExcitationProfileT<float, 8> profile;
    REQUIRE(profile.prepare(base, body) == CommutedStringExcitationPrepareStatus::ok);

    auto first = profile.make_cursor();
    auto second = profile.make_cursor();
    REQUIRE_FALSE(first.active());
    REQUIRE(first.next() == 0.0f);

    first.trigger();
    second.trigger();
    REQUIRE(first.next() == 1.0f);
    REQUIRE(first.next() == 0.5f);
    REQUIRE(second.next() == 1.0f);
    REQUIRE(second.position() == 1);

    first.retrigger();
    REQUIRE(first.next() == 1.0f);
    first.reset();
    REQUIRE_FALSE(first.active());
    REQUIRE(first.next() == 0.0f);
    REQUIRE(second.next() == 0.5f);
    REQUIRE(second.remaining() == 1);
    REQUIRE(second.next() == -0.25f);
    REQUIRE_FALSE(second.active());
    REQUIRE(second.remaining() == 0);
    REQUIRE(second.next() == 0.0f);
}

TEST_CASE("Commuted string excitation rendering is block-partition invariant",
          "[signal][string][commuted-excitation]") {
    constexpr std::array base{0.5, -0.25, 0.75, 0.125};
    constexpr std::array body{0.8, -0.3, 0.1};
    pulp::signal::CommutedStringExcitationProfileT<double, 16> profile;
    REQUIRE(profile.prepare(base, body) == CommutedStringExcitationPrepareStatus::ok);

    std::array<double, 10> whole{};
    auto whole_cursor = profile.make_cursor();
    whole_cursor.trigger();
    whole_cursor.render(whole);

    std::array<double, 10> partitioned{};
    auto partitioned_cursor = profile.make_cursor();
    partitioned_cursor.trigger();
    partitioned_cursor.render(std::span{partitioned}.first(2));
    partitioned_cursor.render(std::span{partitioned}.subspan(2, 5));
    partitioned_cursor.render(std::span{partitioned}.last(3));
    REQUIRE(partitioned == whole);
}

TEST_CASE("Body filtering commutes across a fixed linear string",
          "[signal][string][commuted-excitation]") {
    constexpr std::array base{0.8, -0.2, 0.5, 0.1, -0.05};
    constexpr std::array body{0.75, 0.2, -0.1, 0.05};
    constexpr std::size_t output_samples = 4096;

    pulp::signal::CommutedStringExcitationProfileT<double, 32> profile;
    REQUIRE(profile.prepare(base, body) == CommutedStringExcitationPrepareStatus::ok);

    const auto uncommuted_string = render_string(base, output_samples);
    const auto body_after_string = direct_convolution<double>(uncommuted_string, body);
    const auto commuted_string = render_string(profile.samples(), output_samples);

    REQUIRE(body_after_string.size() >= commuted_string.size());
    for (std::size_t index = 0; index < commuted_string.size(); ++index) {
        INFO("sample " << index);
        REQUIRE(std::abs(commuted_string[index] - body_after_string[index]) < 1.0e-10);
    }
}

TEST_CASE("Commuted string cursor hot paths allocate nothing",
          "[signal][string][commuted-excitation][rt-safety]") {
    constexpr std::array base{0.5f, -0.25f, 0.75f, 0.125f};
    constexpr std::array body{0.8f, -0.3f, 0.1f};
    pulp::signal::CommutedStringExcitationProfileT<float, 16> profile;
    REQUIRE(profile.prepare(base, body) == CommutedStringExcitationPrepareStatus::ok);

    auto cursor = profile.make_cursor();
    std::array<float, 64> output{};
    std::size_t allocations = 1;
    {
        pulp::test::RtAllocationProbe probe;
        cursor.trigger();
        cursor.render(std::span{output}.first(17));
        cursor.retrigger();
        for (std::size_t index = 17; index < output.size(); ++index) {
            output[index] = cursor.next();
        }
        cursor.reset();
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
    REQUIRE(std::all_of(output.begin(), output.end(),
                        [](float sample) { return std::isfinite(sample); }));
}

static_assert(noexcept(std::declval<pulp::signal::CommutedStringExcitationCursor&>().trigger()));
static_assert(noexcept(std::declval<pulp::signal::CommutedStringExcitationCursor&>().next()));
static_assert(noexcept(std::declval<pulp::signal::CommutedStringExcitationCursor&>().render(
    std::declval<std::span<float>>())));
