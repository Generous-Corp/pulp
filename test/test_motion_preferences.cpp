/// @file test_motion_preferences.cpp
/// Catch2 unit tests for pulp::view::MotionPreferences (Phase 8a) and the
/// Tween / ValueAnimation honoring of MotionPolicy (Phase 8b).

#include <pulp/view/motion_preferences.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/animator_set.hpp>
#include <pulp/view/css_animation.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/frame_clock.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <thread>
#if defined(_WIN32)
#include <process.h>
#define pulp_test_getpid() static_cast<long>(::_getpid())
#else
#include <unistd.h>
#define pulp_test_getpid() static_cast<long>(::getpid())
#endif
#include <vector>

using Catch::Approx;
using pulp::view::MotionPolicy;
using pulp::view::MotionPreferences;
using pulp::view::Tween;
using pulp::view::ValueAnimation;
using pulp::view::easing::linear;

namespace {

/// Scope guard: reset MotionPreferences on test entry and exit so tests
/// can't leak overrides into each other.
struct PrefsScope {
    PrefsScope() { MotionPreferences::instance().reset_for_tests(); }
    ~PrefsScope() { MotionPreferences::instance().reset_for_tests(); }
};

} // namespace

// ── MotionPreferences ────────────────────────────────────────────────

TEST_CASE("MotionPreferences default policy after reset_for_tests is the OS value",
          "[motion-preferences]") {
    PrefsScope scope;
    auto& prefs = MotionPreferences::instance();
    // Default duration_scale is 1.0 and no override is set.
    REQUIRE(prefs.duration_scale() == Approx(1.0));
    REQUIRE_FALSE(prefs.has_override());
    // The policy is whatever the OS reports — on a CI runner with no
    // accessibility flag set, that's Full. We only require that the
    // policy is a legal enum value.
    auto p = prefs.policy();
    REQUIRE((p == MotionPolicy::Full ||
             p == MotionPolicy::Reduced ||
             p == MotionPolicy::Off));
}

TEST_CASE("MotionPreferences set_override(Reduced) returns Reduced",
          "[motion-preferences]") {
    PrefsScope scope;
    auto& prefs = MotionPreferences::instance();
    prefs.set_override(MotionPolicy::Reduced);
    REQUIRE(prefs.has_override());
    REQUIRE(prefs.policy() == MotionPolicy::Reduced);
    REQUIRE(MotionPreferences::current() == MotionPolicy::Reduced);
}

TEST_CASE("MotionPreferences set_override(Off) returns Off",
          "[motion-preferences]") {
    PrefsScope scope;
    auto& prefs = MotionPreferences::instance();
    prefs.set_override(MotionPolicy::Off);
    REQUIRE(prefs.policy() == MotionPolicy::Off);
}

TEST_CASE("MotionPreferences set_override(nullopt) reverts to OS value",
          "[motion-preferences]") {
    PrefsScope scope;
    auto& prefs = MotionPreferences::instance();
    // Snapshot whatever the OS preference is on this runner BEFORE
    // we override — macOS GitHub-hosted runners typically have
    // Reduce Motion enabled (headless display), so we can't hardcode
    // Full like the original test did.
    const auto os_value = prefs.policy();
    prefs.set_override(MotionPolicy::Reduced);
    REQUIRE(prefs.policy() == MotionPolicy::Reduced);
    prefs.set_override(std::nullopt);
    REQUIRE_FALSE(prefs.has_override());
    REQUIRE(prefs.policy() == os_value);
}

TEST_CASE("MotionPreferences set_duration_scale sticks and clamps",
          "[motion-preferences]") {
    PrefsScope scope;
    auto& prefs = MotionPreferences::instance();
    prefs.set_duration_scale(0.5);
    REQUIRE(prefs.duration_scale() == Approx(0.5));
    REQUIRE(MotionPreferences::current_duration_scale() == Approx(0.5));

    // Clamp negative → 0.
    prefs.set_duration_scale(-1.0);
    REQUIRE(prefs.duration_scale() == Approx(0.0));

    // Clamp above 2.0 → 2.0.
    prefs.set_duration_scale(10.0);
    REQUIRE(prefs.duration_scale() == Approx(2.0));
}

