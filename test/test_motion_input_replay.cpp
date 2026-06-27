/// @file test_motion_input_replay.cpp
/// Catch2 tests for input recording + replay. Records a
/// hover -> click -> drag sequence against a synthetic view tree, then
/// replays the same fixture against a fresh tree and asserts the
/// motion stream that emerges is identical.

#include <pulp/view/frame_clock.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/motion_preferences.hpp>
#include <pulp/view/view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#if defined(_WIN32)
#include <process.h>
#define pulp_test_getpid() static_cast<int>(::_getpid())
#else
#include <unistd.h>
#define pulp_test_getpid() static_cast<int>(::getpid())
#endif
#include <vector>

using pulp::view::FrameClock;
using pulp::view::Point;
using pulp::view::Rect;
using pulp::view::View;
using namespace pulp::view::motion;

namespace {

std::string tmp_fixture_path(const std::string& tag) {
    char buf[256];
    const char* tmpdir =
#if defined(_WIN32)
        std::getenv("TMP") ? std::getenv("TMP") :
        std::getenv("TEMP") ? std::getenv("TEMP") : ".";
#else
        "/tmp";
#endif
    std::snprintf(buf, sizeof(buf), "%s/pulp-motion-input-replay-%s-%d-%d.jsonl",
                  tmpdir, tag.c_str(), pulp_test_getpid(), std::rand());
    return buf;
}

/// Minimal View subclass that records every on_mouse_* / on_mouse_enter
/// event into a counter we can compare across record/replay tree pairs.
class RecordingView : public View {
public:
    int down_count = 0;
    int up_count = 0;
    int drag_count = 0;
    int enter_count = 0;
    int leave_count = 0;
    int click_count = 0;

    void on_mouse_down(Point) override { ++down_count; }
    void on_mouse_up(Point) override { ++up_count; }
    void on_mouse_drag(Point) override { ++drag_count; }
    void on_mouse_enter() override { ++enter_count; }
    void on_mouse_leave() override { ++leave_count; }
};

/// Build a two-node tree with a target child at known coordinates.
struct TestTree {
    std::unique_ptr<View> root;
    RecordingView* target = nullptr;   // owned by root via child list

    static TestTree make() {
        TestTree t;
        t.root = std::make_unique<View>();
        t.root->set_id("root");
        t.root->set_bounds({0, 0, 400, 400});

        auto target = std::make_unique<RecordingView>();
        target->set_id("target");
        target->set_bounds({100, 100, 100, 100});
        target->on_click = [target_ptr = target.get()]() {
            ++target_ptr->click_count;
        };
        t.target = target.get();
        t.root->add_child(std::move(target));
        return t;
    }
};

/// RAII: reset the Coordinator + MotionPreferences between cases so
/// per-case state doesn't bleed.
struct CoordReset {
    CoordReset() {
        Coordinator::instance().reset();
        pulp::view::MotionPreferences::instance().reset_for_tests();
    }
    ~CoordReset() {
        Coordinator::instance().reset();
        pulp::view::MotionPreferences::instance().reset_for_tests();
    }
};

}  // namespace

// ── Round-trip: Input event survives serialize -> load ───────────────

TEST_CASE("Input event round-trips through fixture serialize/parse",
          "[motion][input][fixture]") {
    CoordReset reset;
    const auto path = tmp_fixture_path("rt");

    FrameClock clock;
    Coordinator::instance().bind(clock);
    {
        TestTree tree = TestTree::make();
        tree.root->set_frame_clock(&clock);
        auto recorder = make_input_recorder(path);
        REQUIRE(recorder.is_recording());

        tree.root->simulate_hover({150, 150});
        clock.tick(1.0f / 60.0f);
        tree.root->simulate_click({150, 150});
        clock.tick(1.0f / 60.0f);
        tree.root->simulate_drag({150, 150}, {180, 180}, /*steps=*/4);
        clock.tick(1.0f / 60.0f);
    }
    Coordinator::instance().unbind();

    auto events = load_fixture(path);
    REQUIRE(!events.empty());

    int hover_seen = 0, click_seen = 0, drag_seen = 0;
    for (const auto& e : events) {
        if (e.kind != SampleEvent::Kind::Input) continue;
        REQUIRE(e.view_id == "target");
        if (e.input_kind == "hover") {
            ++hover_seen;
            REQUIRE(e.components.size() == 2);
            // Components sorted by name: x, y
            REQUIRE(e.components[0].first == "x");
            REQUIRE(e.components[1].first == "y");
            REQUIRE(e.components[0].second == 150.0);
            REQUIRE(e.components[1].second == 150.0);
        } else if (e.input_kind == "click") {
            ++click_seen;
            REQUIRE(e.components.size() == 2);
            REQUIRE(e.components[0].second == 150.0);
        } else if (e.input_kind == "drag") {
            ++drag_seen;
            // Drag carries 5 named components (sorted): end_x, end_y,
            // start_x, start_y, steps
            REQUIRE(e.components.size() == 5);
            double end_x = 0, end_y = 0, start_x = 0, start_y = 0, steps = 0;
            for (const auto& [k, v] : e.components) {
                if (k == "end_x") end_x = v;
                else if (k == "end_y") end_y = v;
                else if (k == "start_x") start_x = v;
                else if (k == "start_y") start_y = v;
                else if (k == "steps") steps = v;
            }
            REQUIRE(start_x == 150.0);
            REQUIRE(start_y == 150.0);
            REQUIRE(end_x == 180.0);
            REQUIRE(end_y == 180.0);
            REQUIRE(steps == 4.0);
        }
    }
    REQUIRE(hover_seen == 1);
    REQUIRE(click_seen == 1);
    REQUIRE(drag_seen == 1);

    std::remove(path.c_str());
}

