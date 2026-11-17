#include "harness/paced_deadline_driver.hpp"
#include "harness/paced_timing_cell.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using pulp::test::paced::BasicDeadlineDriver;
using pulp::test::paced::Lateness;
using pulp::test::paced::WakeBackend;
using pulp::test::paced::WakeConfig;

// A scripted clock: every clock read costs `read_cost_ns`, a kernel wait wakes
// `overshoot_ns` after its deadline, and each spin step burns `relax_ns` of
// both wall and thread CPU time. Each kernel call burns `kernel_call_ns` of
// wall and CPU time. `native_skew_ns` models a clock mapping that converts
// every deadline that much early: the wait wakes before the measurement-clock
// deadline, and a wait on a deadline the native clock has passed returns at
// once.
struct ScriptedPlatform {
    std::int64_t now = 1'000'000'000;
    std::int64_t cpu = 0;
    std::int64_t read_cost_ns = 100;
    std::int64_t overshoot_ns = 5'000;
    std::int64_t relax_ns = 200;
    std::int64_t kernel_call_ns = 0;
    std::int64_t native_skew_ns = 0;
    std::int64_t mapping_skew_ns = 0;
    bool fail_kernel_wait = false;
    std::vector<std::int64_t> kernel_deadlines;
    std::vector<std::int64_t> sleeps;

    std::int64_t now_ns() {
        now += read_cost_ns;
        return now;
    }
    bool kernel_wait_until(std::int64_t deadline) {
        kernel_deadlines.push_back(deadline);
        now += kernel_call_ns;
        cpu += kernel_call_ns;
        if (fail_kernel_wait)
            return false;
        const auto native_deadline = deadline - native_skew_ns;
        if (now < native_deadline)
            now = native_deadline + overshoot_ns;
        return true;
    }
    std::int64_t clock_mapping_skew_ns() const {
        return mapping_skew_ns;
    }
    void sleep_until_ns(std::int64_t deadline) {
        sleeps.push_back(deadline);
        if (now < deadline)
            now = deadline + overshoot_ns;
    }
    void sleep_for_ns(std::int64_t duration) {
        now += duration;
    }
    std::int64_t thread_cpu_ns() const {
        return cpu;
    }
    std::int64_t cpu_clock_resolution_ns() const {
        return 1;
    }
    std::string_view kernel_wait_name() const {
        return "scripted";
    }
    std::uint64_t timebase_numer() const {
        return 125;
    }
    std::uint64_t timebase_denom() const {
        return 3;
    }
    void relax() {
        now += relax_ns;
        cpu += relax_ns;
    }
};

using ScriptedDriver = BasicDeadlineDriver<ScriptedPlatform>;

constexpr std::uint64_t kRate = 48000;
constexpr std::uint64_t kFrames = 128;
constexpr std::int64_t kPeriodNs = 2'666'666;

WakeConfig fixed_guard(std::int64_t guard_ns) {
    WakeConfig config;
    config.guard_ns = guard_ns;
    return config;
}

void set_env(const char* key, const char* value) {
#if defined(_WIN32)
    _putenv_s(key, value == nullptr ? "" : value);
#else
    if (value == nullptr)
        unsetenv(key);
    else
        setenv(key, value, 1);
#endif
}

// Restores one environment variable when it goes out of scope.
class ScopedEnv {
  public:
    ScopedEnv(const char* key, const char* value) : key_(key) {
        if (const char* old = std::getenv(key))
            previous_ = old;
        set_env(key, value);
    }
    ~ScopedEnv() {
        set_env(key_, previous_ ? previous_->c_str() : nullptr);
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

  private:
    const char* key_;
    std::optional<std::string> previous_;
};

} // namespace