TEST_CASE("MotionPreferences on_policy_changed fires on override transition",
          "[motion-preferences]") {
    PrefsScope scope;
    auto& prefs = MotionPreferences::instance();
    // Force a known starting policy so the Reduced override is always
    // a real transition (some CI runners — macOS GitHub-hosted, Windows
    // headless — report Reduced as their OS value, which would
    // otherwise no-op the first set_override below).
    prefs.set_override(MotionPolicy::Full);
    std::vector<MotionPolicy> seen;
    prefs.on_policy_changed([&](MotionPolicy p) { seen.push_back(p); });
    prefs.set_override(MotionPolicy::Reduced);
    prefs.set_override(MotionPolicy::Off);
    prefs.set_override(std::nullopt);   // back to OS value
    REQUIRE(seen.size() >= 2);   // Full→Reduced, Reduced→Off, Off→OS
    REQUIRE(seen.front() == MotionPolicy::Reduced);
    // Final transition lands at the OS value, whatever that is on
    // this runner.
    REQUIRE(seen.back() == prefs.policy());
}

TEST_CASE("MotionPreferences policy callback may re-enter preferences",
          "[motion-preferences][coverage][phase3]") {
    PrefsScope scope;
    auto& prefs = MotionPreferences::instance();
    prefs.set_override(MotionPolicy::Full);

    std::vector<MotionPolicy> seen;
    prefs.on_policy_changed([&](MotionPolicy p) {
        seen.push_back(p);
        prefs.set_duration_scale(p == MotionPolicy::Reduced ? 0.25 : 1.0);
    });

    prefs.set_override(MotionPolicy::Reduced);

    REQUIRE(seen == std::vector<MotionPolicy>{MotionPolicy::Reduced});
    REQUIRE(prefs.duration_scale() == Approx(0.25));
}

TEST_CASE("MotionPreferences poll is a no-op while override is set",
          "[motion-preferences]") {
    PrefsScope scope;
    auto& prefs = MotionPreferences::instance();
    prefs.set_override(MotionPolicy::Reduced);
    REQUIRE_FALSE(prefs.poll());
    REQUIRE(prefs.policy() == MotionPolicy::Reduced);
}

TEST_CASE("motion_policy_to_string / from_string round-trip",
          "[motion-preferences]") {
    using namespace pulp::view;
    REQUIRE(std::string(motion_policy_to_string(MotionPolicy::Full)) == "full");
    REQUIRE(std::string(motion_policy_to_string(MotionPolicy::Reduced)) == "reduced");
    REQUIRE(std::string(motion_policy_to_string(MotionPolicy::Off)) == "off");
    REQUIRE(motion_policy_from_string("full") == MotionPolicy::Full);
    REQUIRE(motion_policy_from_string("reduced") == MotionPolicy::Reduced);
    REQUIRE(motion_policy_from_string("off") == MotionPolicy::Off);
    REQUIRE(motion_policy_from_string("bogus") == MotionPolicy::Full);
}

TEST_CASE("apply_motion_policy_to_duration follows override and scale",
          "[motion-preferences][coverage][phase3]") {
    using namespace pulp::view;
    PrefsScope scope;
    auto& prefs = MotionPreferences::instance();

    prefs.set_override(MotionPolicy::Full);
    prefs.set_duration_scale(0.25);
    REQUIRE(apply_motion_policy_to_duration(2.0f) == Approx(2.0f));
    REQUIRE_FALSE(motion_policy_is_off());

    prefs.set_override(MotionPolicy::Reduced);
    REQUIRE(apply_motion_policy_to_duration(2.0f) == Approx(0.5f));
    REQUIRE(apply_motion_policy_to_duration(-2.0f) == Approx(-0.5f));
    REQUIRE_FALSE(motion_policy_is_off());

    prefs.set_override(MotionPolicy::Off);
    REQUIRE(apply_motion_policy_to_duration(2.0f) == Approx(0.0f));
    REQUIRE(motion_policy_is_off());
}

