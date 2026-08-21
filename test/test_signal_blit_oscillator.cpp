#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/blit_oscillator.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::BlitOscillator;
using pulp::signal::BlitOscillator64;

namespace {

double direct_sum(int harmonics, double phase) {
    double sum = 1.0;
    for (int harmonic = 1; harmonic <= harmonics; ++harmonic)
        sum += 2.0 * std::cos(2.0 * std::numbers::pi * harmonic * phase);
    return sum / static_cast<double>(2 * harmonics + 1);
}

std::vector<double> render(std::size_t block_size, double frequency, std::size_t sample_count) {
    BlitOscillator64 oscillator;
    REQUIRE(oscillator.prepare(48000.0));
    REQUIRE(oscillator.set_frequency(frequency));
    std::vector<double> output(sample_count);
    for (std::size_t begin = 0; begin < sample_count; begin += block_size) {
        const std::size_t end = std::min(sample_count, begin + block_size);
        for (std::size_t i = begin; i < end; ++i)
            output[i] = oscillator.next();
    }
    return output;
}

std::complex<double> dft_bin(const std::vector<double>& signal, int bin) {
    std::complex<double> sum{};
    for (std::size_t i = 0; i < signal.size(); ++i) {
        const double angle = -2.0 * std::numbers::pi * static_cast<double>(bin) *
                             static_cast<double>(i) / static_cast<double>(signal.size());
        sum += signal[i] * std::polar(1.0, angle);
    }
    return sum / static_cast<double>(signal.size());
}

} // namespace

TEST_CASE("BlitOscillator matches an independent finite cosine sum", "[signal][blit]") {
    BlitOscillator64 oscillator;
    REQUIRE(oscillator.prepare(48000.0));
    REQUIRE(oscillator.set_frequency(3200.0));
    REQUIRE(oscillator.harmonic_count() == 7);

    for (const double phase : {0.0, 0.03125, 0.137, 0.5, 0.999}) {
        REQUIRE(oscillator.reset_phase(phase));
        REQUIRE_THAT(oscillator.next(), WithinAbs(direct_sum(7, phase), 2.0e-13));
    }
}

TEST_CASE("BlitOscillator float matches the discrete-summation oracle", "[signal][blit]") {
    BlitOscillator oscillator;
    REQUIRE(oscillator.prepare(48000.0f));
    REQUIRE(oscillator.set_frequency(3200.0f));
    REQUIRE(oscillator.reset_phase(0.137f));
    REQUIRE_THAT(oscillator.next(), WithinAbs(direct_sum(7, 0.137f), 2.0e-5));
}

TEST_CASE("BlitOscillator float evaluates large-count impulse neighborhoods",
          "[signal][blit][regression]") {
    BlitOscillator oscillator;
    REQUIRE(oscillator.prepare(48000.0f));
    REQUIRE(oscillator.set_frequency(0.01f));
    REQUIRE(oscillator.reset_phase());
    REQUIRE(oscillator.next() == 1.0f);
    REQUIRE(std::fabs(oscillator.next()) < 1.0e-5f);
}

TEST_CASE("BlitOscillator float phase advances at accepted sub-Hz frequencies",
          "[signal][blit][regression]") {
    BlitOscillator oscillator;
    REQUIRE(oscillator.prepare(48000.0f));
    REQUIRE(oscillator.set_frequency(0.001f));
    REQUIRE(oscillator.reset_phase(0.5f));
    (void)oscillator.next();
    const double first = oscillator.phase();
    (void)oscillator.next();
    REQUIRE(first > 0.5);
    REQUIRE(oscillator.phase() > first);
}

TEST_CASE("BlitOscillator evaluates the removable impulse limit", "[signal][blit]") {
    BlitOscillator64 oscillator;
    REQUIRE(oscillator.set_frequency(100.0));
    REQUIRE(oscillator.reset_phase(0.0));
    REQUIRE(oscillator.next() == 1.0);
    REQUIRE(oscillator.reset_phase(std::nextafter(1.0, 0.0)));
    REQUIRE_THAT(oscillator.next(), WithinAbs(1.0, 1.0e-10));
}

TEST_CASE("BlitOscillator harmonic count follows frequency and never exceeds Nyquist",
          "[signal][blit]") {
    BlitOscillator64 oscillator;
    REQUIRE(oscillator.prepare(48000.0));
    for (const double frequency : {60.0, 440.0, 937.5, 4000.0, 12000.0}) {
        REQUIRE(oscillator.set_frequency(frequency));
        const int expected = static_cast<int>(std::floor(24000.0 / frequency));
        REQUIRE(oscillator.harmonic_count() == expected);
        REQUIRE(static_cast<double>(expected) * frequency <= 24000.0);
        REQUIRE(static_cast<double>(expected + 1) * frequency > 24000.0);
    }
}

