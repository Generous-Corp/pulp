/// @file test_motion_cost.cpp
/// Catch2 tests for motion cost attribution (CostSample +
/// CostAttributor). Covers:
///   - Off by default — no samples until set_enabled(true).
///   - Each tick produces one CostSample when enabled.
///   - active_trace_ids reflects which traces emitted on the tick.
///   - active_provenance carries the Phase 9 envelope per trace.
///   - Render-cost fields populate from a CostProbe.
///   - Graceful degradation when no probe is wired.
///   - JSONL serialization round-trips a stream.
///   - Bridge to RenderPassManager + DirtyTracker via
///     `make_render_cost_probe`.

#include <pulp/render/dirty_tracker.hpp>
#include <pulp/render/render_pass.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/motion_cost.hpp>
#include <pulp/view/motion_cost_render.hpp>
#include <pulp/view/view.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#if defined(_WIN32)
#include <process.h>
#define pulp_test_getpid() static_cast<long>(::_getpid())
#else
#include <unistd.h>
#define pulp_test_getpid() static_cast<long>(::getpid())
#endif
#include <vector>

using Catch::Approx;
using pulp::view::FrameClock;
using pulp::view::View;
using namespace pulp::view::motion;

namespace {

/// Reset both singletons. Each test owns its FrameClock + a buffer
/// that the attributor pushes into.
class CostFixture {
public:
    CostFixture() {
        Coordinator::instance().reset();
        CostAttributor::instance().reset();
        Coordinator::instance().bind(clock);
    }
    ~CostFixture() {
        Coordinator::instance().reset();
        CostAttributor::instance().reset();
    }

    FrameClock clock;
    std::vector<CostSample> samples;
};

/// A trivial sampler that drives one published metric so the trace
/// has a non-empty sample on every tick.
struct CountingSampler {
    double value = 0.0;
    double operator()() { value += 1.0; return value; }
};

std::string unique_path(const char* tag) {
#if defined(_WIN32)
    const char* tmpdir = std::getenv("TMP");
    if (!tmpdir) tmpdir = std::getenv("TEMP");
    if (!tmpdir) tmpdir = ".";
#else
    const char* tmpdir = "/tmp";
#endif
    std::string p = tmpdir;
    p += "/pulp-motion-cost-";
    p += tag;
    p += "-";
    p += std::to_string(pulp_test_getpid());
    p += "-";
    p += std::to_string(std::rand());
    p += ".jsonl";
    return p;
}

}  // namespace

// ── Off by default ───────────────────────────────────────────────────

TEST_CASE("CostAttributor emits nothing while disabled", "[motion-cost]") {
    CostFixture fx;
    int sink = CostAttributor::instance().add_sink(
        make_cost_buffer_sink(&fx.samples));
    (void)sink;

    // Tracing-enabled coordinator + dummy sink so on_tick has work.
    int evsink = Coordinator::instance().add_sink([](const SampleEvent&) {});
    (void)evsink;
    Coordinator::instance().set_tracing_enabled(true);

    CountingSampler s;
    auto t = Coordinator::instance()
                 .trace("V", {15})
                 .value("metric", [&s]() { return s(); })
                 .attach();
    REQUIRE(t.is_attached());

    fx.clock.tick(1.0f / 15.0f);
    fx.clock.tick(1.0f / 15.0f);
    REQUIRE(fx.samples.empty());
    REQUIRE(CostAttributor::instance().emitted_sample_count() == 0);
}

// ── One CostSample per tick when enabled ─────────────────────────────