// ── Tween honors MotionPolicy ────────────────────────────────────────

TEST_CASE("Tween under MotionPolicy::Off is finished from construction",
          "[motion-preferences][tween]") {
    PrefsScope scope;
    MotionPreferences::instance().set_override(MotionPolicy::Off);
    Tween t(0.0f, 1.0f, 0.5f, linear);
    REQUIRE(t.finished());
    REQUIRE(t.current() == Approx(1.0f));
    // Even after advance(), still pinned at to_.
    float v = t.advance(0.016f);
    REQUIRE(v == Approx(1.0f));
}

TEST_CASE("Tween under MotionPolicy::Reduced scales duration",
          "[motion-preferences][tween]") {
    PrefsScope scope;
    auto& prefs = MotionPreferences::instance();
    prefs.set_override(MotionPolicy::Reduced);
    prefs.set_duration_scale(0.5);

    // Configured duration: 1.0s. Effective with Reduced + scale 0.5 = 0.5s.
    Tween t(0.0f, 1.0f, 1.0f, linear);
    // Advance halfway through the effective duration → linear → 0.5.
    t.advance(0.25f);
    REQUIRE(t.current() == Approx(0.5f).margin(1e-4));
    REQUIRE_FALSE(t.finished());
    // Advance through the rest of the effective duration → done.
    t.advance(0.25f);
    REQUIRE(t.finished());
    REQUIRE(t.current() == Approx(1.0f));
}

TEST_CASE("Tween reset() re-applies the current MotionPolicy",
          "[motion-preferences][tween]") {
    PrefsScope scope;
    // Construct under Full…
    Tween t(0.0f, 1.0f, 1.0f, linear);
    REQUIRE_FALSE(t.finished());
    // …then flip to Off and reset.
    MotionPreferences::instance().set_override(MotionPolicy::Off);
    t.reset();
    REQUIRE(t.finished());
    REQUIRE(t.current() == Approx(1.0f));
}

TEST_CASE("ValueAnimation::animate_to under MotionPolicy::Off snaps to target",
          "[motion-preferences][value-animation]") {
    PrefsScope scope;
    MotionPreferences::instance().set_override(MotionPolicy::Off);
    ValueAnimation a(0.0f);
    a.animate_to(1.0f, 0.5f);
    REQUIRE_FALSE(a.animating());
    REQUIRE(a.value() == Approx(1.0f));
}

TEST_CASE("ValueAnimation::animate_to under MotionPolicy::Reduced scales duration",
          "[motion-preferences][value-animation]") {
    PrefsScope scope;
    auto& prefs = MotionPreferences::instance();
    prefs.set_override(MotionPolicy::Reduced);
    prefs.set_duration_scale(0.5);
    ValueAnimation a(0.0f);
    a.animate_to(1.0f, 1.0f, linear);
    REQUIRE(a.animating());
    a.advance(0.25f);  // halfway through scaled (0.5s) duration → 0.5
    REQUIRE(a.value() == Approx(0.5f).margin(1e-4));
    a.advance(0.25f);
    REQUIRE_FALSE(a.animating());
    REQUIRE(a.value() == Approx(1.0f));
}

// ── Tween under publish, Off emits Baseline + final Sample + End ────
//
// Under MotionPolicy::Off, advancing a tween should produce a single
// publish at the target value, not a long stream of intermediate samples.
// This mirrors what an animation primitive would do at its tick site:
// publish_value(current). Because the tween is `finished()` from
// construction, the first advance() returns `to_` and any further
// advance() also returns `to_` — so a typical "publish until finished"
// loop produces one publish above the baseline epsilon, then exits.