TEST_CASE("BlitOscillator excludes a rounded quotient above Nyquist",
          "[signal][blit][regression]") {
    BlitOscillator64 oscillator;
    REQUIRE(oscillator.prepare(48000.0));
    constexpr double boundary = 24000.0 / 35.0;

    const double above = std::nextafter(boundary, std::numeric_limits<double>::infinity());
    REQUIRE(oscillator.set_frequency(above));
    REQUIRE(oscillator.harmonic_count() == 34);
    REQUIRE(static_cast<long double>(oscillator.harmonic_count()) * above <= 24000.0L);

    const double below = std::nextafter(boundary, 0.0);
    REQUIRE(oscillator.set_frequency(below));
    REQUIRE(oscillator.harmonic_count() == 35);
    REQUIRE(static_cast<long double>(oscillator.harmonic_count()) * below <= 24000.0L);
}

TEST_CASE("BlitOscillator atomically reaches valid pairs outside its defaults",
          "[signal][blit][regression]") {
    BlitOscillator64 oscillator;
    REQUIRE(oscillator.prepare(1.0, 1.0e-9));
    REQUIRE(oscillator.sample_rate() == 1.0);
    REQUIRE(oscillator.frequency() == 1.0e-9);
    const int expected =
        static_cast<int>(std::floor(0.5L / static_cast<long double>(oscillator.frequency())));
    REQUIRE(oscillator.harmonic_count() == expected);
    REQUIRE(static_cast<long double>(expected) * oscillator.frequency() <= 0.5L);
    REQUIRE(std::isfinite(oscillator.next()));
}

TEST_CASE("BlitOscillator spectrum contains no partial above its Nyquist cap",
          "[signal][blit][spectrum]") {
    constexpr std::size_t length = 256;
    constexpr int fundamental_bin = 5;
    constexpr double frequency = 48000.0 * fundamental_bin / length;
    const auto signal = render(length, frequency, length);
    constexpr int harmonics = 25;
    const double expected = 1.0 / static_cast<double>(2 * harmonics + 1);

    REQUIRE_THAT(std::abs(dft_bin(signal, 0)), WithinAbs(expected, 2.0e-13));
    for (int harmonic = 1; harmonic <= harmonics; ++harmonic)
        REQUIRE_THAT(std::abs(dft_bin(signal, harmonic * fundamental_bin)),
                     WithinAbs(expected, 3.0e-13));
    for (int bin = harmonics * fundamental_bin + 1; bin <= 128; ++bin)
        REQUIRE(std::abs(dft_bin(signal, bin)) < 4.0e-13);
}

TEST_CASE("Removing the Nyquist cap measurably aliases", "[signal][blit][negative-control]") {
    constexpr int length = 256;
    constexpr int fundamental_bin = 5;
    constexpr int capped_harmonics = 25;
    std::vector<double> capped(static_cast<std::size_t>(length));
    std::vector<double> uncapped(static_cast<std::size_t>(length));
    double squared_error = 0.0;
    for (int i = 0; i < length; ++i) {
        const double phase = static_cast<double>(fundamental_bin * i) / length;
        capped[static_cast<std::size_t>(i)] = direct_sum(capped_harmonics, phase);
        uncapped[static_cast<std::size_t>(i)] = direct_sum(capped_harmonics + 4, phase);
        const double error =
            uncapped[static_cast<std::size_t>(i)] - capped[static_cast<std::size_t>(i)];
        squared_error += error * error;
    }
    REQUIRE(std::sqrt(squared_error / length) > 0.05);
    REQUIRE(std::abs(dft_bin(uncapped, 111)) > std::abs(dft_bin(capped, 111)) + 0.01);
}

TEST_CASE("BlitOscillator is deterministic across irregular block partitions", "[signal][blit]") {
    const auto reference = render(1, 997.0, 4096);
    for (const std::size_t block : {std::size_t{7}, std::size_t{64}, std::size_t{257}})
        REQUIRE(render(block, 997.0, 4096) == reference);
}

TEST_CASE("BlitOscillator rejects invalid controls transactionally", "[signal][blit]") {
    BlitOscillator64 oscillator;
    REQUIRE(oscillator.prepare(96000.0));
    REQUIRE(oscillator.set_frequency(1000.0));
    REQUIRE_FALSE(oscillator.prepare(1000.0));
    REQUIRE(oscillator.sample_rate() == 96000.0);
    for (const double bad : {0.0, -1.0, 48000.0, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()})
        REQUIRE_FALSE(oscillator.set_frequency(bad));
    REQUIRE(oscillator.frequency() == 1000.0);
    REQUIRE_FALSE(oscillator.reset_phase(-0.1));
    REQUIRE_FALSE(oscillator.reset_phase(1.0));
    REQUIRE_FALSE(oscillator.set_frequency(1.0e-9));
    REQUIRE(oscillator.frequency() == 1000.0);
}

TEST_CASE("BlitOscillator audio-thread operations allocate no memory", "[signal][blit][rt]") {
    BlitOscillator64 oscillator;
    REQUIRE(oscillator.prepare(48000.0));
    REQUIRE(oscillator.set_frequency(997.0));
    double checksum = 0.0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 4096; ++i) {
            checksum += oscillator.next();
            if (i == 2048) {
                REQUIRE(oscillator.set_frequency(1234.0));
                REQUIRE(oscillator.reset_phase(0.125));
            }
        }
        CHECK_FALSE(probe.saw_allocation());
    }
    CHECK(std::isfinite(checksum));
}