TEST_CASE("CostAttributor emits one sample per tick when enabled",
          "[motion-cost]") {
    CostFixture fx;
    CostAttributor::instance().add_sink(make_cost_buffer_sink(&fx.samples));
    CostAttributor::instance().set_enabled(true);

    // Coordinator needs at least one sink + tracing on for on_tick to
    // process traces — but cost attribution should still fire even
    // when those are absent. Verify both arms.
    Coordinator::instance().set_tracing_enabled(true);
    Coordinator::instance().add_sink([](const SampleEvent&) {});

    fx.clock.tick(1.0f / 60.0f);
    fx.clock.tick(1.0f / 60.0f);
    fx.clock.tick(1.0f / 60.0f);

    REQUIRE(fx.samples.size() == 3);
    REQUIRE(CostAttributor::instance().emitted_sample_count() == 3);
}

// ── Cost emits even without coordinator event sinks ──────────────────

TEST_CASE("CostAttributor still emits when coordinator has no sinks",
          "[motion-cost]") {
    CostFixture fx;
    CostAttributor::instance().add_sink(make_cost_buffer_sink(&fx.samples));
    CostAttributor::instance().set_enabled(true);

    // Coordinator tracing OFF and no sinks installed.
    fx.clock.tick(1.0f / 60.0f);
    fx.clock.tick(1.0f / 60.0f);

    REQUIRE(fx.samples.size() == 2);
    for (const auto& s : fx.samples) {
        REQUIRE(s.active_trace_ids.empty());
        REQUIRE(s.active_provenance.empty());
    }
}

// ── active_trace_ids reflects emitting traces ────────────────────────

TEST_CASE("CostSample.active_trace_ids includes traces that emitted this tick",
          "[motion-cost]") {
    CostFixture fx;
    CostAttributor::instance().add_sink(make_cost_buffer_sink(&fx.samples));
    CostAttributor::instance().set_enabled(true);

    Coordinator::instance().set_tracing_enabled(true);
    int evsink = Coordinator::instance().add_sink([](const SampleEvent&) {});
    (void)evsink;

    CountingSampler a, b;
    auto th_a = Coordinator::instance()
                    .trace("A", {60})
                    .value("v", [&a]() { return a(); })
                    .attach();
    auto th_b = Coordinator::instance()
                    .trace("B", {60})
                    .value("v", [&b]() { return b(); })
                    .attach();
    REQUIRE(th_a.is_attached());
    REQUIRE(th_b.is_attached());

    fx.clock.tick(1.0f / 60.0f);  // baseline emission for both

    REQUIRE(fx.samples.size() == 1);
    auto& s = fx.samples.back();
    // Both traces should appear and be sorted ascending.
    REQUIRE(s.active_trace_ids.size() == 2);
    REQUIRE(s.active_trace_ids[0] < s.active_trace_ids[1]);
}

// ── Provenance round-trips through CostSample ────────────────────────

TEST_CASE("CostSample carries Provenance for each active trace",
          "[motion-cost]") {
    CostFixture fx;
    CostAttributor::instance().add_sink(make_cost_buffer_sink(&fx.samples));
    CostAttributor::instance().set_enabled(true);

    Coordinator::instance().set_tracing_enabled(true);
    Coordinator::instance().add_sink([](const SampleEvent&) {});

    CountingSampler s;
    Provenance prov;
    prov.source_kind = "design-import";
    prov.source_id = "figma:LevelMeter/Panel";
    prov.source_file = __FILE__;
    prov.source_line = __LINE__;

    auto th = Coordinator::instance()
                  .trace("Panel", {60})
                  .value("opacity", [&s]() { return s(); })
                  .with_provenance(prov)
                  .attach();
    REQUIRE(th.is_attached());

    fx.clock.tick(1.0f / 60.0f);

    REQUIRE(fx.samples.size() == 1);
    auto& cs = fx.samples.back();
    REQUIRE(cs.active_trace_ids.size() == 1);
    REQUIRE(cs.active_provenance.size() == 1);
    REQUIRE(cs.active_provenance[0].source_kind == "design-import");
    REQUIRE(cs.active_provenance[0].source_id == "figma:LevelMeter/Panel");
    REQUIRE(cs.active_provenance[0].source_line == prov.source_line);
}

// ── Probe populates render-cost fields ───────────────────────────────

