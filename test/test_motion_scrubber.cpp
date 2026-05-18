/// @file test_motion_scrubber.cpp
/// Catch2 tests for pulp::inspect::MotionScrubber.
///
/// Coverage:
///   * load_fixture / scrub_to playhead semantics over a real
///     recorded fixture (forward + backward scrubs both re-emit
///     the correct prefix);
///   * play() jumps to max frame and emits every event;
///   * DomainHandler protocol roundtrip for Motion.loadFixture +
///     Motion.scrubTo + Motion.play + Motion.pause.

#include <pulp/inspect/domain_handler.hpp>
#include <pulp/inspect/motion_scrubber.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/motion.hpp>

#include <choc/text/choc_JSON.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
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

using pulp::inspect::DomainHandler;
using pulp::inspect::InspectorMessage;
using pulp::inspect::MotionScrubber;
using pulp::inspect::make_request;
using pulp::view::FrameClock;
using pulp::view::motion::Coordinator;
using pulp::view::motion::SampleEvent;
using pulp::view::motion::make_buffer_sink;
using pulp::view::motion::make_fixture_sink;
using pulp::view::motion::publish_value;

namespace {

std::string tmp_fixture_path(const std::string& tag) {
#if defined(_WIN32)
    const char* tmpdir = std::getenv("TMP");
    if (!tmpdir) tmpdir = std::getenv("TEMP");
    if (!tmpdir) tmpdir = ".";
#else
    const char* tmpdir = "/tmp";
#endif
    std::ostringstream ss;
    ss << tmpdir << "/pulp-motion-scrubber-" << tag << "-"
       << pulp_test_getpid() << "-"
       << std::rand() << ".jsonl";
    return ss.str();
}

/// Write a fixture with a non-trivial frame range. Returns the file
/// path; the caller is responsible for deleting it.
std::string record_sample_fixture(const std::string& tag, FrameClock& clock) {
    const auto path = tmp_fixture_path(tag);
    Coordinator::instance().reset();
    Coordinator::instance().bind(clock);
    Coordinator::instance().set_tracing_enabled(true);
    Coordinator::instance().set_firehose(true);
    const int sink_id = Coordinator::instance().add_sink(make_fixture_sink(path));

    // Stage frame 0: baseline publish.
    publish_value("Card", "opacity", 0.0);
    clock.tick(1.0 / 60.0);

    // Stage frame 1: change → Start + Sample at frame 1.
    publish_value("Card", "opacity", 0.5);
    clock.tick(1.0 / 60.0);

    // Stage frame 2: continued change → Sample at frame 2.
    publish_value("Card", "opacity", 0.75);
    clock.tick(1.0 / 60.0);

    // Stage frame 3: stable → End at frame 3.
    publish_value("Card", "opacity", 0.75);
    clock.tick(1.0 / 60.0);
    publish_value("Card", "opacity", 0.75);

    Coordinator::instance().remove_sink(sink_id);
    Coordinator::instance().reset();
    return path;
}

}  // namespace

// ── load_fixture populates the scrubber but emits nothing yet ─────────

TEST_CASE("MotionScrubber load_fixture is passive — no events until scrub",
          "[motion-scrubber]") {
    FrameClock clock;
    const auto path = record_sample_fixture("passive", clock);

    MotionScrubber scrub;
    std::vector<SampleEvent> sink_events;
    scrub.add_sink(make_buffer_sink(&sink_events));

    REQUIRE(scrub.load_fixture(path));
    REQUIRE(scrub.loaded());
    REQUIRE(scrub.event_count() > 0);
    REQUIRE(scrub.playhead_frame() == 0u);
    REQUIRE_FALSE(scrub.playing());
    REQUIRE(sink_events.empty());  // load is passive

    std::remove(path.c_str());
}

// ── scrub_to(N) emits exactly the events with frame <= N ──────────────