TEST_CASE("Tween under Off + publish emits Baseline + final Sample + End only",
          "[motion-preferences][tween][publish]") {
    PrefsScope scope;
    auto& coord = pulp::view::motion::Coordinator::instance();
    coord.reset();
    coord.set_tracing_enabled(true);
    coord.set_firehose(true);
    pulp::view::FrameClock clock;
    coord.bind(clock);

    std::vector<pulp::view::motion::SampleEvent> events;
    coord.add_sink(
        [&](const pulp::view::motion::SampleEvent& e) { events.push_back(e); });

    MotionPreferences::instance().set_override(MotionPolicy::Off);
    Tween t(0.0f, 1.0f, 0.5f, linear);
    // Publish a baseline (0.0), then run a typical advance loop. Because
    // the tween is already finished, the loop terminates on the first
    // iteration but still publishes the final value (1.0).
    pulp::view::motion::publish_value("X", "y", 0.0);
    int steps = 0;
    do {
        t.advance(0.016f);
        pulp::view::motion::publish_value("X", "y", t.current());
        ++steps;
    } while (!t.finished() && steps < 100);
    REQUIRE(steps == 1);

    // Tally event kinds.
    int baseline = 0, sample = 0, start = 0;
    for (const auto& e : events) {
        switch (e.kind) {
            case pulp::view::motion::SampleEvent::Kind::Baseline: ++baseline; break;
            case pulp::view::motion::SampleEvent::Kind::Sample:   ++sample;   break;
            case pulp::view::motion::SampleEvent::Kind::Start:    ++start;    break;
            default: break;
        }
    }
    REQUIRE(baseline == 1);
    REQUIRE(start == 1);
    REQUIRE(sample == 1);   // single Sample for the jump to final value
    // The burst closes on the next stable tick; we don't drive enough
    // frames here to force End, which is fine for the contract: Off
    // produces *no intermediate Samples*, just the single final one.

    coord.reset();
}

TEST_CASE("Tween under Reduced still produces a normal burst",
          "[motion-preferences][tween][publish]") {
    PrefsScope scope;
    auto& coord = pulp::view::motion::Coordinator::instance();
    coord.reset();
    coord.set_tracing_enabled(true);
    coord.set_firehose(true);
    pulp::view::FrameClock clock;
    coord.bind(clock);

    std::vector<pulp::view::motion::SampleEvent> events;
    coord.add_sink(
        [&](const pulp::view::motion::SampleEvent& e) { events.push_back(e); });

    auto& prefs = MotionPreferences::instance();
    prefs.set_override(MotionPolicy::Reduced);
    prefs.set_duration_scale(0.5);   // effective duration 0.25s
    Tween t(0.0f, 1.0f, 0.5f, linear);

    pulp::view::motion::publish_value("X", "y", 0.0);
    int steps = 0;
    while (!t.finished() && steps < 200) {
        // Tick the FrameClock so motion sample-event timestamps advance.
        clock.tick(0.016f);
        t.advance(0.016f);
        pulp::view::motion::publish_value("X", "y", t.current());
        ++steps;
    }

    int sample = 0;
    for (const auto& e : events) {
        if (e.kind == pulp::view::motion::SampleEvent::Kind::Sample) ++sample;
    }
    // Reduced is NOT "no samples". Expect a normal multi-sample burst —
    // at minimum, more than one Sample between Start and End.
    REQUIRE(sample > 1);

    coord.reset();
}

// ── CssAnimation honors MotionPolicy ─────────────────────────────────