TEST_CASE("CostProbe populates render_pass_duration_ms and dirty fields",
          "[motion-cost]") {
    CostFixture fx;
    CostAttributor::instance().add_sink(make_cost_buffer_sink(&fx.samples));
    CostAttributor::instance().set_enabled(true);

    // Mutable scalars the probe reads each tick.
    double cur_ms = 0.0;
    double cur_area = 0.0;
    int cur_count = 0;
    CostAttributor::instance().set_probe([&]() {
        RenderCostSnapshot s;
        s.render_pass_duration_ms = cur_ms;
        s.dirty_rect_area_px = cur_area;
        s.dirty_rect_count = cur_count;
        return s;
    });

    cur_ms = 12.5; cur_area = 400.0; cur_count = 2;
    fx.clock.tick(1.0f / 60.0f);
    cur_ms = 0.5; cur_area = 16.0; cur_count = 1;
    fx.clock.tick(1.0f / 60.0f);

    REQUIRE(fx.samples.size() == 2);
    REQUIRE(fx.samples[0].render_pass_duration_ms == Approx(12.5));
    REQUIRE(fx.samples[0].dirty_rect_area_px == Approx(400.0));
    REQUIRE(fx.samples[0].dirty_rect_count == 2);
    REQUIRE(fx.samples[1].render_pass_duration_ms == Approx(0.5));
    REQUIRE(fx.samples[1].dirty_rect_count == 1);
}

// ── Graceful degradation when no probe is wired ──────────────────────

TEST_CASE("CostSample zeroes render fields when no probe is wired",
          "[motion-cost]") {
    CostFixture fx;
    CostAttributor::instance().add_sink(make_cost_buffer_sink(&fx.samples));
    CostAttributor::instance().set_enabled(true);
    // No set_probe call.

    fx.clock.tick(1.0f / 60.0f);

    REQUIRE(fx.samples.size() == 1);
    REQUIRE(fx.samples[0].render_pass_duration_ms == Approx(0.0));
    REQUIRE(fx.samples[0].dirty_rect_area_px == Approx(0.0));
    REQUIRE(fx.samples[0].dirty_rect_count == 0);
}

TEST_CASE("Cost buffer sink tolerates a null destination",
          "[motion-cost][coverage][phase3]") {
    auto sink = make_cost_buffer_sink(nullptr);

    CostSample s;
    s.frame = 5;

    REQUIRE_NOTHROW(sink(s));
}

// ── Bridge probe pulls real RenderPassManager + DirtyTracker stats ──

TEST_CASE("make_render_cost_probe surfaces RenderPassManager + DirtyTracker",
          "[motion-cost]") {
    CostFixture fx;
    CostAttributor::instance().add_sink(make_cost_buffer_sink(&fx.samples));
    CostAttributor::instance().set_enabled(true);

    pulp::render::RenderPassManager passes;
    pulp::render::DirtyTracker dirty;
    CostAttributor::instance().set_probe(make_render_cost_probe(&passes, &dirty));

    // Simulate a render frame: begin/end one pass, mark two dirty rects.
    passes.begin_frame();
    passes.begin_pass(pulp::render::RenderPassType::content);
    passes.end_pass(/*time_ms=*/8.25f, /*draw_calls=*/4);
    passes.end_frame();
    dirty.invalidate({0, 0, 10, 20});  // area 200
    dirty.invalidate({100, 100, 5, 5});  // area 25

    fx.clock.tick(1.0f / 60.0f);

    REQUIRE(fx.samples.size() == 1);
    REQUIRE(fx.samples[0].render_pass_duration_ms == Approx(8.25));
    REQUIRE(fx.samples[0].dirty_rect_area_px == Approx(225.0));
    REQUIRE(fx.samples[0].dirty_rect_count == 2);

    // Null pointers — defensive path returns zero.
    CostAttributor::instance().set_probe(make_render_cost_probe(nullptr, nullptr));
    fx.clock.tick(1.0f / 60.0f);
    REQUIRE(fx.samples.size() == 2);
    REQUIRE(fx.samples[1].render_pass_duration_ms == Approx(0.0));
    REQUIRE(fx.samples[1].dirty_rect_count == 0);
}

