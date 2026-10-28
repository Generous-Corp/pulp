#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/load_measurer.hpp>
#include "harness/rt_allocation_probe.hpp"
#include <algorithm>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

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

TEST_CASE("AudioProcessLoadMeasurer reports spans in nanoseconds of wall time", "[audio][load]") {
    // The measurer reads its own monotonic clock (mach_absolute_time on Apple
    // targets). Its span must match steady_clock's for the same wait, which
    // pins the tick-to-nanosecond conversion end to end.
    AudioProcessLoadMeasurer m;
    for (int attempt = 0; attempt < 5; ++attempt) {
        const auto outer_start = std::chrono::steady_clock::now();
        m.begin(512, 44100.0f);
        busy_wait_for(std::chrono::milliseconds(3));
        m.end();
        const auto outer_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - outer_start)
                                  .count();
        const auto measured = m.snapshot().elapsed_ns;
        INFO("measured " << measured << " ns, steady_clock " << outer_ns << " ns");
        // At least the wait itself, and no more than the enclosing span.
        REQUIRE(measured >= 3'000'000 * 95 / 100);
        REQUIRE(measured <= outer_ns + 50'000);
    }
}

TEST_CASE("LoadClockRatio converts tick spans to nanoseconds", "[audio][load][clock]") {
    // 24 MHz (M1's counter) against 1 GHz reduces to 125/3, and 1 GHz to 1/1.
    REQUIRE(LoadClockRatio::reduced(1'000'000'000, 24'000'000) == LoadClockRatio{125, 3});
    REQUIRE(LoadClockRatio::reduced(125, 3) == LoadClockRatio{125, 3});
    REQUIRE(LoadClockRatio::reduced(7, 7) == LoadClockRatio{1, 1});

    const auto m1 = LoadClockRatio::reduced(125, 3);
    REQUIRE(m1.to_ns(24'000'000) == 1'000'000'000); // one second of 24 MHz ticks
    REQUIRE(m1.to_ns(3) == 125);
    REQUIRE(m1.to_ns(1) == 41); // truncates, never rounds up
    REQUIRE(LoadClockRatio{1, 1}.to_ns(123'456'789) == 123'456'789);

    // A zero rate is an unavailable clock: every span is 0, never a division
    // by zero.
    REQUIRE(LoadClockRatio::reduced(0, 3).to_ns(1'000) == 0);
    REQUIRE(LoadClockRatio::reduced(125, 0).to_ns(1'000) == 0);

    // A span too long for the multiply saturates instead of wrapping.
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    REQUIRE(m1.to_ns(std::numeric_limits<std::uint64_t>::max()) == kMax);
    REQUIRE(LoadClockRatio{1, 1}.to_ns(std::numeric_limits<std::uint64_t>::max()) == kMax);

    // The host ratio converts the measurer's own ticks to wall time: a
    // steady_clock-timed wait reads the same in both.
    const auto ratio = LoadClockRatio::host();
    REQUIRE(ratio.numer > 0);
    REQUIRE(ratio.denom > 0);
    for (int attempt = 0; attempt < 5; ++attempt) {
        const auto outer_start = std::chrono::steady_clock::now();
        const auto start = AudioProcessLoadMeasurer::read_ticks();
        busy_wait_for(std::chrono::milliseconds(2));
        const auto end = AudioProcessLoadMeasurer::read_ticks();
        const auto outer_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - outer_start)
                                  .count();
        const auto span_ns = ratio.to_ns(end - start);
        INFO("span " << span_ns << " ns, steady_clock " << outer_ns << " ns");
        REQUIRE(span_ns >= 2'000'000 * 95 / 100);
        REQUIRE(span_ns <= outer_ns + 50'000);
    }
}

TEST_CASE("AudioProcessLoadMeasurer converts spans with the ratio captured at construction",
          "[audio][load][clock]") {
    // begin() and end() must not query the platform timebase (a guarded
    // function-local static or a kernel call on the audio thread). The ratio
    // is read once by the constructor, so an injected ratio is the one every
    // span uses.
    const auto host = LoadClockRatio::host();
    AudioProcessLoadMeasurer native;
    REQUIRE(native.clock_ratio() == host);

    AudioProcessLoadMeasurer doubled(LoadClockRatio{host.numer * 2, host.denom});
    REQUIRE(doubled.clock_ratio() == LoadClockRatio{host.numer * 2, host.denom});
    AudioProcessLoadMeasurer unavailable(LoadClockRatio::reduced(0, 1));

    // Nest the spans: doubled encloses native, so its raw ticks are at least
    // native's and its nanoseconds at least twice native's.
    doubled.begin(512, 44100.0f);
    unavailable.begin(512, 44100.0f);
    native.begin(512, 44100.0f);
    busy_wait_for(std::chrono::milliseconds(2));
    native.end();
    unavailable.end();
    doubled.end();

    const auto native_ns = native.snapshot().elapsed_ns;
    const auto doubled_ns = doubled.snapshot().elapsed_ns;
    INFO("native " << native_ns << " ns, doubled " << doubled_ns << " ns");
    REQUIRE(native_ns >= 2'000'000 * 95 / 100);
    REQUIRE(doubled_ns >= 2 * native_ns);
    REQUIRE(unavailable.snapshot().elapsed_ns == 0);
}

TEST_CASE("AudioProcessLoadMeasurer clock reads are ordered and monotonic",
          "[audio][load][clock]") {
    // Each read is ordered against the work around it, so successive reads
    // never go backwards and a span always covers the work it brackets.
    auto previous = AudioProcessLoadMeasurer::read_ticks();
    for (int i = 0; i < 1'000'000; ++i) {
        const auto now = AudioProcessLoadMeasurer::read_ticks();
        if (now < previous) {
            FAIL("read_ticks went backwards at read " << i << ": " << previous << " then "
                                                      << now);
        }
        previous = now;
    }

    // A span that brackets a short, data-dependent loop is never negative and
    // never shorter than steady_clock's reading of the same loop by more than
    // a tick of either clock.
    const auto ratio = LoadClockRatio::host();
    volatile std::uint64_t sink = 0;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const auto start = AudioProcessLoadMeasurer::read_ticks();
        const auto inner_start = std::chrono::steady_clock::now();
        std::uint64_t acc = static_cast<std::uint64_t>(attempt);
        for (int k = 0; k < 2000; ++k)
            acc = acc * 6364136223846793005ull + 1442695040888963407ull;
        sink = acc;
        const auto inner_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - inner_start)
                                  .count();
        const auto end = AudioProcessLoadMeasurer::read_ticks();
        REQUIRE(end >= start);
        const auto span_ns = ratio.to_ns(end - start);
        if (span_ns + 1'000 < inner_ns) {
            FAIL("span " << span_ns << " ns is shorter than the bracketed " << inner_ns
                         << " ns");
        }
    }
    static_cast<void>(sink);
}

TEST_CASE("AudioProcessLoadMeasurer clock read cost against steady_clock",
          "[.][audio][load][clock][benchmark]") {
    // Advisory: reports the per-read cost of the measurer's clock and of
    // steady_clock on this host. Perf assertions flake on shared runners, so
    // this is hidden and asserts nothing about speed.
    constexpr int kReads = 2'000'000;
    auto best_ns_per_read = [](auto&& read) {
        double best = std::numeric_limits<double>::infinity();
        for (int run = 0; run < 9; ++run) {
            std::uint64_t acc = 0;
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < kReads; ++i)
                acc += read();
            const auto ns = std::chrono::duration<double, std::nano>(
                                std::chrono::steady_clock::now() - start)
                                .count();
            REQUIRE(acc != 0);
            best = std::min(best, ns / kReads);
        }
        return best;
    };
    const double measurer = best_ns_per_read([] { return AudioProcessLoadMeasurer::read_ticks(); });
    const double steady = best_ns_per_read([] {
        return static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    });
    WARN("measurer clock " << measurer << " ns/read, steady_clock " << steady << " ns/read");
    SUCCEED();
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
    REQUIRE(m.peak_load() + 0.000001f >= m.load());
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

TEST_CASE("AudioProcessLoadMeasurer publishes bounded overload telemetry",
          "[audio][load][telemetry]") {
    AudioProcessLoadMeasurer m;
    m.set_smoothing(1.0f);
    m.set_overload_threshold(0.05f);

    m.begin(512, 48000.0f);
    busy_wait_for(std::chrono::microseconds(1200));
    m.end();

    const auto first = m.snapshot();
    REQUIRE(first.callback_count == 1);
    REQUIRE(first.overload_count == 1);
    REQUIRE(first.elapsed_ns > 0);
    REQUIRE(first.available_ns > 0);
    REQUIRE(first.last_load > 0.05f);
    REQUIRE_THAT(first.load, WithinAbs(first.last_load, 0.001f));
    REQUIRE_THAT(first.peak_load, WithinAbs(first.last_load, 0.001f));

    m.set_overload_threshold(100.0f);
    m.begin(512, 48000.0f);
    busy_wait_for(std::chrono::microseconds(100));
    m.end();

    const auto second = m.snapshot();
    REQUIRE(second.callback_count == 2);
    REQUIRE(second.overload_count == 1);
    REQUIRE(second.elapsed_ns > 0);
    REQUIRE(second.available_ns == first.available_ns);

    m.reset();
    const auto reset = m.snapshot();
    REQUIRE(reset.callback_count == 0);
    REQUIRE(reset.overload_count == 0);
    REQUIRE(reset.elapsed_ns == 0);
    REQUIRE(reset.available_ns == 0);
    REQUIRE_THAT(reset.load, WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(reset.peak_load, WithinAbs(0.0f, 0.001f));
}

TEST_CASE("Audio runtime overload policy classifies load severity",
          "[audio][load][overload-policy]") {
    AudioProcessLoadSnapshot snapshot;

    auto report = evaluate_audio_runtime_overload(snapshot, 0);
    REQUIRE(report.severity == AudioRuntimeOverloadSeverity::Nominal);
    REQUIRE(std::string(to_string(report.severity)) == "nominal");
    REQUIRE_FALSE(report.should_shed_optional_work);
    REQUIRE_FALSE(report.validation_failure);
    REQUIRE(std::string(report.action) == "normal");

    snapshot.load = 0.80f;
    report = evaluate_audio_runtime_overload(snapshot, 0);
    REQUIRE(report.severity == AudioRuntimeOverloadSeverity::Watch);
    REQUIRE(std::string(to_string(report.severity)) == "watch");
    REQUIRE_FALSE(report.should_shed_optional_work);
    REQUIRE(std::string(report.action) == "monitor");

    snapshot.last_load = 1.05f;
    report = evaluate_audio_runtime_overload(snapshot, 0);
    REQUIRE(report.severity == AudioRuntimeOverloadSeverity::Overloaded);
    REQUIRE(std::string(to_string(report.severity)) == "overloaded");
    REQUIRE(report.should_shed_optional_work);
    REQUIRE_FALSE(report.should_bypass_optional_work);
    REQUIRE_FALSE(report.validation_failure);
    REQUIRE(std::string(report.action) == "shed-optional-work");

    snapshot.peak_load = 1.30f;
    report = evaluate_audio_runtime_overload(snapshot, 0);
    REQUIRE(report.severity == AudioRuntimeOverloadSeverity::Critical);
    REQUIRE(std::string(to_string(report.severity)) == "critical");
    REQUIRE(report.should_shed_optional_work);
    REQUIRE(report.should_bypass_optional_work);
    REQUIRE(report.validation_failure);
    REQUIRE(std::string(report.action) == "bypass-optional-work");
}

TEST_CASE("Audio runtime overload policy escalates xrun and overload counters",
          "[audio][load][overload-policy]") {
    AudioProcessLoadSnapshot snapshot;

    auto report = evaluate_audio_runtime_overload(snapshot, 1);
    REQUIRE(report.severity == AudioRuntimeOverloadSeverity::Overloaded);
    REQUIRE(report.should_shed_optional_work);
    REQUIRE_FALSE(report.validation_failure);

    report = evaluate_audio_runtime_overload(snapshot, 3);
    REQUIRE(report.severity == AudioRuntimeOverloadSeverity::Critical);
    REQUIRE(report.should_bypass_optional_work);
    REQUIRE(report.validation_failure);

    snapshot.overload_count = 1;
    report = evaluate_audio_runtime_overload(snapshot, 0);
    REQUIRE(report.severity == AudioRuntimeOverloadSeverity::Overloaded);

    snapshot.overload_count = 3;
    report = evaluate_audio_runtime_overload(snapshot, 0);
    REQUIRE(report.severity == AudioRuntimeOverloadSeverity::Critical);
}

TEST_CASE("Audio runtime overload policy honors custom validation thresholds",
          "[audio][load][overload-policy]") {
    AudioProcessLoadSnapshot snapshot;
    snapshot.peak_load = 0.70f;

    AudioRuntimeOverloadPolicy policy;
    policy.watch_load = 0.5f;
    policy.overload_load = 0.65f;
    policy.critical_load = 0.9f;
    policy.watch_xruns = 2;
    policy.critical_xruns = 4;
    policy.watch_overloads = 2;
    policy.critical_overloads = 4;

    auto report = evaluate_audio_runtime_overload(snapshot, 0, policy);
    REQUIRE(report.severity == AudioRuntimeOverloadSeverity::Overloaded);
    REQUIRE(report.should_shed_optional_work);

    snapshot.peak_load = 0.95f;
    report = evaluate_audio_runtime_overload(snapshot, 0, policy);
    REQUIRE(report.severity == AudioRuntimeOverloadSeverity::Critical);
    REQUIRE(report.validation_failure);

    snapshot = {};
    report = evaluate_audio_runtime_overload(snapshot, 1, policy);
    REQUIRE(report.severity == AudioRuntimeOverloadSeverity::Nominal);

    report = evaluate_audio_runtime_overload(snapshot, 2, policy);
    REQUIRE(report.severity == AudioRuntimeOverloadSeverity::Overloaded);
}

TEST_CASE("Audio runtime overload policy sanitizes invalid load thresholds",
          "[audio][load][overload-policy]") {
    AudioRuntimeOverloadPolicy policy;
    policy.watch_load = -1.0f;
    policy.overload_load = 0.25f;
    policy.critical_load = 0.5f;

    AudioProcessLoadSnapshot snapshot;
    snapshot.last_load = 0.8f;
    auto report = evaluate_audio_runtime_overload(snapshot, 0, policy);
    REQUIRE(report.severity == AudioRuntimeOverloadSeverity::Watch);

    snapshot.last_load = 1.05f;
    report = evaluate_audio_runtime_overload(snapshot, 0, policy);
    REQUIRE(report.severity == AudioRuntimeOverloadSeverity::Overloaded);

    snapshot.last_load = 1.3f;
    report = evaluate_audio_runtime_overload(snapshot, 0, policy);
    REQUIRE(report.severity == AudioRuntimeOverloadSeverity::Critical);
}

TEST_CASE("AudioProcessLoadMeasurer hot path and snapshot polling allocate zero times",
          "[audio][load][telemetry][rt-safety]") {
    AudioProcessLoadMeasurer m;
    m.set_smoothing(1.0f);
    m.set_overload_threshold(0.000001f);

    pulp::test::RtAllocationProbe probe;

    for (int i = 0; i < 8; ++i) {
        m.begin(128, 48000.0f);
        busy_wait_for(std::chrono::microseconds(50));
        m.end();
        (void)m.load();
        (void)m.peak_load();
        (void)m.last_load();
        (void)m.callback_count();
        (void)m.overload_count();
        (void)m.snapshot();
    }

    REQUIRE_FALSE(probe.saw_allocation());
}
