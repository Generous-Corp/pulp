#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/load_measurer.hpp>
#include <thread>
#include <chrono>

using namespace pulp::audio;
using Catch::Matchers::WithinAbs;

TEST_CASE("AudioProcessLoadMeasurer initial state is zero", "[audio][load]") {
    AudioProcessLoadMeasurer m;
    REQUIRE_THAT(m.load(), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(m.peak_load(), WithinAbs(0.0, 0.001));
}

TEST_CASE("AudioProcessLoadMeasurer measures nonzero load", "[audio][load]") {
    AudioProcessLoadMeasurer m;
    m.set_smoothing(1.0f); // no averaging, raw measurement

    m.begin(512, 44100.0f);
    // Simulate some work (~1ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    m.end();

    // 512 samples at 44100 Hz = ~11.6ms available
    // 1ms work ≈ 8.6% load
    REQUIRE(m.load() > 0.01f);
    REQUIRE(m.load() < 1.0f);
}

TEST_CASE("AudioProcessLoadMeasurer peak tracking", "[audio][load]") {
    AudioProcessLoadMeasurer m;
    m.set_smoothing(1.0f);

    // First measurement
    m.begin(512, 44100.0f);
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    m.end();
    float first = m.load();

    // Second measurement with more work
    m.begin(512, 44100.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    m.end();

    REQUIRE(m.peak_load() >= m.load());
}

TEST_CASE("AudioProcessLoadMeasurer reset clears state", "[audio][load]") {
    AudioProcessLoadMeasurer m;
    m.set_smoothing(1.0f);

    m.begin(512, 44100.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    m.end();

    float peak = m.peak_load();
    REQUIRE(peak > 0);

    m.reset_peak();
    REQUIRE_THAT(m.peak_load(), WithinAbs(0.0, 0.001));
    // load() should still be nonzero
    REQUIRE(m.load() > 0);
}