TEST_CASE("Paced deadline driver waits to a guard then spins to each fixed-origin target",
          "[paced][deadline-driver]") {
    ScriptedPlatform platform;
    ScriptedDriver driver(platform, kRate, kFrames, fixed_guard(50'000));
    CHECK(driver.settings().guard_source == "fixed");
    CHECK(driver.settings().guard_ns == 50'000);
    CHECK(driver.settings().spin_limit_ns == 2 * 50'000 + pulp::test::paced::kGuardMarginNs);
    driver.start();
    const auto origin = driver.origin_ns();
    for (std::size_t i = 0; i < 16; ++i) {
        const auto tick = driver.wait(i);
        INFO("index=" << i);
        CHECK(tick.target_offset_ns ==
              static_cast<std::int64_t>(i * kFrames * 1'000'000'000ull / kRate));
        CHECK(tick.target_ns == origin + tick.target_offset_ns);
        CHECK(platform.kernel_deadlines.back() == tick.target_ns - 50'000);
        CHECK(tick.lateness == Lateness::on_time);
        CHECK(tick.kernel_wake_ns <= tick.target_ns);
        CHECK(tick.start_ns >= tick.target_ns);
        // The spin ends within one spin step and one clock read of the target.
        CHECK(tick.start_ns - tick.target_ns <= platform.relax_ns + platform.read_cost_ns);
        CHECK(tick.spin_cpu_ns > 0);
        CHECK(tick.spin_cpu_ns <= 50'000 + platform.relax_ns);
        CHECK_FALSE(tick.spin_overrun);
    }
    CHECK(driver.origin_ns() == origin);
    CHECK(driver.target_offset_ns(100000) == 266'666'666'666);
}

TEST_CASE("Paced deadline driver keeps the original schedule when a callback is late",
          "[paced][deadline-driver]") {
    ScriptedPlatform platform;
    ScriptedDriver driver(platform, kRate, kFrames, fixed_guard(50'000));
    driver.start();
    const auto origin = driver.origin_ns();
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(driver.wait(i).lateness == Lateness::on_time);
    // The thread loses a block and a half before callback 3.
    platform.now = origin + driver.target_offset_ns(3) + kPeriodNs / 2;
    const auto late = driver.wait(3);
    CHECK(late.lateness == Lateness::entered_late);
    CHECK(late.target_ns == origin + driver.target_offset_ns(3));
    CHECK(late.start_ns - late.target_ns >= kPeriodNs / 2);
    // The next target was not moved to absorb the miss.
    const auto next = driver.wait(4);
    CHECK(next.target_ns == origin + driver.target_offset_ns(4));
    CHECK(next.lateness == Lateness::on_time);
    CHECK(next.start_ns >= next.target_ns);
    CHECK(next.start_ns - next.target_ns <= platform.relax_ns + platform.read_cost_ns);

    // Negative control: a kernel wait that oversleeps the whole guard is
    // reported as an overshoot, not as an on-time wake.
    platform.overshoot_ns = 60'000;
    const auto overshot = driver.wait(5);
    CHECK(overshot.lateness == Lateness::kernel_overshot);
    CHECK(overshot.kernel_wake_ns > overshot.target_ns);
    CHECK(overshot.target_ns == origin + driver.target_offset_ns(5));
}

TEST_CASE("Paced deadline driver injected lateness enters late without moving targets",
          "[paced][deadline-driver]") {
    ScriptedPlatform platform;
    auto config = fixed_guard(50'000);
    config.inject_late_us = 12'000;
    config.inject_late_every = 5;
    ScriptedDriver driver(platform, kRate, kFrames, config);
    CHECK(driver.settings().inject_late_us == 12'000);
    driver.start();
    const auto origin = driver.origin_ns();
    std::size_t late = 0;
    for (std::size_t i = 0; i < 12; ++i) {
        const auto tick = driver.wait(i);
        INFO("index=" << i);
        CHECK(tick.target_ns == origin + driver.target_offset_ns(i));
        CHECK(tick.start_ns >= tick.target_ns);
        if (i == 0 || i == 5 || i == 10)
            CHECK(tick.lateness == Lateness::entered_late);
        late += tick.lateness == Lateness::entered_late ? 1 : 0;
    }
    CHECK(late >= 3);

    // Control: the same schedule without injection is on time throughout.
    ScriptedPlatform quiet;
    ScriptedDriver control(quiet, kRate, kFrames, fixed_guard(50'000));
    control.start();
    for (std::size_t i = 0; i < 12; ++i)
        CHECK(control.wait(i).lateness == Lateness::on_time);
}

TEST_CASE("Paced deadline driver calibrates a bounded guard from kernel overshoot",
          "[paced][deadline-driver]") {
    {
        ScriptedPlatform platform;
        platform.overshoot_ns = 30'000;
        ScriptedDriver driver(platform, kRate, kFrames, WakeConfig{});
        const auto& settings = driver.settings();
        CHECK(settings.guard_source == "calibrated");
        CHECK(settings.calibration_samples == 64);
        CHECK(settings.calibration_p99_ns >= 30'000);
        CHECK(settings.calibration_p99_ns <= 30'000 + 2 * platform.read_cost_ns);
        CHECK(settings.guard_ns == settings.calibration_p99_ns + pulp::test::paced::kGuardMarginNs);
        CHECK(settings.guard_clamped == 0);
        CHECK(settings.guard_bound_ns == kPeriodNs / 4);
        CHECK(settings.timebase_numer == 125);
        CHECK(settings.timebase_denom == 3);
    }
    {
        // An overshoot larger than the bound clamps the guard and says so.
        ScriptedPlatform platform;
        platform.overshoot_ns = 5'000'000;
        ScriptedDriver driver(platform, kRate, kFrames, WakeConfig{});
        CHECK(driver.settings().guard_ns == kPeriodNs / 4);
        CHECK(driver.settings().guard_clamped == 1);
    }
    {
        ScriptedPlatform platform;
        platform.overshoot_ns = 0;
        platform.read_cost_ns = 0;
        ScriptedDriver driver(platform, kRate, kFrames, WakeConfig{});
        CHECK(driver.settings().guard_ns == pulp::test::paced::kMinGuardNs);
        CHECK(driver.settings().guard_clamped == 0);
    }
}

TEST_CASE("Paced deadline driver bounds the spin and still never starts early",
          "[paced][deadline-driver]") {
    ScriptedPlatform platform;
    auto config = fixed_guard(1'000'000);
    config.spin_limit_ns = 100'000;
    ScriptedDriver driver(platform, kRate, kFrames, config);
    CHECK(driver.settings().spin_limit_ns == 100'000);
    driver.start();
    const auto tick = driver.wait(0);
    CHECK(tick.spin_overrun);
    CHECK(tick.start_ns >= tick.target_ns);
    REQUIRE(platform.sleeps.size() == 1);
    CHECK(platform.sleeps.front() == tick.target_ns);
    // The bounded spin burned no more than its limit plus one step.
    CHECK(tick.spin_cpu_ns <= 100'000 + 2 * platform.relax_ns);

    // Control: a guard within the spin limit never overruns.
    ScriptedPlatform quiet;
    auto within = fixed_guard(50'000);
    within.spin_limit_ns = 100'000;
    ScriptedDriver control(quiet, kRate, kFrames, within);
    control.start();
    CHECK_FALSE(control.wait(0).spin_overrun);
    CHECK(quiet.sleeps.empty());
}

TEST_CASE("Paced deadline driver bounds and accounts kernel retries under a skewed clock mapping",
          "[paced][deadline-driver]") {
    // Every kernel deadline lands 1 ms early, so after the first wake each
    // retry returns at once until the measurement clock reaches the guard.
    ScriptedPlatform platform;
    platform.native_skew_ns = 1'000'000;
    platform.mapping_skew_ns = 1'000'000;
    platform.overshoot_ns = 0;
    platform.kernel_call_ns = 400;
    ScriptedDriver driver(platform, kRate, kFrames, fixed_guard(50'000));
    CHECK(driver.settings().clock_mapping_skew_ns == 1'000'000);
    const auto limit = driver.settings().spin_limit_ns;
    const auto retry_cost = platform.kernel_call_ns + platform.read_cost_ns;
    driver.start();
    for (std::size_t i = 0; i < 4; ++i) {
        platform.kernel_deadlines.clear();
        platform.sleeps.clear();
        const auto tick = driver.wait(i);
        INFO("index=" << i);
        CHECK(tick.spin_overrun);
        CHECK(tick.kernel_retries > 0);
        // The retries stop at the spin limit, not at the guard deadline.
        CHECK(tick.kernel_retries <= static_cast<std::uint32_t>(limit / retry_cost + 2));
        CHECK(platform.kernel_deadlines.size() == tick.kernel_retries + 1u);
        // Their CPU cost is recorded as spin cost.
        CHECK(tick.spin_cpu_ns >=
              static_cast<std::int64_t>(tick.kernel_retries) * platform.kernel_call_ns);
        // The overrun falls back to a blocking wait and still never starts early.
        REQUIRE(platform.sleeps.size() == 1);
        CHECK(platform.sleeps.front() == tick.target_ns);
        CHECK(tick.start_ns >= tick.target_ns);
        CHECK(tick.kernel_wake_ns <= tick.start_ns);
    }

    // Control: the same retry cost with an exact mapping issues one wait.
    ScriptedPlatform exact;
    exact.overshoot_ns = 0;
    exact.kernel_call_ns = 400;
    ScriptedDriver control(exact, kRate, kFrames, fixed_guard(50'000));
    CHECK(control.settings().clock_mapping_skew_ns == 0);
    control.start();
    const auto tick = control.wait(0);
    CHECK_FALSE(tick.spin_overrun);
    CHECK(tick.kernel_retries == 0);
    CHECK(exact.kernel_deadlines.size() == 1);
}

TEST_CASE("Paced clock mapping keeps a narrow bracket and flags a preempted one",
          "[paced][deadline-driver]") {
    using pulp::test::paced::bracket_clock_mapping;
    // The first bracket was preempted for 5 ms; the second is 200 ns wide.
    const std::vector<std::int64_t> steady{0, 5'000'000, 6'000'000, 6'000'200};
    std::size_t at = 0;
    std::uint64_t native_reads = 0;
    const auto mapping = bracket_clock_mapping([&] { return steady.at(at++); },
                                               [&] { return ++native_reads * 1000; });
    CHECK(mapping.ok);
    CHECK(mapping.attempts == 2);
    CHECK(mapping.skew_ns == 200);
    CHECK(mapping.steady_ns == 6'000'100);
    CHECK(mapping.native == 2000);

    // Every bracket is preempted: the narrowest is kept and flagged.
    std::int64_t t = 0;
    const auto skewed = bracket_clock_mapping(
        [&] {
            t += 50'000;
            return t;
        },
        [] { return std::uint64_t{7}; });
    CHECK_FALSE(skewed.ok);
    CHECK(skewed.attempts == pulp::test::paced::kClockMappingAttempts);
    CHECK(skewed.skew_ns == 50'000);
    CHECK(skewed.skew_ns > pulp::test::paced::kMaxClockMappingSkewNs);
}

TEST_CASE("Paced deadline driver counts kernel wait failures and still honours the target",
          "[paced][deadline-driver]") {
    ScriptedPlatform platform;
    platform.fail_kernel_wait = true;
    ScriptedDriver driver(platform, kRate, kFrames, WakeConfig{});
    CHECK(driver.settings().wait_failures == 64);
    CHECK(driver.settings().calibration_samples == 0);
    CHECK(driver.settings().guard_ns == driver.settings().guard_bound_ns);
    driver.start();
    const auto tick = driver.wait(0);
    CHECK(driver.settings().wait_failures == 65);
    CHECK(tick.spin_overrun);
    CHECK(tick.start_ns >= tick.target_ns);
}

TEST_CASE("Paced deadline driver sleep-until baseline has no guard and no spin",
          "[paced][deadline-driver]") {
    ScriptedPlatform platform;
    WakeConfig config;
    config.backend = WakeBackend::sleep_until;
    ScriptedDriver driver(platform, kRate, kFrames, config);
    CHECK(driver.settings().kernel_wait == "sleep_until");
    CHECK(driver.settings().guard_source == "none");
    CHECK(driver.settings().guard_ns == 0);
    driver.start();
    const auto tick = driver.wait(1);
    CHECK(platform.kernel_deadlines.empty());
    CHECK(tick.spin_cpu_ns == 0);
    CHECK(tick.start_ns == tick.kernel_wake_ns);
    CHECK(tick.start_ns - tick.target_ns == platform.overshoot_ns + platform.read_cost_ns);
}

TEST_CASE("Paced deadline driver refuses malformed configuration", "[paced][deadline-driver]") {
    ScriptedPlatform platform;
    CHECK_THROWS_AS(ScriptedDriver(platform, 0, kFrames, fixed_guard(1)), std::invalid_argument);
    CHECK_THROWS_AS(ScriptedDriver(platform, kRate, 0, fixed_guard(1)), std::invalid_argument);
    auto every = fixed_guard(1);
    every.inject_late_every = 0;
    CHECK_THROWS_AS(ScriptedDriver(platform, kRate, kFrames, every), std::invalid_argument);
    ScriptedDriver unstarted(platform, kRate, kFrames, fixed_guard(1));
    CHECK_THROWS_AS(unstarted.wait(0), std::logic_error);

    {
        ScopedEnv backend("PULP_PACED_WAKE", "busy-wait");
        CHECK_THROWS_AS(pulp::test::paced::read_wake_config(), std::invalid_argument);
    }
    {
        ScopedEnv backend("PULP_PACED_WAKE", "sleep-until");
        ScopedEnv guard("PULP_PACED_WAKE_GUARD_NS", "75000");
        ScopedEnv late("PULP_PACED_INJECT_LATE_US", "300");
        const auto config = pulp::test::paced::read_wake_config();
        CHECK(config.backend == WakeBackend::sleep_until);
        CHECK(config.guard_ns == 75'000);
        CHECK(config.inject_late_us == 300);
    }
    {
        ScopedEnv guard("PULP_PACED_WAKE_GUARD_NS", "x");
        CHECK_THROWS_AS(pulp::test::paced::read_wake_config(), std::invalid_argument);
    }
    {
        ScopedEnv every_env("PULP_PACED_INJECT_LATE_EVERY", "0");
        CHECK_THROWS_AS(pulp::test::paced::read_wake_config(), std::invalid_argument);
    }
    const auto defaults = pulp::test::paced::read_wake_config();
    CHECK(defaults.backend == WakeBackend::deadline_spin);
    CHECK(defaults.guard_ns < 0);
}

TEST_CASE("Paced deadline driver on the system clock releases every callback at or after target",
          "[paced][deadline-driver]") {
    pulp::test::paced::SystemWakePlatform platform;
    pulp::test::paced::DeadlineDriver driver(platform, kRate, kFrames, WakeConfig{});
    const auto& settings = driver.settings();
    CHECK(platform.clock_mapping().attempts >= 1);
    CHECK(settings.clock_mapping_skew_ns == platform.clock_mapping_skew_ns());
    CHECK(settings.clock_mapping_skew_ns >= 0);
    CHECK(settings.guard_source == "calibrated");
    CHECK(settings.guard_ns >= pulp::test::paced::kMinGuardNs);
    CHECK(settings.guard_ns <= settings.guard_bound_ns);
#if defined(__APPLE__)
    CHECK(settings.kernel_wait == "mach_wait_until");
#elif defined(__linux__)
    CHECK(settings.kernel_wait == "clock_nanosleep_abstime");
#endif
    driver.start();
    const auto origin = driver.origin_ns();
    for (std::size_t i = 0; i < 32; ++i) {
        const auto tick = driver.wait(i);
        INFO("index=" << i);
        CHECK(tick.target_ns == origin + driver.target_offset_ns(i));
        CHECK(tick.start_ns >= tick.target_ns);
        CHECK(tick.kernel_wake_ns <= tick.start_ns);
        CHECK(tick.spin_cpu_ns >= 0);
    }
}

// Paced-timing matrix cell with an empty callback: the A/B baseline for the
// wake backends. Its oracle is trivially true; it never qualifies.
TEST_CASE("Paced no-op wake cell", "[.][paced][paced-matrix]") {
    const auto config = pulp::test::paced::read_config("noop", 128);
    REQUIRE(config.variant == "noop");
    REQUIRE((config.rate == 44100 || config.rate == 48000 || config.rate == 96000));
    REQUIRE((config.frames == 32 || config.frames == 64 || config.frames == 128 ||
             config.frames == 256));
    std::uint64_t calls = 0;
    const auto result = pulp::test::paced::run_cell(
        config, [&](std::size_t, std::uint64_t) { ++calls; },
        [&](std::size_t index, std::uint64_t) { return calls == index + 1; });
    CHECK(pulp::test::paced::print_cell("noop-wake", config, result) == 0);
}