TEST_CASE("MotionScrubber scrub_to(N) re-emits prefix with frame <= N",
          "[motion-scrubber]") {
    FrameClock clock;
    const auto path = record_sample_fixture("prefix", clock);

    MotionScrubber scrub;
    REQUIRE(scrub.load_fixture(path));

    // Sanity: the recorded fixture covers at least frames 0..3.
    REQUIRE(scrub.max_frame() >= 3u);

    // Expected prefix counts based on the recorded fixture.
    auto golden = pulp::view::motion::load_fixture(path);
    auto expected_count_le = [&](std::uint64_t f) {
        std::size_t n = 0;
        for (const auto& e : golden) {
            if (e.frame <= f) ++n;
        }
        return n;
    };

    // Scrub to frame 1.
    {
        std::vector<SampleEvent> buf;
        int id = scrub.add_sink(make_buffer_sink(&buf));
        const std::size_t emitted = scrub.scrub_to(1);
        REQUIRE(emitted == expected_count_le(1));
        REQUIRE(buf.size() == expected_count_le(1));
        for (const auto& e : buf) REQUIRE(e.frame <= 1u);
        REQUIRE(scrub.playhead_frame() == 1u);
        scrub.remove_sink(id);
    }

    // Scrub forward to a higher frame — sink gets a larger prefix.
    {
        std::vector<SampleEvent> buf;
        int id = scrub.add_sink(make_buffer_sink(&buf));
        const std::size_t emitted = scrub.scrub_to(3);
        REQUIRE(emitted == expected_count_le(3));
        REQUIRE(buf.size() == expected_count_le(3));
        for (const auto& e : buf) REQUIRE(e.frame <= 3u);
        REQUIRE(scrub.playhead_frame() == 3u);
        scrub.remove_sink(id);
    }

    // Scrub backwards to frame 0 — sink gets only the baseline prefix.
    {
        std::vector<SampleEvent> buf;
        int id = scrub.add_sink(make_buffer_sink(&buf));
        const std::size_t emitted = scrub.scrub_to(0);
        REQUIRE(emitted == expected_count_le(0));
        REQUIRE(buf.size() == expected_count_le(0));
        for (const auto& e : buf) REQUIRE(e.frame == 0u);
        REQUIRE(scrub.playhead_frame() == 0u);
        // The prefix at frame 0 must be strictly smaller than the
        // prefix at frame 3 — backwards scrub is observable.
        REQUIRE(buf.size() < expected_count_le(3));
        scrub.remove_sink(id);
    }

    std::remove(path.c_str());
}

// ── play() jumps to the max frame and emits every event ───────────────

TEST_CASE("MotionScrubber play() emits every loaded event",
          "[motion-scrubber]") {
    FrameClock clock;
    const auto path = record_sample_fixture("play", clock);

    MotionScrubber scrub;
    REQUIRE(scrub.load_fixture(path));

    std::vector<SampleEvent> buf;
    scrub.add_sink(make_buffer_sink(&buf));

    const std::size_t emitted = scrub.play();
    REQUIRE(scrub.playing());
    REQUIRE(emitted == scrub.event_count());
    REQUIRE(buf.size() == scrub.event_count());
    REQUIRE(scrub.playhead_frame() == scrub.max_frame());

    scrub.pause();
    REQUIRE_FALSE(scrub.playing());

    std::remove(path.c_str());
}

// ── Unloaded scrubber rejects scrub / play gracefully ────────────────

TEST_CASE("MotionScrubber scrub_to / play are no-ops when no fixture loaded",
          "[motion-scrubber]") {
    MotionScrubber scrub;
    REQUIRE_FALSE(scrub.loaded());
    REQUIRE(scrub.scrub_to(10) == 0u);
    REQUIRE(scrub.play() == 0u);
}

// ── Bug-sweep regressions (pre-merge sweep #2142) ────────────────────

// Re-emission used to drop SampleEvent::Kind::Input entirely — the
// switch fell through to "?", the method routed to kMotionSample, and
// input_kind / view_id were not serialized at all. Phase 10 fixtures
// stayed silent on scrub.
TEST_CASE("MotionScrubber re-emits Phase 10 Input events with input_kind + view_id",
          "[motion-scrubber][bug-sweep]") {
    FrameClock clock;
    const auto path = tmp_fixture_path("input-replay");

    // Record a fixture containing an Input event.
    Coordinator::instance().reset();
    Coordinator::instance().bind(clock);
    Coordinator::instance().set_tracing_enabled(true);
    const int sink_id = Coordinator::instance().add_sink(make_fixture_sink(path));

    SampleEvent input;
    input.kind = SampleEvent::Kind::Input;
    input.view_name = "Knob";
    input.metric_name = "interaction";
    input.input_kind = "click";
    input.view_id = "knob-7";
    Coordinator::instance().dispatch_input_event(input);
    clock.tick(1.0 / 60.0);

    Coordinator::instance().remove_sink(sink_id);
    Coordinator::instance().reset();

    MotionScrubber scrub;
    REQUIRE(scrub.load_fixture(path));

    std::vector<SampleEvent> buf;
    scrub.add_sink(make_buffer_sink(&buf));
    REQUIRE(scrub.play() > 0u);

    bool found_input = false;
    for (const auto& e : buf) {
        if (e.kind == SampleEvent::Kind::Input) {
            found_input = true;
            REQUIRE(e.input_kind == "click");
            REQUIRE(e.view_id == "knob-7");
        }
    }
    REQUIRE(found_input);

    std::remove(path.c_str());
}

