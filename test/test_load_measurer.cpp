#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/load_measurer.hpp>
#include <thread>
#include <chrono>
#include <cmath>
#include <limits>

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

    const float first = measure_load(m, std::chrono::microseconds(1200));

    float second = 0.0f;
    for (int attempt = 0; attempt < 8; ++attempt) {
        second = measure_load(m, std::chrono::microseconds(6000));
        if (second > first * 1.25f) {
            break;
        }
    }

    REQUIRE(std::isfinite(first));
    REQUIRE(std::isfinite(second));
    INFO("first=" << first << " second=" << second << " peak=" << m.peak_load());
    REQUIRE(second > first * 1.25f);
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

TEST_CASE("AudioProcessLoadMeasurer clamps smoothing and ignores zero available time",
          "[audio][load][issue-640]") {
    AudioProcessLoadMeasurer m;

    m.end();
    REQUIRE_THAT(m.load(), WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(m.peak_load(), WithinAbs(0.0f, 0.001f));

    m.set_smoothing(-1.0f);
    m.begin(512, 44100.0f);
    busy_wait_for(std::chrono::microseconds(1200));
    m.end();
    REQUIRE_THAT(m.load(), WithinAbs(0.0f, 0.001f));
    REQUIRE(m.peak_load() > 0.0f);

    m.reset();
    m.set_smoothing(2.0f);
    auto raw_load = measure_load(m, std::chrono::microseconds(1200));
    REQUIRE(raw_load > 0.0f);
    REQUIRE_THAT(m.peak_load(), WithinAbs(raw_load, 0.001f));
}

TEST_CASE("AudioProcessLoadMeasurer zero smoothing still tracks peak",
          "[audio][load][issue-640]") {
    AudioProcessLoadMeasurer m;
    m.set_smoothing(0.0f);

    m.begin(512, 44100.0f);
    busy_wait_for(std::chrono::microseconds(1200));
    m.end();

    REQUIRE_THAT(m.load(), WithinAbs(0.0f, 0.001f));
    REQUIRE(m.peak_load() > 0.0f);
}

TEST_CASE("AudioProcessLoadMeasurer reset keeps smoothing configuration",
          "[audio][load][issue-640]") {
    AudioProcessLoadMeasurer m;
    m.set_smoothing(0.0f);

    m.reset();
    m.begin(512, 44100.0f);
    busy_wait_for(std::chrono::microseconds(1200));
    m.end();

    REQUIRE_THAT(m.load(), WithinAbs(0.0f, 0.001f));
    REQUIRE(m.peak_load() > 0.0f);
}

TEST_CASE("AudioProcessLoadMeasurer invalid timing leaves prior load unchanged",
          "[audio][load][issue-640]") {
    AudioProcessLoadMeasurer m;
    m.set_smoothing(1.0f);

    const float previous = measure_load(m, std::chrono::microseconds(1200));
    REQUIRE(previous > 0.0f);
    const float previous_peak = m.peak_load();

    m.begin(512, 0.0f);
    busy_wait_for(std::chrono::microseconds(1200));
    m.end();

    REQUIRE_THAT(m.load(), WithinAbs(previous, 0.001f));
    REQUIRE_THAT(m.peak_load(), WithinAbs(previous_peak, 0.001f));
}

TEST_CASE("AudioProcessLoadMeasurer ignores invalid begin timing inputs", "[audio][load][issue-640]") {
    AudioProcessLoadMeasurer m;
    m.set_smoothing(1.0f);

    struct InvalidTimingInput {
        int num_frames;
        float sample_rate;
    };

    const InvalidTimingInput inputs[] = {
        {0, 44100.0f},
        {-128, 44100.0f},
        {512, 0.0f},
        {512, -44100.0f},
        {512, std::numeric_limits<float>::quiet_NaN()},
        {512, std::numeric_limits<float>::infinity()},
    };

    for (const auto& input : inputs) {
        INFO("num_frames=" << input.num_frames << " sample_rate=" << input.sample_rate);
        m.begin(input.num_frames, input.sample_rate);
        m.end();

        REQUIRE_THAT(m.load(), WithinAbs(0.0f, 0.001f));
        REQUIRE_THAT(m.peak_load(), WithinAbs(0.0f, 0.001f));
    }
}

TEST_CASE("AudioProcessLoadMeasurer rejects overflow-sized available time", "[audio][load][issue-640]") {
    AudioProcessLoadMeasurer m;
    m.set_smoothing(1.0f);

    m.begin(std::numeric_limits<int>::max(), std::numeric_limits<float>::min());
    m.end();

    REQUIRE_THAT(m.load(), WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(m.peak_load(), WithinAbs(0.0f, 0.001f));
}

TEST_CASE("AudioProcessLoadMeasurer accepts valid available time", "[audio][load][issue-640]") {
    AudioProcessLoadMeasurer m;
    m.set_smoothing(1.0f);

    m.begin(480, 48000.0f);
    busy_wait_for(std::chrono::microseconds(1200));
    m.end();

    REQUIRE(m.load() > 0.0f);
    REQUIRE(std::isfinite(m.load()));
    REQUIRE_THAT(m.peak_load(), WithinAbs(m.load(), 0.001f));
}

TEST_CASE("AudioProcessLoadMeasurer reset clears pending timing state", "[audio][load][issue-640]") {
    AudioProcessLoadMeasurer m;
    m.set_smoothing(1.0f);

    m.begin(512, 44100.0f);
    busy_wait_for(std::chrono::microseconds(1200));

    m.reset();
    m.end();

    REQUIRE_THAT(m.load(), WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(m.peak_load(), WithinAbs(0.0f, 0.001f));
}