// ── JSONL serialization round-trips ──────────────────────────────────

TEST_CASE("CostSample JSONL serialization round-trips a stream", "[motion-cost]") {
    CostFixture fx;
    const std::string path = unique_path("rt");
    std::remove(path.c_str());

    {
        CostAttributor::instance().add_sink(make_cost_sink(path));
        CostAttributor::instance().set_enabled(true);

        Coordinator::instance().set_tracing_enabled(true);
        Coordinator::instance().add_sink([](const SampleEvent&) {});
        CountingSampler s;
        Provenance prov;
        prov.source_kind = "tween";
        prov.source_id = "Card.opacity";
        prov.source_file = "card.cpp";
        prov.source_line = 42;
        auto th = Coordinator::instance()
                      .trace("Card", {60})
                      .value("opacity", [&s]() { return s(); })
                      .with_provenance(prov)
                      .attach();
        (void)th;

        fx.clock.tick(1.0f / 60.0f);
        fx.clock.tick(1.0f / 60.0f);
        fx.clock.tick(1.0f / 60.0f);

        // Drop sinks so the file flushes / closes.
        CostAttributor::instance().clear_sinks();
    }

    // File should exist and contain the version header + 3 body lines.
    std::ifstream in(path);
    REQUIRE(in.is_open());
    std::string first;
    REQUIRE(std::getline(in, first).good());
    REQUIRE(first.find("\"motion_cost_version\":1") != std::string::npos);

    auto loaded = load_cost_stream(path);
    REQUIRE(loaded.size() == 3);

    // Every loaded sample should have at least one active trace.
    bool found_any_prov = false;
    for (const auto& s : loaded) {
        if (!s.active_trace_ids.empty()) {
            REQUIRE(s.active_provenance.size() == s.active_trace_ids.size());
            for (const auto& p : s.active_provenance) {
                if (p.source_kind == "tween") found_any_prov = true;
            }
        }
    }
    REQUIRE(found_any_prov);

    std::remove(path.c_str());
}

TEST_CASE("load_cost_stream rejects missing or unsupported headers",
          "[motion-cost][coverage][phase3]") {
    REQUIRE(load_cost_stream(unique_path("missing")).empty());

    const std::string no_header = unique_path("no-header");
    {
        std::ofstream out(no_header);
        out << serialize_cost_sample(CostSample{}) << "\n";
    }
    REQUIRE(load_cost_stream(no_header).empty());
    std::remove(no_header.c_str());

    const std::string unsupported = unique_path("unsupported");
    {
        std::ofstream out(unsupported);
        out << "{\"motion_cost_version\":2}\n"
            << serialize_cost_sample(CostSample{}) << "\n";
    }
    REQUIRE(load_cost_stream(unsupported).empty());
    std::remove(unsupported.c_str());
}

// ── serialize_cost_sample produces well-formed JSON ──────────────────

TEST_CASE("serialize_cost_sample emits valid JSON shape", "[motion-cost]") {
    CostSample s;
    s.frame = 42;
    s.t_seconds = 1.5;
    s.render_pass_duration_ms = 9.5;
    s.dirty_rect_area_px = 256.0;
    s.dirty_rect_count = 3;
    s.active_trace_ids = {7, 11};
    Provenance p1;
    p1.source_kind = "css-transition";
    p1.source_id = "panel.bg";
    Provenance p2;
    s.active_provenance = {p1, p2};

    auto line = serialize_cost_sample(s);
    REQUIRE(line.front() == '{');
    REQUIRE(line.back() == '}');
    REQUIRE(line.find("\"frame\":42") != std::string::npos);
    REQUIRE(line.find("\"active_trace_ids\":[7,11]") != std::string::npos);
    REQUIRE(line.find("\"source_kind\":\"css-transition\"") != std::string::npos);
}