// ── Recorder is OFF by default ───────────────────────────────────────

TEST_CASE("simulate_* produces no Input events when no recorder is alive",
          "[motion][input]") {
    CoordReset reset;
    REQUIRE_FALSE(input_recording_enabled());

    FrameClock clock;
    Coordinator::instance().bind(clock);
    std::vector<SampleEvent> buf;
    Coordinator::instance().add_sink(make_buffer_sink(&buf));

    TestTree tree = TestTree::make();
    tree.root->set_frame_clock(&clock);
    tree.root->simulate_click({150, 150});

    REQUIRE(buf.empty());
    Coordinator::instance().unbind();
}

// ── make_input_recorder RAII toggles the global flag ────────────────

TEST_CASE("InputRecorder RAII toggles input_recording_enabled",
          "[motion][input]") {
    CoordReset reset;
    const auto path = tmp_fixture_path("raii");
    REQUIRE_FALSE(input_recording_enabled());
    {
        auto r = make_input_recorder(path);
        REQUIRE(input_recording_enabled());
        REQUIRE(r.is_recording());
    }
    REQUIRE_FALSE(input_recording_enabled());
    std::remove(path.c_str());
}

// ── End-to-end: record, replay against a fresh tree, motion matches ─

TEST_CASE("hover -> click -> drag record/replay produces matching motion fixture",
          "[motion][input][replay]") {
    CoordReset reset;
    const auto record_path = tmp_fixture_path("record");
    const auto replay_path = tmp_fixture_path("replay");

    // ── Recording pass ──────────────────────────────────────────────
    int record_clicks = 0, record_downs = 0, record_drags = 0, record_enters = 0;
    double record_opacity_start = 0.0;
    double record_opacity_end = 0.0;
    {
        FrameClock clock;
        Coordinator::instance().bind(clock);
        Coordinator::instance().set_tracing_enabled(true);

        TestTree tree = TestTree::make();
        tree.root->set_frame_clock(&clock);

        // A scalar metric that toggles when the target is clicked.
        // We trace it so the recorded fixture contains motion samples
        // alongside the input events.
        double opacity = 0.0;
        tree.target->on_click = [t = tree.target, &opacity]() {
            ++t->click_count;
            opacity = 1.0;
        };
        auto handle = Coordinator::instance()
            .trace("Card", { /*fps=*/60 })
            .value("opacity", [&]{ return opacity; })
            .attach();

        // Sink wired BEFORE the recorder so opacity samples + input
        // events fan out through both. The recorder's own sink writes
        // the JSONL fixture; we tick once before recording so the
        // baseline opacity sample lands first.
        record_opacity_start = opacity;
        clock.tick(1.0f / 60.0f);  // Baseline opacity sample.

        auto recorder = make_input_recorder(record_path);

        tree.root->simulate_hover({150, 150});
        clock.tick(1.0f / 60.0f);
        tree.root->simulate_click({150, 150});
        clock.tick(1.0f / 60.0f);  // opacity transition Start + Sample
        clock.tick(1.0f / 60.0f);  // opacity End on next stable tick
        tree.root->simulate_drag({150, 150}, {170, 170}, /*steps=*/3);
        clock.tick(1.0f / 60.0f);

        record_clicks = tree.target->click_count;
        record_downs = tree.target->down_count;
        record_drags = tree.target->drag_count;
        record_enters = tree.target->enter_count;
        record_opacity_end = opacity;
    }
    Coordinator::instance().reset();

    // ── Replay pass against a fresh tree ────────────────────────────
    int replay_clicks = 0, replay_downs = 0, replay_drags = 0, replay_enters = 0;
    double replay_opacity_end = 0.0;
    {
        FrameClock clock;
        Coordinator::instance().bind(clock);
        Coordinator::instance().set_tracing_enabled(true);

        TestTree tree = TestTree::make();
        tree.root->set_frame_clock(&clock);

        double opacity = 0.0;
        tree.target->on_click = [t = tree.target, &opacity]() {
            ++t->click_count;
            opacity = 1.0;
        };
        auto handle = Coordinator::instance()
            .trace("Card", { 60 })
            .value("opacity", [&]{ return opacity; })
            .attach();

        // Open a fresh fixture sink so the replayed motion writes to
        // its own file. Inputs are NOT being recorded here — only the
        // motion stream — so input_recording_enabled() stays false.
        Coordinator::instance().add_sink(make_fixture_sink(replay_path));
        clock.tick(1.0f / 60.0f);   // baseline before replay starts

        const int replayed = replay_inputs(record_path, *tree.root, clock);
        REQUIRE(replayed == 3);

        // One last tick so the End event on the opacity burst lands.
        clock.tick(1.0f / 60.0f);

        replay_clicks = tree.target->click_count;
        replay_downs = tree.target->down_count;
        replay_drags = tree.target->drag_count;
        replay_enters = tree.target->enter_count;
        replay_opacity_end = opacity;
    }
    Coordinator::instance().reset();

    // Per-handler counts match (same simulate dispatch behavior).
    REQUIRE(replay_clicks == record_clicks);
    REQUIRE(replay_downs == record_downs);
    REQUIRE(replay_drags == record_drags);
    REQUIRE(replay_enters == record_enters);
    // The clicked target's side-effect (opacity toggling to 1.0) fired.
    REQUIRE(record_opacity_start == 0.0);
    REQUIRE(record_opacity_end == 1.0);
    REQUIRE(replay_opacity_end == record_opacity_end);

    // Compare the motion streams from both fixtures. We extract only
    // motion-side events (Baseline / Start / Sample / End) on the
    // `Card.opacity` metric so the comparison focuses on the animation
    // the inputs drove, not the input log itself.
    auto record_events = load_fixture(record_path);
    auto replay_events = load_fixture(replay_path);
    REQUIRE(!record_events.empty());
    REQUIRE(!replay_events.empty());

    auto motion_only = [](const std::vector<SampleEvent>& src) {
        std::vector<SampleEvent> out;
        for (const auto& e : src) {
            if (e.kind == SampleEvent::Kind::Input) continue;
            if (e.kind == SampleEvent::Kind::TraceStarted) continue;
            if (e.view_name != "Card") continue;
            out.push_back(e);
        }
        return out;
    };

    auto rec_motion = motion_only(record_events);
    auto rep_motion = motion_only(replay_events);

    // Both sides should have the same baseline -> start -> sample -> end
    // shape on the Card.opacity metric.
    auto count_kind_local = [](const std::vector<SampleEvent>& v,
                               SampleEvent::Kind k) {
        std::size_t n = 0;
        for (const auto& e : v) if (e.kind == k) ++n;
        return n;
    };
    REQUIRE(count_kind_local(rec_motion, SampleEvent::Kind::Baseline) ==
            count_kind_local(rep_motion, SampleEvent::Kind::Baseline));
    REQUIRE(count_kind_local(rec_motion, SampleEvent::Kind::Start) ==
            count_kind_local(rep_motion, SampleEvent::Kind::Start));
    REQUIRE(count_kind_local(rec_motion, SampleEvent::Kind::Sample) ==
            count_kind_local(rep_motion, SampleEvent::Kind::Sample));
    REQUIRE(count_kind_local(rec_motion, SampleEvent::Kind::End) ==
            count_kind_local(rep_motion, SampleEvent::Kind::End));

    // ID-aware comparator: identical Baseline + Start + Sample + End on
    // the same (view, metric, burst_id) triples within tolerance.
    FixtureMatchOptions opts;
    opts.require_same_event_count = false;
    opts.require_matching_policy = false;
    opts.component_epsilon = 1e-6;
    // Replay anchors on the first input's recorded timestamp and ticks
    // deltas between subsequent inputs, but the sampler-driven motion
    // events ride a separate FrameClock cadence that doesn't have a
    // fixed offset relative to the input timeline. The shape of the
    // animation (counts + values) must match exactly; the absolute t
    // is allowed to drift within one frame's worth of slop (we sample
    // at 60 fps in this test).
    opts.timing_epsilon_seconds = 0.05;
    auto diff = assert_matches(rec_motion, rep_motion, opts);
    INFO("motion fixture diff items: " << diff.differences.size());
    for (const auto& d : diff.differences) {
        INFO("  " << d.kind << " " << d.view_name << "/" << d.metric_name
             << " " << d.component_name << " " << d.detail);
    }
    REQUIRE(diff.matches());

    std::remove(record_path.c_str());
    std::remove(replay_path.c_str());
}