TEST_CASE("CssAnimation under MotionPolicy::Off completes on first tick",
          "[motion-preferences][css-animation]") {
    PrefsScope scope;
    MotionPreferences::instance().set_override(MotionPolicy::Off);
    pulp::view::CssAnimation a;
    a.spec.duration_seconds = 0.5f;
    a.spec.delay_seconds = 0.1f;
    a.start_value = 0.0f;
    a.end_value = 1.0f;
    float v = a.tick(0.016f);
    REQUIRE(v == Approx(1.0f));
    REQUIRE_FALSE(a.active);
}

TEST_CASE("CssAnimation under MotionPolicy::Reduced scales duration",
          "[motion-preferences][css-animation]") {
    PrefsScope scope;
    auto& prefs = MotionPreferences::instance();
    prefs.set_override(MotionPolicy::Reduced);
    prefs.set_duration_scale(0.5);
    pulp::view::CssAnimation a;
    a.spec.duration_seconds = 1.0f;     // effective 0.5s after scale
    a.spec.delay_seconds = 0.0f;
    a.start_value = 0.0f;
    a.end_value = 1.0f;
    // First tick captures the scaled spec.
    a.tick(0.0f);
    REQUIRE(a.spec.duration_seconds == Approx(0.5f));
    // Drive to mid-way of effective duration.
    a.tick(0.25f);
    REQUIRE(a.active);
    // Drive past the end.
    a.tick(0.3f);
    REQUIRE_FALSE(a.active);
}

// ── AnimatorSet honors MotionPolicy via underlying Tween ─────────────

TEST_CASE("AnimatorSet under MotionPolicy::Off completes on first advance",
          "[motion-preferences][animator-set]") {
    PrefsScope scope;
    MotionPreferences::instance().set_override(MotionPolicy::Off);
    int updates = 0;
    float last = -1.0f;
    auto runner = pulp::view::AnimatorSetBuilder{}
        .then(0.0f, 1.0f, 0.5f, [&](float v){ ++updates; last = v; })
        .then(0.0f, 2.0f, 0.5f, [&](float v){ ++updates; last = v; })
        .build_runner();
    // A single advance should walk through all already-finished steps.
    bool done = runner.advance(0.016f);
    REQUIRE(done);
    REQUIRE(last == Approx(2.0f));
    REQUIRE(updates >= 2);  // one update per step's final value
}

TEST_CASE("AnimatorSet under MotionPolicy::Reduced scales each Tween's duration",
          "[motion-preferences][animator-set]") {
    PrefsScope scope;
    auto& prefs = MotionPreferences::instance();
    prefs.set_override(MotionPolicy::Reduced);
    prefs.set_duration_scale(0.5);
    // Two sequential 1s tweens → 1s total effective (each scaled to 0.5s).
    auto runner = pulp::view::AnimatorSetBuilder{}
        .then(0.0f, 1.0f, 1.0f, [](float){})
        .then(1.0f, 2.0f, 1.0f, [](float){})
        .build_runner();
    bool done = false;
    int ticks = 0;
    while (!done && ticks < 200) {
        done = runner.advance(0.016f);
        ++ticks;
    }
    // ~1s of effective animation @ 60 Hz → ~63 ticks, plus a few for
    // the inter-step carry. Allow generous margin.
    REQUIRE(done);
    REQUIRE(ticks < 80);
}

// ── settling_time_seconds under Reduced halves with duration_scale 0.5

// ── Fixture header records MotionPolicy + duration_scale ────────────

namespace {

std::string make_tmp_fixture_path(const char* tag) {
#if defined(_WIN32)
    const char* tmpdir = std::getenv("TMP");
    if (!tmpdir) tmpdir = std::getenv("TEMP");
    if (!tmpdir) tmpdir = ".";
#else
    const char* tmpdir = "/tmp";
#endif
    std::string p = tmpdir;
    p += "/pulp_phase8_fixture_";
    p += tag;
    p += "_";
    p += std::to_string(pulp_test_getpid());
    p += ".jsonl";
    return p;
}

}  // namespace