// ── Bug-sweep regressions (pre-merge sweep #2142) ────────────────────

// serialize_cost_sample used to emit raw doubles, so a single bad
// render-stat tick (NaN / Inf) produced invalid JSON tokens (`nan` /
// `inf`) that broke every downstream consumer. Match motion.cpp's
// quoted-sentinel convention and round-trip them back.
TEST_CASE("serialize_cost_sample emits NaN/Inf as quoted sentinels",
          "[motion-cost][bug-sweep]") {
    CostSample s;
    s.frame = 7;
    s.t_seconds = std::nan("");
    s.render_pass_duration_ms = std::numeric_limits<double>::infinity();
    s.dirty_rect_area_px = -std::numeric_limits<double>::infinity();
    s.dirty_rect_count = 0;

    auto line = serialize_cost_sample(s);
    REQUIRE(line.find("\"t\":\"NaN\"") != std::string::npos);
    REQUIRE(line.find("\"render_pass_duration_ms\":\"Infinity\"") != std::string::npos);
    REQUIRE(line.find("\"dirty_rect_area_px\":\"-Infinity\"") != std::string::npos);

    // Round-trip through write + load. The loader recognizes the same
    // three quoted sentinels and restores the IEEE-754 value.
    std::string path = unique_path("nan");
    {
        std::ofstream out(path);
        out << "{\"motion_cost_version\":1}\n" << line << "\n";
    }
    auto loaded = load_cost_stream(path);
    REQUIRE(loaded.size() == 1);
    REQUIRE(std::isnan(loaded[0].t_seconds));
    REQUIRE(std::isinf(loaded[0].render_pass_duration_ms));
    REQUIRE(loaded[0].render_pass_duration_ms > 0);
    REQUIRE(std::isinf(loaded[0].dirty_rect_area_px));
    REQUIRE(loaded[0].dirty_rect_area_px < 0);
    std::remove(path.c_str());
}

// load_cost_stream's active_provenance loop used line.find('}', pos)
// to locate each object's end. A literal `}` inside a source_file
// path (legal JSON — only `"` and `\\` are escaped) truncated the
// entry mid-string and corrupted every following provenance object.
// The fix is brace-depth + string-aware scanning.
TEST_CASE("load_cost_stream parses provenance objects whose strings contain '}'",
          "[motion-cost][bug-sweep]") {
    CostSample s;
    s.frame = 11;
    s.t_seconds = 0.5;
    s.dirty_rect_count = 1;
    s.active_trace_ids = {3, 4};
    Provenance p1;
    p1.source_kind = "tween";
    p1.source_id = "card.opacity";
    // The literal `}` here used to truncate the parser mid-string.
    p1.source_file = "weird/path}with-brace.cpp";
    p1.source_line = 42;
    Provenance p2;
    p2.source_kind = "css-transition";
    p2.source_id = "panel.bg";
    p2.source_file = "ok.cpp";
    p2.source_line = 7;
    s.active_provenance = {p1, p2};

    std::string path = unique_path("brace");
    {
        std::ofstream out(path);
        out << "{\"motion_cost_version\":1}\n"
            << serialize_cost_sample(s) << "\n";
    }
    auto loaded = load_cost_stream(path);
    REQUIRE(loaded.size() == 1);
    REQUIRE(loaded[0].active_provenance.size() == 2);
    REQUIRE(loaded[0].active_provenance[0].source_kind == "tween");
    REQUIRE(loaded[0].active_provenance[0].source_file == "weird/path}with-brace.cpp");
    REQUIRE(loaded[0].active_provenance[0].source_line == 42);
    REQUIRE(loaded[0].active_provenance[1].source_kind == "css-transition");
    REQUIRE(loaded[0].active_provenance[1].source_file == "ok.cpp");
    REQUIRE(loaded[0].active_provenance[1].source_line == 7);
    std::remove(path.c_str());
}