// emit_prefix_locked used to fire sinks (and broadcast through the
// inspector server) while holding mtx_. A sink that re-entered any
// MotionScrubber accessor self-deadlocked. The fix is dispatch-outside-
// lock via dispatch_snapshot(); this test pins that property.
TEST_CASE("MotionScrubber sink may re-enter scrubber accessors during scrub",
          "[motion-scrubber][bug-sweep]") {
    FrameClock clock;
    const auto path = record_sample_fixture("reentrant", clock);

    MotionScrubber scrub;
    REQUIRE(scrub.load_fixture(path));

    // Sink that calls back into the scrubber on every event. Under
    // the old design, this deadlocked on the second event because
    // event_count() / playhead_frame() / playing() / loaded() all
    // acquire mtx_, which scrub_to was still holding.
    std::size_t reentrant_calls = 0;
    scrub.add_sink([&scrub, &reentrant_calls](const SampleEvent&) {
        (void)scrub.event_count();
        (void)scrub.playhead_frame();
        (void)scrub.playing();
        (void)scrub.loaded();
        ++reentrant_calls;
    });

    const std::size_t emitted = scrub.scrub_to(scrub.max_frame());
    REQUIRE(emitted > 0);
    REQUIRE(reentrant_calls == emitted);

    std::remove(path.c_str());
}

// ── DomainHandler protocol roundtrip ─────────────────────────────────

TEST_CASE("DomainHandler routes Motion.loadFixture + scrubTo to MotionScrubber",
          "[motion-scrubber][protocol]") {
    FrameClock clock;
    const auto path = record_sample_fixture("proto", clock);

    MotionScrubber scrub;
    DomainHandler dh;
    dh.set_motion_scrubber(&scrub);

    std::vector<SampleEvent> wire_events;
    scrub.add_sink(make_buffer_sink(&wire_events));

    // Motion.loadFixture — on Windows the path uses backslashes which
    // are invalid JSON unless escaped; the host's path functions accept
    // forward slashes too, so substitute for the wire payload.
    std::string wire_path = path;
    std::replace(wire_path.begin(), wire_path.end(), '\\', '/');
    std::ostringstream load_params;
    load_params << "{\"path\":\"" << wire_path << "\"}";
    auto load_resp = dh.handle(make_request(1, "Motion.loadFixture",
                                            load_params.str()));
    REQUIRE_FALSE(load_resp.is_error);
    auto load_body = choc::json::parse(load_resp.params_json);
    REQUIRE(load_body["ok"].getBool() == true);
    REQUIRE(load_body["event_count"].getInt64() > 0);
    REQUIRE(load_body["max_frame"].getInt64() >= 3);
    REQUIRE(load_body.hasObjectMember("header"));
    REQUIRE(load_body["header"]["version"].getInt64() ==
            pulp::view::motion::kFixtureSchemaVersion);

    // Motion.scrubTo frame 2
    auto scrub_resp = dh.handle(make_request(2, "Motion.scrubTo",
                                             "{\"frame\":2}"));
    REQUIRE_FALSE(scrub_resp.is_error);
    auto scrub_body = choc::json::parse(scrub_resp.params_json);
    REQUIRE(scrub_body["playhead_frame"].getInt64() == 2);
    const auto emitted = scrub_body["emitted_count"].getInt64();
    REQUIRE(emitted > 0);
    REQUIRE(static_cast<std::int64_t>(wire_events.size()) == emitted);
    for (const auto& e : wire_events) REQUIRE(e.frame <= 2u);

    // Motion.play
    wire_events.clear();
    auto play_resp = dh.handle(make_request(3, "Motion.play", "{}"));
    REQUIRE_FALSE(play_resp.is_error);
    auto play_body = choc::json::parse(play_resp.params_json);
    REQUIRE(play_body["playing"].getBool() == true);
    REQUIRE(static_cast<std::int64_t>(wire_events.size()) ==
            play_body["emitted_count"].getInt64());
    REQUIRE(scrub.playhead_frame() == scrub.max_frame());

    // Motion.pause
    auto pause_resp = dh.handle(make_request(4, "Motion.pause", "{}"));
    REQUIRE_FALSE(pause_resp.is_error);
    auto pause_body = choc::json::parse(pause_resp.params_json);
    REQUIRE(pause_body["playing"].getBool() == false);

    std::remove(path.c_str());
}

// ── Missing scrubber returns a targeted error ────────────────────────

TEST_CASE("DomainHandler errors on scrubber methods without a scrubber attached",
          "[motion-scrubber][protocol]") {
    DomainHandler dh;
    auto resp = dh.handle(make_request(1, "Motion.scrubTo", "{\"frame\":0}"));
    REQUIRE(resp.is_error);
    REQUIRE(resp.params_json.find("scrubber") != std::string::npos);
}

