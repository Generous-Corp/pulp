#include <pulp/signal/filter_morph.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::FilterMorph64;
using pulp::signal::MorphFilterType;

namespace {

using Endpoint = FilterMorph64::Endpoint;

struct Coefficients {
    double b0, b1, b2, a1, a2;
};

Coefficients independent_design(MorphFilterType type, double frequency, double q,
                                double sample_rate) {
    const double w = 2.0 * std::acos(-1.0) * frequency / sample_rate;
    const double cosine = std::cos(w);
    const double alpha = std::sin(w) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    switch (type) {
    case MorphFilterType::lowpass:
        return {(1.0 - cosine) / (2.0 * a0), (1.0 - cosine) / a0, (1.0 - cosine) / (2.0 * a0),
                -2.0 * cosine / a0, (1.0 - alpha) / a0};
    case MorphFilterType::bandpass:
        return {alpha / a0, 0.0, -alpha / a0, -2.0 * cosine / a0, (1.0 - alpha) / a0};
    case MorphFilterType::highpass:
        return {(1.0 + cosine) / (2.0 * a0), -(1.0 + cosine) / a0, (1.0 + cosine) / (2.0 * a0),
                -2.0 * cosine / a0, (1.0 - alpha) / a0};
    case MorphFilterType::notch:
        return {1.0 / a0, -2.0 * cosine / a0, 1.0 / a0, -2.0 * cosine / a0, (1.0 - alpha) / a0};
    }
    return {};
}

std::complex<double> independent_response(const Coefficients& c, double frequency,
                                          double sample_rate) {
    const auto z1 = std::polar(1.0, -2.0 * std::acos(-1.0) * frequency / sample_rate);
    return (c.b0 + c.b1 * z1 + c.b2 * z1 * z1) / (1.0 + c.a1 * z1 + c.a2 * z1 * z1);
}

class ReferenceBiquad {
  public:
    explicit ReferenceBiquad(Coefficients c) : c_(c) {}
    double process(double input) {
        const double output = c_.b0 * input + s1_;
        s1_ = c_.b1 * input - c_.a1 * output + s2_;
        s2_ = c_.b2 * input - c_.a2 * output;
        return output;
    }

  private:
    Coefficients c_;
    double s1_ = 0.0, s2_ = 0.0;
};

constexpr Endpoint low{MorphFilterType::lowpass, 900.0, 0.7071067811865476};
constexpr Endpoint high{MorphFilterType::highpass, 2200.0, 1.1};

} // namespace

TEST_CASE("filter morph validates endpoint sets transactionally", "[signal][filter-morph]") {
    FilterMorph64 filter;
    REQUIRE(filter.configure(48000.0, low, high));
    const auto before = filter.first_coefficients();
    auto invalid = low;
    invalid.frequency_hz = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(filter.configure(48000.0, invalid, high));
    CHECK(filter.first_coefficients().b0 == before.b0);
    invalid = low;
    invalid.q = 20.01;
    CHECK_FALSE(filter.configure(48000.0, invalid, high));
    invalid = low;
    invalid.frequency_hz = 21600.01;
    CHECK_FALSE(filter.configure(48000.0, invalid, high));
    invalid = low;
    invalid.type = static_cast<MorphFilterType>(99);
    CHECK_FALSE(filter.configure(48000.0, invalid, high));
    CHECK_FALSE(filter.configure(std::numeric_limits<double>::infinity(), low, high));
    REQUIRE(filter.set_morph(0.4));
    CHECK_FALSE(filter.set_morph(-0.01));
    CHECK_FALSE(filter.set_morph(std::numeric_limits<double>::quiet_NaN()));
    CHECK(filter.morph() == 0.4);
}

TEST_CASE("float filter morph retains low-frequency coefficients at the maximum sample rate",
          "[signal][filter-morph]") {
    pulp::signal::FilterMorph filter;
    const pulp::signal::FilterMorph::Endpoint low_corner{MorphFilterType::lowpass, 20.0f, 0.707f};
    const pulp::signal::FilterMorph::Endpoint high_corner{MorphFilterType::highpass, 20.0f, 0.707f};
    REQUIRE(filter.configure(384000.0f, low_corner, high_corner));
    CHECK(filter.first_coefficients().b0 > 0.0f);
    REQUIRE(filter.set_morph(0.5f));
    CHECK(std::isfinite(filter.process(std::numeric_limits<float>::max())));
    CHECK_FALSE(filter.configure(384001.0f, low_corner, high_corner));
}

TEST_CASE("near-limit endpoint blending remains finite at a tiny nonzero morph",
          "[signal][filter-morph]") {
    pulp::signal::FilterMorph filter;
    const pulp::signal::FilterMorph::Endpoint near_unity{MorphFilterType::highpass, 20.0f, 20.0f};
    REQUIRE(filter.configure(384000.0f, near_unity, near_unity));
    REQUIRE(filter.set_morph(std::numeric_limits<float>::epsilon()));
    const float output = filter.process(std::numeric_limits<float>::max());
    CHECK(std::isfinite(output));
    CHECK(output > std::numeric_limits<float>::max() * 0.99f);
    CHECK(std::isfinite(filter.process(0.25f)));
    CHECK(filter.process(0.25f) != 0.0f);
}