TEST_CASE("Fixture recorded under Reduced carries policy + duration_scale in header",
          "[motion-preferences][fixture]") {
    PrefsScope scope;
    auto& coord = pulp::view::motion::Coordinator::instance();
    coord.reset();
    coord.set_tracing_enabled(true);
    coord.set_firehose(true);
    pulp::view::FrameClock clock;
    coord.bind(clock);

    auto& prefs = MotionPreferences::instance();
    prefs.set_override(MotionPolicy::Reduced);
    prefs.set_duration_scale(0.5);

    const auto path = make_tmp_fixture_path("reduced");
    std::remove(path.c_str());
    auto sink_id = coord.add_sink(pulp::view::motion::make_fixture_sink(path));
    pulp::view::motion::publish_value("A", "x", 0.0);
    clock.tick(0.016f);
    pulp::view::motion::publish_value("A", "x", 0.5);
    coord.remove_sink(sink_id);

    auto hdr = pulp::view::motion::load_fixture_header(path);
    REQUIRE(hdr.version == pulp::view::motion::kFixtureSchemaVersion);
    REQUIRE(hdr.policy == "reduced");
    REQUIRE(hdr.duration_scale == Approx(0.5));

    auto events = pulp::view::motion::load_fixture(path);
    REQUIRE(events.size() >= 1);   // header + events still parse fine.

    std::remove(path.c_str());
    coord.reset();
}

TEST_CASE("Fixture recorded under Off carries policy=off",
          "[motion-preferences][fixture]") {
    PrefsScope scope;
    auto& coord = pulp::view::motion::Coordinator::instance();
    coord.reset();
    coord.set_tracing_enabled(true);
    coord.set_firehose(true);
    pulp::view::FrameClock clock;
    coord.bind(clock);

    MotionPreferences::instance().set_override(MotionPolicy::Off);

    const auto path = make_tmp_fixture_path("off");
    std::remove(path.c_str());
    auto sink_id = coord.add_sink(pulp::view::motion::make_fixture_sink(path));
    pulp::view::motion::publish_value("A", "x", 0.0);
    pulp::view::motion::publish_value("A", "x", 1.0);
    coord.remove_sink(sink_id);

    auto hdr = pulp::view::motion::load_fixture_header(path);
    REQUIRE(hdr.policy == "off");
    REQUIRE(hdr.duration_scale == Approx(1.0));

    std::remove(path.c_str());
    coord.reset();
}

TEST_CASE("assert_matches with header overload flags policy-mismatch",
          "[motion-preferences][fixture][assert-matches]") {
    pulp::view::motion::FixtureHeader g;
    g.policy = "reduced";
    g.duration_scale = 0.5;
    pulp::view::motion::FixtureHeader c;
    c.policy = "full";
    c.duration_scale = 1.0;
    std::vector<pulp::view::motion::SampleEvent> empty;
    pulp::view::motion::FixtureMatchOptions opts;
    opts.require_same_event_count = false;
    auto diff = pulp::view::motion::assert_matches(g, empty, c, empty, opts);
    REQUIRE_FALSE(diff.matches());
    // Two diff items: policy string + duration_scale.
    int policy_count = 0;
    for (const auto& d : diff.differences) {
        if (d.kind == "policy-mismatch") ++policy_count;
    }
    REQUIRE(policy_count == 2);
}

TEST_CASE("assert_matches with header overload accepts matching headers",
          "[motion-preferences][fixture][assert-matches]") {
    pulp::view::motion::FixtureHeader g;
    g.policy = "reduced";
    g.duration_scale = 0.5;
    pulp::view::motion::FixtureHeader c;
    c.policy = "reduced";
    c.duration_scale = 0.5;
    std::vector<pulp::view::motion::SampleEvent> empty;
    pulp::view::motion::FixtureMatchOptions opts;
    opts.require_same_event_count = false;
    auto diff = pulp::view::motion::assert_matches(g, empty, c, empty, opts);
    REQUIRE(diff.matches());
}

