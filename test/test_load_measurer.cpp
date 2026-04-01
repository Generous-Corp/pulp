#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/load_measurer.hpp>
#include <thread>
#include <chrono>
#include <cmath>

using namespace pulp::audio;
using Catch::Matchers::WithinAbs;

template <typename Rep, typename Period>
static void busy_wait_for(std::chrono::duration<Rep, Period> duration) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < duration) {
    }
}

template <typename Rep, typename Period>
static float measure_load(AudioProcessLoadMeasurer& m, std::chrono::duration<Rep, Period> duration) {
    m.begin(512, 44100.0f);
    busy_wait_for(duration);
    m.end();
    return m.load();
}

TEST_CASE("AudioProcessLoadMeasurer initial state is zero", "[audio][load]") {
    AudioProcessLoadMeasurer m;
    REQUIRE_THAT(m.load(), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(m.peak_load(), WithinAbs(0.0, 0.001));
}

TEST_CASE("AudioProcessLoadMeasurer measures nonzero load", "[audio][load]") {
    AudioProcessLoadMeasurer m;
    m.set_smoothing(1.0f); // no averaging, raw measurement

    m.begin(512, 44100.0f);
    // Use busy-wait instead of sleep_for so the test is not hostage to the
    // scheduler's timer granularity on Windows.
    busy_wait_for(std::chrono::microseconds(1200));
    m.end();

    REQUIRE(m.load() > 0.01f);
    REQUIRE(std::isfinite(m.load()));
}

TEST_CASE("AudioProcessLoadMeasurer peak tracking", "[audio][load]") {
    AudioProcessLoadMeasurer m;
    m.set_smoothing(1.0f);

    const float first = measure_load(m, std::chrono::microseconds(600));

    float second = 0.0f;
    for (int attempt = 0; attempt < 5; ++attempt) {
        second = measure_load(m, std::chrono::microseconds(2200));
        if (second > first) {
            break;
        }
    }

    REQUIRE(std::isfinite(first));
    REQUIRE(std::isfinite(second));
    INFO("first=" << first << " second=" << second << " peak=" << m.peak_load());
    REQUIRE(second > first);
    REQUIRE(m.peak_load() >= m.load());
}

TEST_CASE("AudioProcessLoadMeasurer reset clears state", "[audio][load]") {
    AudioProcessLoadMeasurer m;
    m.set_smoothing(1.0f);

    m.begin(512, 44100.0f);
    busy_wait_for(std::chrono::microseconds(1200));
    m.end();
    REQUIRE(m.load() > 0);

    m.reset();
    REQUIRE_THAT(m.load(), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(m.peak_load(), WithinAbs(0.0, 0.001));
}

TEST_CASE("AudioProcessLoadMeasurer reset_peak", "[audio][load]") {
    AudioProcessLoadMeasurer m;
    m.set_smoothing(1.0f);

    m.begin(512, 44100.0f);
    busy_wait_for(std::chrono::microseconds(1200));
    m.end();

    float peak = m.peak_load();
    REQUIRE(peak > 0);

    m.reset_peak();
    REQUIRE_THAT(m.peak_load(), WithinAbs(0.0, 0.001));
    // load() should still be nonzero
    REQUIRE(m.load() > 0);
}