TEST_CASE("filter morph has exact endpoint renders and an independent time oracle",
          "[signal][filter-morph]") {
    for (const double amount : {0.0, 1.0, 0.37}) {
        FilterMorph64 filter;
        REQUIRE(filter.configure(48000.0, low, high));
        REQUIRE(filter.set_morph(amount));
        ReferenceBiquad first(independent_design(low.type, low.frequency_hz, low.q, 48000.0));
        ReferenceBiquad second(independent_design(high.type, high.frequency_hz, high.q, 48000.0));
        for (int i = 0; i < 512; ++i) {
            const double input = i == 0 ? 1.0 : 0.13 * std::sin(0.071 * i);
            const double expected_first = first.process(input);
            const double expected_second = second.process(input);
            const double expected =
                amount == 0.0   ? expected_first
                : amount == 1.0 ? expected_second
                                : expected_first + amount * (expected_second - expected_first);
            CHECK_THAT(filter.process(input), WithinAbs(expected, 2.0e-14));
        }
    }
}

TEST_CASE("filter morph response matches an independent complex oracle and rejects a planted swap",
          "[signal][filter-morph]") {
    FilterMorph64 filter;
    REQUIRE(filter.configure(48000.0, low, high));
    REQUIRE(filter.set_morph(0.31));
    const auto c0 = independent_design(low.type, low.frequency_hz, low.q, 48000.0);
    const auto c1 = independent_design(high.type, high.frequency_hz, high.q, 48000.0);
    bool planted_swap_rejected = false;
    for (double frequency : {20.0, 100.0, 900.0, 2200.0, 8000.0, 18000.0}) {
        const auto h0 = independent_response(c0, frequency, 48000.0);
        const auto h1 = independent_response(c1, frequency, 48000.0);
        const double expected = std::abs(h0 + 0.31 * (h1 - h0));
        CHECK_THAT(filter.magnitude(frequency), WithinAbs(expected, 3.0e-14));
        const double deliberately_swapped = std::abs(h1 + 0.31 * (h0 - h1));
        planted_swap_rejected |= std::abs(expected - deliberately_swapped) > 0.02;
    }
    CHECK(planted_swap_rejected);
}

TEST_CASE("filter morph block in-place and partition renders are deterministic",
          "[signal][filter-morph]") {
    std::array<double, 257> input{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = std::sin(0.017 * static_cast<double>(i)) + (i == 0 ? 1.0 : 0.0);
    auto one_block = input;
    auto partitioned = input;
    FilterMorph64 a, b;
    REQUIRE(a.configure(48000.0, low, high));
    REQUIRE(b.configure(48000.0, low, high));
    REQUIRE(a.set_morph(0.63));
    REQUIRE(b.set_morph(0.63));
    REQUIRE(a.process_block(one_block.data(), one_block.size()));
    REQUIRE(b.process_block(partitioned.data(), 17));
    REQUIRE(b.process_block(partitioned.data() + 17, 100));
    REQUIRE(b.process_block(partitioned.data() + 117, partitioned.size() - 117));
    CHECK(one_block == partitioned);
    CHECK_FALSE(a.process_block(nullptr, 1));
    CHECK(a.process_block(nullptr, 0));
}

TEST_CASE("filter morph reset and nonfinite recovery restore a clean deterministic state",
          "[signal][filter-morph]") {
    FilterMorph64 filter, clean;
    REQUIRE(filter.configure(48000.0, low, high));
    REQUIRE(clean.configure(48000.0, low, high));
    REQUIRE(filter.set_morph(0.5));
    REQUIRE(clean.set_morph(0.5));
    (void)filter.process(1.0);
    CHECK(filter.process(std::numeric_limits<double>::infinity()) == 0.0);
    for (int i = 0; i < 64; ++i)
        CHECK(filter.process(i == 0 ? 1.0 : 0.0) == clean.process(i == 0 ? 1.0 : 0.0));
    filter.reset();
    clean.reset();
    CHECK(filter.process(1.0) == clean.process(1.0));
    CHECK(FilterMorph64::latency_samples() == 0);
    CHECK(FilterMorph64::tail_samples() == -1);
}

TEST_CASE("filter morph realtime paths allocate no memory", "[signal][filter-morph][rt]") {
    FilterMorph64 filter;
    REQUIRE(filter.configure(48000.0, low, high));
    std::array<double, 64> block{};
    pulp::test::RtAllocationProbe probe;
    REQUIRE(filter.set_morph(0.25));
    REQUIRE(filter.process_block(block.data(), block.size()));
    REQUIRE(filter.configure(48000.0, high, low));
    filter.reset();
    CHECK_FALSE(probe.saw_allocation());
}