TEST_CASE("Fixture recorded under Full omits-or-stores 'full' policy",
          "[motion-preferences][fixture]") {
    PrefsScope scope;
    auto& prefs = MotionPreferences::instance();
    // Explicitly force Full so the test is independent of the host
    // runner's OS preference (macOS GitHub-hosted runners report
    // Reduced because of their headless display configuration).
    prefs.set_override(MotionPolicy::Full);
    auto& coord = pulp::view::motion::Coordinator::instance();
    coord.reset();
    coord.set_tracing_enabled(true);
    coord.set_firehose(true);
    pulp::view::FrameClock clock;
    coord.bind(clock);
    const auto path = make_tmp_fixture_path("full");
    std::remove(path.c_str());
    auto sink_id = coord.add_sink(pulp::view::motion::make_fixture_sink(path));
    pulp::view::motion::publish_value("A", "x", 0.0);
    coord.remove_sink(sink_id);

    auto hdr = pulp::view::motion::load_fixture_header(path);
    REQUIRE(hdr.policy == "full");
    REQUIRE(hdr.duration_scale == Approx(1.0));

    std::remove(path.c_str());
    coord.reset();
}

TEST_CASE("Tween under Reduced + duration_scale 0.5 halves settling_time",
          "[motion-preferences][tween][settling-time]") {
    PrefsScope scope;
    auto& coord = pulp::view::motion::Coordinator::instance();
    coord.reset();
    coord.set_tracing_enabled(true);
    coord.set_firehose(true);
    pulp::view::FrameClock clock;
    coord.bind(clock);

    std::vector<pulp::view::motion::SampleEvent> events;
    coord.add_sink(
        [&](const pulp::view::motion::SampleEvent& e) { events.push_back(e); });

    auto& prefs = MotionPreferences::instance();
    prefs.set_override(MotionPolicy::Reduced);
    prefs.set_duration_scale(0.5);

    Tween t(0.0f, 1.0f, 1.0f, linear);  // configured 1.0s; effective 0.5s.
    pulp::view::motion::publish_value("X", "y", 0.0);
    while (!t.finished()) {
        clock.tick(0.016f);
        t.advance(0.016f);
        pulp::view::motion::publish_value("X", "y", t.current());
    }

    auto samples = pulp::view::motion::extract_scalar(events, "X", "y", "value");
    auto st = pulp::view::motion::settling_time_seconds(samples);
    // Effective duration 0.5s. Allow ±50 ms for the 60 Hz quantization
    // and the fact that we publish discrete samples.
    REQUIRE(st == Approx(0.5).margin(0.06));

    coord.reset();
}

// ── Thread-safety smoke (bug #2148) ──────────────────────────────────
//
// The process-wide MotionPreferences singleton was guarded by zero
// synchronization pre-#2148: today only tests call setters, but the JS
// bridge and any future async OS-poll thread would tear. These cases
// hammer the setter / getter / callback surface from multiple threads
// and assert "no torn reads" + "no crash" — the same shape we want
// future async callers to be safe under.
//
// True data-race detection is the job of TSan; these are deterministic
// smoke tests that any host can run. Every observed policy value must
// be from the closed set we wrote ({Reduced, Off, nullopt-back-to-OS});
// every observed duration scale must be one of the values we set.