// ── scrubTo with no fixture loaded surfaces a usable error ──────────

TEST_CASE("Motion.scrubTo errors when no fixture is loaded",
          "[motion-scrubber][protocol]") {
    MotionScrubber scrub;
    DomainHandler dh;
    dh.set_motion_scrubber(&scrub);
    auto resp = dh.handle(make_request(1, "Motion.scrubTo", "{\"frame\":0}"));
    REQUIRE(resp.is_error);
}

// ── Existing MotionInspector methods still flow when scrubber present ─

TEST_CASE("Non-scrubber Motion.* methods still route to MotionInspector",
          "[motion-scrubber][protocol]") {
    // No MotionInspector attached, only a scrubber. The dispatcher
    // must NOT consume Motion.startTrace into the scrubber — it should
    // surface the "No motion inspector attached" error.
    MotionScrubber scrub;
    DomainHandler dh;
    dh.set_motion_scrubber(&scrub);
    auto resp = dh.handle(make_request(1, "Motion.startTrace",
                                       R"({"view_name":"X","fps":60,"metrics":[]})"));
    REQUIRE(resp.is_error);
    REQUIRE(resp.params_json.find("motion inspector") != std::string::npos);
}

// ── FixtureFileSink double-registration regression (bug #2151) ───────
//
// `make_fixture_sink(path)` returns a Sink that owns a shared
// `FixtureFileSink` state (file handle + header_written flag). If the
// SAME sink is registered on both `Coordinator::add_sink` AND
// `MotionScrubber::add_sink`, two sink-fire threads can interleave
// writes to the underlying `std::ofstream` — header gets emitted
// twice, or a body line gets cut in half by another thread's line.
//
// Pre-#2151 the sink had no internal synchronization. Post-fix, the
// sink takes a `std::mutex` around the open + header-write + body-
// write sequence. This test drives N threads invoking the SAME sink
// in parallel and asserts the resulting file parses cleanly: exactly
// one header line, every body line a valid JSON event.

TEST_CASE("FixtureFileSink shared by N threads writes intact lines",
          "[motion-fixture-sink][bug-sweep][thread-safety][issue-2151]") {
    const auto path = tmp_fixture_path("shared-2151");
    std::remove(path.c_str());

    auto sink = pulp::view::motion::make_fixture_sink(path);

    // Construct a SampleEvent template. We use the same shape on
    // every thread but tag `frame` with the worker id so we can
    // verify after the fact that every published event made it
    // through (no dropped or truncated lines).
    const int workers = 8;
    const int iters = 250;
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (int w = 0; w < workers; ++w) {
        threads.emplace_back([&, w] {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < iters; ++i) {
                SampleEvent e;
                e.kind = SampleEvent::Kind::Sample;
                e.view_name = "View";
                e.metric_name = "value";
                e.t_seconds = static_cast<double>(w) + 0.001 * i;
                e.frame = static_cast<std::uint64_t>(w * iters + i);
                e.precision = 3;
                e.components.emplace_back("value", static_cast<double>(i));
                sink(e);
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    // 1. The fixture loader must succeed (any torn header or truncated
    //    event line would surface as an empty vector — load_fixture
    //    bails on parse failure).
    auto events = pulp::view::motion::load_fixture(path);
    REQUIRE(events.size() == static_cast<std::size_t>(workers * iters));

    // 2. The header must have parsed: schema version present.
    auto hdr = pulp::view::motion::load_fixture_header(path);
    REQUIRE(hdr.version == pulp::view::motion::kFixtureSchemaVersion);

    // 3. Re-read the raw file: must have exactly one header line, and
    //    every subsequent line must start with `{"kind":` (i.e. is a
    //    full event line, not a torn fragment).
    std::ifstream in(path);
    REQUIRE(in.is_open());
    std::string line;
    REQUIRE(std::getline(in, line));   // header
    REQUIRE(line.find("motion_fixture_version") != std::string::npos);
    int header_dupes = 0;
    int body_lines = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line.find("motion_fixture_version") != std::string::npos) {
            ++header_dupes;
        } else {
            // Must look like a JSON object literal. A torn write
            // would either leave a non-`{` opener or omit the
            // closing `}`. Both conditions would also have made
            // load_fixture return empty, but check the raw bytes
            // too so a regression surfaces with a clear locator.
            REQUIRE(line.front() == '{');
            REQUIRE(line.back()  == '}');
            ++body_lines;
        }
    }
    REQUIRE(header_dupes == 0);
    REQUIRE(body_lines == workers * iters);

    std::remove(path.c_str());
}