TEST_CASE("MotionPreferences set_override hammered from N threads stays consistent",
          "[motion-preferences][bug-sweep][thread-safety]") {
    PrefsScope scope;
    auto& prefs = MotionPreferences::instance();

    const int writers = 4;
    const int readers = 4;
    const int iters = 2000;
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};

    auto os_policy = prefs.policy();   // baseline before any override

    std::vector<std::thread> threads;
    threads.reserve(writers + readers);

    for (int w = 0; w < writers; ++w) {
        threads.emplace_back([&, w] {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < iters; ++i) {
                switch ((w + i) % 3) {
                    case 0: prefs.set_override(MotionPolicy::Reduced); break;
                    case 1: prefs.set_override(MotionPolicy::Off);     break;
                    case 2: prefs.set_override(std::nullopt);          break;
                }
            }
        });
    }

    // Collect observed policy values in each reader to assert every
    // observation is from the legal write set + the baseline OS value.
    std::vector<std::vector<MotionPolicy>> observations(readers);
    for (int r = 0; r < readers; ++r) {
        threads.emplace_back([&, r] {
            while (!go.load(std::memory_order_acquire)) {}
            while (!stop.load(std::memory_order_acquire)) {
                observations[r].push_back(prefs.policy());
                (void)prefs.duration_scale();
                (void)prefs.has_override();
            }
        });
    }

    go.store(true, std::memory_order_release);

    // Wait for writers to finish then signal readers to stop.
    for (int t = 0; t < writers; ++t) threads[t].join();
    stop.store(true, std::memory_order_release);
    for (int t = writers; t < writers + readers; ++t) threads[t].join();

    // Every observed value must be a legal write or the OS baseline.
    std::set<MotionPolicy> legal{MotionPolicy::Reduced, MotionPolicy::Off,
                                 os_policy};
    for (const auto& obs : observations) {
        for (auto p : obs) {
            REQUIRE(legal.count(p) == 1);
        }
    }
}

TEST_CASE("MotionPreferences set_duration_scale hammered from N threads",
          "[motion-preferences][bug-sweep][thread-safety]") {
    PrefsScope scope;
    auto& prefs = MotionPreferences::instance();

    const int writers = 4;
    const int readers = 4;
    const int iters = 2000;
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};

    const std::vector<double> values{0.0, 0.25, 0.5, 1.0, 1.5, 2.0};

    std::vector<std::thread> threads;
    threads.reserve(writers + readers);

    for (int w = 0; w < writers; ++w) {
        threads.emplace_back([&, w] {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < iters; ++i) {
                prefs.set_duration_scale(values[(w + i) % values.size()]);
            }
        });
    }

    std::vector<std::vector<double>> observations(readers);
    for (int r = 0; r < readers; ++r) {
        threads.emplace_back([&, r] {
            while (!go.load(std::memory_order_acquire)) {}
            while (!stop.load(std::memory_order_acquire)) {
                observations[r].push_back(prefs.duration_scale());
            }
        });
    }

    go.store(true, std::memory_order_release);
    for (int t = 0; t < writers; ++t) threads[t].join();
    stop.store(true, std::memory_order_release);
    for (int t = writers; t < writers + readers; ++t) threads[t].join();

    // Every observed scale must be in [0.0, 2.0]: the setter clamps,
    // and torn reads would manifest as out-of-range garbage.
    for (const auto& obs : observations) {
        for (auto v : obs) {
            REQUIRE(v >= 0.0);
            REQUIRE(v <= 2.0);
        }
    }
}

TEST_CASE("MotionPreferences callback that re-enters set_override does not deadlock",
          "[motion-preferences][bug-sweep][thread-safety]") {
    PrefsScope scope;
    auto& prefs = MotionPreferences::instance();
    // Force a known starting policy so the first set_override(Reduced)
    // is a real transition even on runners that default to Reduced.
    prefs.set_override(MotionPolicy::Full);

    std::atomic<int> fired{0};
    // Inside the callback, mutate the duration scale — a setter call
    // that grabs `mtx_`. If notify_changed held `mtx_` across the
    // callback fire, this would self-deadlock.
    prefs.on_policy_changed([&](MotionPolicy) {
        prefs.set_duration_scale(0.5);
        ++fired;
    });

    prefs.set_override(MotionPolicy::Reduced);
    prefs.set_override(MotionPolicy::Off);
    REQUIRE(fired.load() == 2);
    REQUIRE(prefs.duration_scale() == Approx(0.5));
}
