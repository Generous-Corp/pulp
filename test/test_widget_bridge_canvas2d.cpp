// WidgetBridge Canvas2D tests covering the bridge surface end-to-end through
// CanvasWidget::paint:
//
//   - canvasSetTransform / canvasClip / canvasGlobalCompositeOperation:
//     the bridge records a CanvasDrawCmd, paint() replays it
//   - canvasMeasureText / canvasSetLineDash / canvasDrawImage /
//     canvasGetImageData / canvasPutImageData (issue-916)
//   - 4-arg canvasFillText preserves prior state
//
// Tests share fixtures with the smaller WidgetBridge surface clusters.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/asset_manager.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <thread>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

// Local copy of the JS single-quote escaper that test_widget_bridge.cpp
// also defines. The original is `static` (file-local) — duplicated here
// to keep the split self-contained until a shared
// test/test_widget_bridge_helpers.hpp lands.
static std::string js_single_quoted(std::string value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c; break;
        }
    }
    return out;
}

// ── canvasSetTransform / canvasClip / canvasGlobalCompositeOperation (issue-896) ──
//
// These three CanvasRenderingContext2D bridge functions are exercised end-to-end:
// the JS bridge records a CanvasDrawCmd, and CanvasWidget::paint() replays each
// command on a pulp::canvas::RecordingCanvas, which lets us assert on the
// resulting Canvas-API call sequence.
namespace {
static pulp::view::CanvasWidget* canvasFromBridge(pulp::view::WidgetBridge& bridge,
                                                  pulp::view::ScriptEngine& engine,
                                                  const std::string& id) {
    auto value = engine.evaluate("document.getElementById('" + id + "')._id");
    auto nativeId = std::string(value.getWithDefault<std::string_view>(""));
    return dynamic_cast<pulp::view::CanvasWidget*>(bridge.widget(nativeId));
}
} // namespace

TEST_CASE("WidgetBridge canvasSetTransform records affine matrix and replays via setMatrix",
          "[view][bridge][canvas][issue-896]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'xform-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        // Bypass the prelude — the bridge function is the unit under test.
        canvasSetTransform(c._id, 2.0, 0.0, 0.0, 3.0, 17.0, 23.0);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "xform-canvas");
    REQUIRE(canvas != nullptr);
    REQUIRE(canvas->command_count() >= 1);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    bool sawSetTransform = false;
    for (auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_transform) {
            REQUIRE_THAT(cmd.f[0], WithinAbs(2.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[1], WithinAbs(0.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[2], WithinAbs(0.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[3], WithinAbs(3.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[4], WithinAbs(17.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[5], WithinAbs(23.0f, 1e-5f));
            sawSetTransform = true;
        }
    }
    REQUIRE(sawSetTransform);
}

TEST_CASE("WidgetBridge canvasClip records clip command and replays Canvas::clip()",
          "[view][bridge][canvas][issue-896]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'clip-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        canvasBeginPath(c._id);
        canvasMoveTo(c._id, 10, 10);
        canvasLineTo(c._id, 80, 10);
        canvasLineTo(c._id, 80, 60);
        canvasLineTo(c._id, 10, 60);
        canvasClosePath(c._id);
        canvasClip(c._id);
        canvasRect(c._id, 0, 0, 200, 100, '#ff0000');
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "clip-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    int clipIndex = -1;
    int fillRectAfterClip = -1;
    int idx = 0;
    for (auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::clip) clipIndex = idx;
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_rect && clipIndex >= 0
            && fillRectAfterClip < 0) {
            fillRectAfterClip = idx;
        }
        ++idx;
    }
    REQUIRE(clipIndex >= 0);                  // canvasClip dispatched Canvas::clip()
    REQUIRE(fillRectAfterClip > clipIndex);   // subsequent draws are still issued (clip applies, doesn't drop)
}

TEST_CASE("WidgetBridge canvasGlobalCompositeOperation maps CSS strings to BlendMode",
          "[view][bridge][canvas][issue-896]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'comp-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        canvasGlobalCompositeOperation(c._id, 'destination-out');
        canvasGlobalCompositeOperation(c._id, 'multiply');
        canvasGlobalCompositeOperation(c._id, 'lighter');
        // Invalid string — must be a graceful no-op (no command emitted).
        canvasGlobalCompositeOperation(c._id, 'not-a-real-blend-mode');
        canvasGlobalCompositeOperation(c._id, 'source-over');
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "comp-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    std::vector<int> blendIndices;
    for (auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_blend_mode) {
            blendIndices.push_back(static_cast<int>(cmd.f[0]));
        }
    }

    // 4 valid strings → 4 set_blend_mode commands; the bogus mode string
    // emits nothing.
    REQUIRE(blendIndices.size() == 4);
    using BM = pulp::canvas::Canvas::BlendMode;
    REQUIRE(blendIndices[0] == static_cast<int>(BM::destination_out));
    REQUIRE(blendIndices[1] == static_cast<int>(BM::multiply));
    REQUIRE(blendIndices[2] == static_cast<int>(BM::lighter));
    REQUIRE(blendIndices[3] == static_cast<int>(BM::source_over));
}

TEST_CASE("WidgetBridge direct Canvas2D gap APIs replay expected canvas commands",
          "[view][bridge][canvas]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'canvas-gap-api';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);

        canvasSetTextAlign(c._id, 'right');
        canvasSetTextBaseline(c._id, 'middle');
        canvasClearRect(c._id, 1, 2, 3, 4);
        canvasClipRect(c._id, 5, 6, 7, 8);
        canvasFillRoundedRect(c._id, 10, 11, 12, 13, 4, '#ff0000');
        canvasStrokeRoundedRect(c._id, 20, 21, 22, 23, 6, '#00ff00', 2.5);
        canvasStrokeCircle(c._id, 30, 31, 9, '#0000ff', 3);
        canvasSetGlobalAlpha(c._id, 0.25);
        canvasSetLineCap(c._id, 'square');
        canvasSetLineJoin(c._id, 'bevel');
        canvasArc(c._id, 40, 41, 10, 0.5, 1.5, '#abcdef', 2);
        canvasSetBlendMode(c._id, 'copy');
        canvasSetBlendMode(c._id, 'not-a-real-blend-mode');
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "canvas-gap-api");
    REQUIRE(canvas != nullptr);
    REQUIRE(canvas->command_count() == 12);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    using DrawType = pulp::canvas::DrawCommand::Type;
    auto first = [&](DrawType type) -> const pulp::canvas::DrawCommand* {
        for (const auto& cmd : rec.commands()) {
            if (cmd.type == type) return &cmd;
        }
        return nullptr;
    };

    const auto* align = first(DrawType::set_text_align);
    REQUIRE(align != nullptr);
    REQUIRE(align->f[0] == static_cast<float>(pulp::canvas::TextAlign::right));

    const auto* clear = first(DrawType::clear_rect);
    REQUIRE(clear != nullptr);
    REQUIRE_THAT(clear->f[0], WithinAbs(1.0f, 1e-5f));
    REQUIRE_THAT(clear->f[1], WithinAbs(2.0f, 1e-5f));
    REQUIRE_THAT(clear->f[2], WithinAbs(3.0f, 1e-5f));
    REQUIRE_THAT(clear->f[3], WithinAbs(4.0f, 1e-5f));

    const auto* clip = first(DrawType::clip_rect);
    REQUIRE(clip != nullptr);
    REQUIRE_THAT(clip->f[0], WithinAbs(5.0f, 1e-5f));
    REQUIRE_THAT(clip->f[1], WithinAbs(6.0f, 1e-5f));
    REQUIRE_THAT(clip->f[2], WithinAbs(7.0f, 1e-5f));
    REQUIRE_THAT(clip->f[3], WithinAbs(8.0f, 1e-5f));

    const auto* fillRounded = first(DrawType::fill_rounded_rect);
    REQUIRE(fillRounded != nullptr);
    REQUIRE_THAT(fillRounded->f[0], WithinAbs(10.0f, 1e-5f));
    REQUIRE_THAT(fillRounded->f[4], WithinAbs(4.0f, 1e-5f));

    const auto* strokeRounded = first(DrawType::stroke_rounded_rect);
    REQUIRE(strokeRounded != nullptr);
    REQUIRE_THAT(strokeRounded->f[0], WithinAbs(20.0f, 1e-5f));
    REQUIRE_THAT(strokeRounded->f[4], WithinAbs(6.0f, 1e-5f));

    const auto* strokeCircle = first(DrawType::stroke_circle);
    REQUIRE(strokeCircle != nullptr);
    REQUIRE_THAT(strokeCircle->f[0], WithinAbs(30.0f, 1e-5f));
    REQUIRE_THAT(strokeCircle->f[1], WithinAbs(31.0f, 1e-5f));
    REQUIRE_THAT(strokeCircle->f[2], WithinAbs(9.0f, 1e-5f));

    const auto* cap = first(DrawType::set_line_cap);
    REQUIRE(cap != nullptr);
    REQUIRE(cap->f[0] == static_cast<float>(pulp::canvas::LineCap::square));

    const auto* join = first(DrawType::set_line_join);
    REQUIRE(join != nullptr);
    REQUIRE(join->f[0] == static_cast<float>(pulp::canvas::LineJoin::bevel));

    const auto* arc = first(DrawType::stroke_arc);
    REQUIRE(arc != nullptr);
    REQUIRE_THAT(arc->f[0], WithinAbs(40.0f, 1e-5f));
    REQUIRE_THAT(arc->f[1], WithinAbs(41.0f, 1e-5f));
    REQUIRE_THAT(arc->f[2], WithinAbs(10.0f, 1e-5f));
    REQUIRE_THAT(arc->f[3], WithinAbs(0.5f, 1e-5f));
    REQUIRE_THAT(arc->f[4], WithinAbs(1.5f, 1e-5f));

    REQUIRE(rec.count(DrawType::set_blend_mode) == 1);
    const auto* blend = first(DrawType::set_blend_mode);
    REQUIRE(blend != nullptr);
    REQUIRE(blend->f[0] == static_cast<float>(static_cast<int>(pulp::canvas::Canvas::BlendMode::copy)));
}

// ───────────────────────────────────────────────────────────────────────────
// WidgetBridge auto-wires repaint_callback_ to the root view's host invalidator
// so JS-driven UI changes (and rAF callbacks) actually schedule a paint when
// the View owns its own bridge.
// ───────────────────────────────────────────────────────────────────────────

namespace {

class CountingWindowHost final : public WindowHost {
public:
    int repaint_calls = 0;

    void show() override {}
    void hide() override {}
    bool is_visible() const override { return true; }
    void repaint() override { ++repaint_calls; }
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
};

class CountingPluginViewHost final : public pulp::view::PluginViewHost {
public:
    int repaint_calls = 0;
    Size size = {400, 300};

    pulp::view::NativeViewHandle native_handle() override { return {}; }
    void attach_to_parent(pulp::view::NativeViewHandle) override {}
    void detach() override {}
    void repaint() override { ++repaint_calls; }
    void set_size(uint32_t w, uint32_t h) override { size = {w, h}; }
    Size get_size() const override { return size; }
};

} // namespace

TEST_CASE("WidgetBridge default repaint_callback routes to root host (issue 899)",
          "[view][bridge][issue-899]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;

    CountingWindowHost host;
    root.set_window_host(&host);

    WidgetBridge bridge(engine, root, store);

    REQUIRE(host.repaint_calls == 0);

    // layout() always calls request_repaint() inside the bridge; with the
    // auto-wired default that must reach the host. Without the fix this
    // stays at 0 because repaint_callback_ is null.
    bridge.load_script("layout()");

    REQUIRE(host.repaint_calls >= 1);
}

TEST_CASE("WidgetBridge default repaint reaches host through child view (issue 899)",
          "[view][bridge][issue-899]") {
    // The Spectr NativeEditorView case: a child View owns its own
    // WidgetBridge. set_window_host propagates the host to children on
    // add_child, so the child's own request_repaint() reaches the host
    // directly without parent walking.
    ScriptEngine engine;
    View top_root;
    top_root.set_bounds({0, 0, 800, 600});

    CountingWindowHost host;
    top_root.set_window_host(&host);

    auto child_owned = std::make_unique<View>();
    auto* child = child_owned.get();
    top_root.add_child(std::move(child_owned));
    child->set_bounds({0, 0, 400, 300});

    StateStore store;
    WidgetBridge bridge(engine, *child, store);

    REQUIRE(host.repaint_calls == 0);

    bridge.load_script("layout()");

    REQUIRE(host.repaint_calls >= 1);
}

TEST_CASE("WidgetBridge default repaint routes to plugin_view_host (issue 899)",
          "[view][bridge][issue-899]") {
    // DAW plugin context: when a View has a PluginViewHost set instead of
    // a WindowHost, the bridge's auto-wired repaint must still reach it.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});

    CountingPluginViewHost plugin_host;
    root.set_plugin_view_host(&plugin_host);

    StateStore store;
    WidgetBridge bridge(engine, root, store);

    REQUIRE(plugin_host.repaint_calls == 0);
    bridge.load_script("layout()");
    REQUIRE(plugin_host.repaint_calls >= 1);
}

TEST_CASE("WidgetBridge set_repaint_callback overrides the default (issue 899)",
          "[view][bridge][issue-899]") {
    // Hosts (e.g. the standalone window) must still be able to replace the
    // default invalidator with a window-level repaint callback.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;

    CountingWindowHost host;
    root.set_window_host(&host);

    WidgetBridge bridge(engine, root, store);

    int override_calls = 0;
    bridge.set_repaint_callback([&] { ++override_calls; });

    bridge.load_script("layout()");

    REQUIRE(override_calls >= 1);
    // The override must displace the default — not run alongside it.
    REQUIRE(host.repaint_calls == 0);
}

TEST_CASE("WidgetBridge default repaint is a no-op when no host attached (issue 899)",
          "[view][bridge][issue-899]") {
    // Before the fix, repaint_callback_ was null. After the fix it routes
    // through View::request_repaint(), which itself silently no-ops when
    // no host is wired up. Verify that constructing/using the bridge in a
    // host-less test setup still works (lots of existing tests rely on
    // this).
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    REQUIRE_NOTHROW(bridge.load_script("layout()"));
}

TEST_CASE("View::request_repaint reaches host through propagated descendants (issue 899)",
          "[view][issue-899]") {
    // set_window_host propagates the host pointer to every existing and
    // subsequently-added child. A grandchild's own window_host_ is
    // therefore set; calling request_repaint() reaches the host directly
    // without parent walking.
    View root;
    CountingWindowHost host;
    root.set_window_host(&host);

    auto child_owned = std::make_unique<View>();
    auto* child = child_owned.get();
    root.add_child(std::move(child_owned));

    auto grand_owned = std::make_unique<View>();
    auto* grand = grand_owned.get();
    child->add_child(std::move(grand_owned));

    REQUIRE(host.repaint_calls == 0);
    grand->request_repaint();
    REQUIRE(host.repaint_calls == 1);

    // Detached view: silently no-ops, no crash.
    View orphan;
    REQUIRE_NOTHROW(orphan.request_repaint());
}

// ───────────────────────────────────────────────────────────────────────────
// __requestFrame__ must call request_repaint() so that requestAnimationFrame()
// actually drives the host paint loop. Without this wiring, JS-side rAF
// callbacks accumulate in pending_frame_ids_ but the host never schedules the
// paint that drains them — Spectr's FilterBank canvas stays blank.
// ───────────────────────────────────────────────────────────────────────────

TEST_CASE("WidgetBridge requestAnimationFrame triggers a host repaint (issue 921)",
          "[view][bridge][issue-921]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;

    CountingWindowHost host;
    root.set_window_host(&host);

    WidgetBridge bridge(engine, root, store);

    // Snapshot — bridge construction may itself touch the host (it does
    // not today, but pin behavior to a delta around the rAF call).
    int repaint_baseline = host.repaint_calls;

    bridge.load_script("var __raf_id = window.requestAnimationFrame(function () {});");

    // The fix wires __requestFrame__ → request_repaint() → repaint_callback_
    // → root.request_repaint() → host.repaint(). Without it, repaint_calls
    // would not advance until something else (mouse, resize, layout) ran.
    REQUIRE(host.repaint_calls > repaint_baseline);

    // The id round-trip from JS proves the queue path itself still works
    // — the fix only adds the repaint signal, it must not break the queue.
    auto id_value = engine.evaluate("__raf_id");
    REQUIRE(id_value.getWithDefault<int>(-1) >= 1);
}

TEST_CASE("WidgetBridge cancelAnimationFrame removes the pending native frame",
          "[view][bridge][async][issue-493]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;

    CountingWindowHost host;
    root.set_window_host(&host);

    WidgetBridge bridge(engine, root, store);

    const int baseline_repaints = host.repaint_calls;
    bridge.load_script("var invalid_raf_id = window.requestAnimationFrame('not a callback');");
    REQUIRE(engine.evaluate("invalid_raf_id").getWithDefault<int>(-1) == 0);
    REQUIRE(host.repaint_calls == baseline_repaints);

    bridge.load_script(R"(
        var raf_hits = 0;
        var canceled_raf_id = window.requestAnimationFrame(function () {
            raf_hits += 1;
        });
        window.cancelAnimationFrame(canceled_raf_id);
    )");

    REQUIRE(engine.evaluate("canceled_raf_id").getWithDefault<int>(-1) >= 1);
    REQUIRE(host.repaint_calls > baseline_repaints);

    const int repaints_after_cancel = host.repaint_calls;
    bridge.poll_async_results();
    bridge.service_frame_callbacks();

    REQUIRE(engine.evaluate("raf_hits").getWithDefault<int>(-1) == 0);
    REQUIRE(host.repaint_calls == repaints_after_cancel);
}

TEST_CASE("WidgetBridge requestAnimationFrame chain keeps requesting paints (issue 921)",
          "[view][bridge][issue-921]") {
    // The Spectr FilterBank pattern: a draw() callback re-arms itself via
    // requestAnimationFrame. Each rAF must request a paint so the host
    // actually services the next frame. Three queued rAFs across a poll
    // cycle therefore produce at least three host repaint signals.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;

    CountingWindowHost host;
    root.set_window_host(&host);

    WidgetBridge bridge(engine, root, store);

    int repaint_baseline = host.repaint_calls;

    bridge.load_script(R"(
        var rafs_requested = 0;
        function tick() {
            rafs_requested++;
            if (rafs_requested < 3) window.requestAnimationFrame(tick);
        }
        window.requestAnimationFrame(tick);
    )");

    // First rAF was synchronous in the script and must already have hit
    // the host once.
    REQUIRE(host.repaint_calls > repaint_baseline);

    // Drain queued rAFs so each tick re-arms and lands another rAF /
    // host repaint. Bound the loop — production hosts coalesce, but the
    // test only needs to see the rAF→repaint signal continuing.
    for (int i = 0; i < 10; ++i) {
        bridge.poll_async_results();
        if (engine.evaluate("rafs_requested").getWithDefault<int>(-1) >= 3)
            break;
    }

    REQUIRE(engine.evaluate("rafs_requested").getWithDefault<int>(-1) == 3);
    // Each of the three rAFs requested a repaint (one synchronous + two
    // re-armed inside tick()). The host counter is monotonic; we want
    // strictly more than the first-rAF post-condition above.
    REQUIRE(host.repaint_calls >= repaint_baseline + 3);
}

// ── canvasMeasureText / canvasSetLineDash / canvasDrawImage / canvasGetImageData / canvasPutImageData (issue-916) ──
//
// These five CanvasRenderingContext2D bridge functions close the gap
// list in issue-916. The first three are the user-priority items
// (Spectr FilterBank text alignment + footer icon rendering); the last
// two ship together to land the surface in one pass.

TEST_CASE("WidgetBridge canvasMeasureText returns full TextMetrics object",
          "[view][bridge][canvas][issue-916]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'measure-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.font = '20px Inter';
        var m1 = ctx.measureText('Hello');
        var m2 = ctx.measureText('Hello, world!');
        // Expose results for the C++ side to read back.
        window.__metrics_short = m1;
        window.__metrics_long  = m2;
    )");

    auto width_short = engine.evaluate("window.__metrics_short.width");
    auto width_long  = engine.evaluate("window.__metrics_long.width");
    REQUIRE(width_short.getWithDefault<double>(-1.0) > 0.0);
    REQUIRE(width_long.getWithDefault<double>(-1.0)
            > width_short.getWithDefault<double>(0.0));

    // Ascent/descent must be populated and non-zero — Spectr text
    // centring relies on these.
    auto ascent  = engine.evaluate("window.__metrics_short.fontBoundingBoxAscent");
    auto descent = engine.evaluate("window.__metrics_short.fontBoundingBoxDescent");
    REQUIRE(ascent.getWithDefault<double>(0.0)  > 0.0);
    REQUIRE(descent.getWithDefault<double>(0.0) > 0.0);

    // actualBoundingBox{Left,Right} fields exist (HTML5 spec — never
    // missing, even when the bounding box collapses to width).
    auto left  = engine.evaluate("typeof window.__metrics_short.actualBoundingBoxLeft  === 'number'");
    auto right = engine.evaluate("typeof window.__metrics_short.actualBoundingBoxRight === 'number'");
    REQUIRE(left.getWithDefault<bool>(false));
    REQUIRE(right.getWithDefault<bool>(false));
}

TEST_CASE("WidgetBridge canvasSetLineDash records pattern + phase, "
          "and an odd-length pattern is duplicated per HTML5 spec",
          "[view][bridge][canvas][issue-916][issue-952]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'dash-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.lineDashOffset = 0;
        ctx.setLineDash([5, 3, 2]);   // odd → duplicates to [5,3,2,5,3,2]
        ctx.strokeRect(10, 10, 80, 40);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "dash-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    int dashIndex = -1;
    int idx = 0;
    for (auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_line_dash) {
            // The lineDashOffset setter may flush the current empty pattern
            // before setLineDash() records the requested dash array.
            if (cmd.floats.size() != 6) {
                ++idx;
                continue;
            }
            dashIndex = idx;
            REQUIRE_THAT(cmd.f[0], WithinAbs(0.0f, 1e-5f));
            REQUIRE_THAT(cmd.floats[0], WithinAbs(5.0f, 1e-5f));
            REQUIRE_THAT(cmd.floats[1], WithinAbs(3.0f, 1e-5f));
            REQUIRE_THAT(cmd.floats[2], WithinAbs(2.0f, 1e-5f));
            REQUIRE_THAT(cmd.floats[3], WithinAbs(5.0f, 1e-5f));
            break;
        }
        ++idx;
    }
    REQUIRE(dashIndex >= 0);
}

// Spectr's bundle calls setLineDash with [4,4], [2,3], [2,2], [1,3] for the
// 0dB baseline, ruler grid, and meter markers, plus an empty array to reset
// to a solid stroke. Each call must produce a `set_line_dash` command whose
// `floats` payload equals the requested pattern verbatim — even-length
// arrays are not duplicated, and the empty pattern records a zero-length
// `floats` (HTML5 "solid line" reset).
TEST_CASE("WidgetBridge canvasSetLineDash carries spectr-style patterns "
          "and solid-line reset through to the recording stream",
          "[view][bridge][canvas][issue-952]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'spectr-dash';
        c.width = 400; c.height = 200;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        ctx.setLineDash([4, 4]);   // 0dB baseline
        ctx.strokeRect(0, 100, 400, 0);
        ctx.setLineDash([2, 3]);   // ruler grid
        ctx.strokeRect(0, 0, 400, 0);
        ctx.setLineDash([2, 2]);   // meter markers
        ctx.strokeRect(0, 50, 400, 0);
        ctx.setLineDash([1, 3]);   // band-grid dashes
        ctx.strokeRect(0, 150, 400, 0);
        ctx.setLineDash([]);       // solid-line reset
        ctx.strokeRect(0, 175, 400, 0);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "spectr-dash");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    // Collect every set_line_dash command, in order.
    std::vector<std::vector<float>> dashes;
    for (auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_line_dash) {
            dashes.emplace_back(cmd.floats.begin(), cmd.floats.end());
        }
    }

    REQUIRE(dashes.size() == 5);
    REQUIRE(dashes[0] == std::vector<float>{4.0f, 4.0f});
    REQUIRE(dashes[1] == std::vector<float>{2.0f, 3.0f});
    REQUIRE(dashes[2] == std::vector<float>{2.0f, 2.0f});
    REQUIRE(dashes[3] == std::vector<float>{1.0f, 3.0f});
    REQUIRE(dashes[4].empty()); // solid-line pass-through
}

TEST_CASE("WidgetBridge canvasDrawImage records draw_image with src + dst rect",
          "[view][bridge][canvas][issue-916]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'img-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        // 5-arg form — most common for plugin icons.
        ctx.drawImage({ src: '/path/to/icon.png', width: 16, height: 16 },
                      10, 20, 64, 32);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "img-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    int drawIndex = -1;
    int idx = 0;
    for (auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::draw_image) {
            drawIndex = idx;
            REQUIRE(cmd.text == "/path/to/icon.png");
            REQUIRE_THAT(cmd.f[0], WithinAbs(10.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[1], WithinAbs(20.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[2], WithinAbs(64.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[3], WithinAbs(32.0f, 1e-5f));
            // 5-arg form leaves the floats payload empty — has_source_rect
            // is false, so the renderer routes through the dst-only path.
            REQUIRE(cmd.floats.empty());
        }
        ++idx;
    }
    REQUIRE(drawIndex >= 0);
}

// drawImage(img, sx,sy,sw,sh, dx,dy,dw,dh) sprite-sheet slicing form.
// Pre-fix, the JS shim accepted the 9-arg signature but
// silently dropped the source rect — only the dst rect made it across
// the bridge, so a sprite-sheet `drawImage(strip, 32,0,32,32, 0,0,32,32)`
// drew the entire strip scaled into the 32×32 dst tile. Post-fix, the
// JS shim passes sx/sy/sw/sh as args[6..9], the bridge plumbs them via
// a `has_source_rect` flag, and the canvas widget routes through
// `Canvas::draw_image_from_file_rect` so the source sub-rectangle maps
// onto the destination rect.
TEST_CASE("WidgetBridge canvasDrawImage 9-arg form plumbs source rect end-to-end",
          "[view][bridge][canvas][issue-916][issue-1737]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'sprite-canvas';
        c.width = 200; c.height = 100;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        // 9-arg form: slice the (32, 0)→(64, 32) sub-rect of the
        // sprite-strip and paint it into (10, 20)→(74, 52).
        ctx.drawImage({ src: '/sprites/walk.png', width: 256, height: 32 },
                      32, 0, 32, 32,
                      10, 20, 64, 32);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "sprite-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    int drawIndex = -1;
    int idx = 0;
    for (auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::draw_image) {
            drawIndex = idx;
            REQUIRE(cmd.text == "/sprites/walk.png");
            // dst rect in f[0..3]
            REQUIRE_THAT(cmd.f[0], WithinAbs(10.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[1], WithinAbs(20.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[2], WithinAbs(64.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[3], WithinAbs(32.0f, 1e-5f));
            // src rect in floats[0..3] — proves the 9-arg form routed
            // through draw_image_from_file_rect, not the dst-only fallback.
            REQUIRE(cmd.floats.size() == 4);
            REQUIRE_THAT(cmd.floats[0], WithinAbs(32.0f, 1e-5f));
            REQUIRE_THAT(cmd.floats[1], WithinAbs(0.0f, 1e-5f));
            REQUIRE_THAT(cmd.floats[2], WithinAbs(32.0f, 1e-5f));
            REQUIRE_THAT(cmd.floats[3], WithinAbs(32.0f, 1e-5f));
        }
        ++idx;
    }
    REQUIRE(drawIndex >= 0);
}

// Focused unit test for the bridge-side 9-arg drawImage plumbing. The
// end-to-end test above exercises the same path through the JS shim; this test
// invokes canvasDrawImage directly via JS and asserts the CanvasDrawCmd state
// right at the JS-→-bridge boundary (read back via CanvasWidget::commands()).
TEST_CASE("WidgetBridge canvasDrawImage 10-arg call sets has_source_rect on the recorded cmd",
          "[view][bridge][canvas][issue-1739][canvas2d]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // Drive canvasDrawImage directly with the 10-arg shape so the
    // `args.numArgs >= 10` branch in widget_bridge.cpp fires. Using the
    // raw bridge function (not the JS shim) keeps the call site tightly bound
    // to the branch under test.
    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'bridge-9arg-canvas';
        c.width = 256; c.height = 64;
        document.body.appendChild(c);
        // 10 args: canvasId, src, dx, dy, dw, dh, sx, sy, sw, sh.
        // Bridge stashes sx,sy,sw,sh in x2,y2,x3,y3 and flags
        // has_source_rect=true. Use c._id — the bridge looks up the
        // CanvasWidget by its internal id, not the DOM `id` attribute.
        canvasDrawImage(c._id, '/atlas/strip.png',
                        100, 50, 64, 32,
                        128, 16, 32, 16);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "bridge-9arg-canvas");
    REQUIRE(canvas != nullptr);
    REQUIRE(canvas->command_count() == 1);

    // CanvasWidget::commands() exposes the recorded cmd queue (read-only).
    // We assert directly on the CanvasDrawCmd to pin the bridge's
    // x2/y2/x3/y3 + has_source_rect writes without an intervening paint-replay
    // layer.
    const auto& cmds = canvas->commands();
    REQUIRE(cmds.size() == 1);
    const auto& cmd = cmds.front();
    REQUIRE(cmd.type == CanvasDrawCmd::Type::draw_image);
    REQUIRE(cmd.text == "/atlas/strip.png");
    // dst rect (args[2..5])
    REQUIRE_THAT(cmd.x, WithinAbs(100.0f, 1e-5f));
    REQUIRE_THAT(cmd.y, WithinAbs(50.0f, 1e-5f));
    REQUIRE_THAT(cmd.w, WithinAbs(64.0f, 1e-5f));
    REQUIRE_THAT(cmd.h, WithinAbs(32.0f, 1e-5f));
    // src rect (args[6..9]) — proves the args.numArgs >= 10 branch fired.
    REQUIRE(cmd.has_source_rect == true);
    REQUIRE_THAT(cmd.x2, WithinAbs(128.0f, 1e-5f));
    REQUIRE_THAT(cmd.y2, WithinAbs(16.0f, 1e-5f));
    REQUIRE_THAT(cmd.x3, WithinAbs(32.0f, 1e-5f));
    REQUIRE_THAT(cmd.y3, WithinAbs(16.0f, 1e-5f));
}

// 6-arg form (canvas-only-dst): the bridge's `args.numArgs >= 10`
// branch must NOT fire, so has_source_rect stays false. Pins the
// else-branch of the bridge plumbing.
TEST_CASE("WidgetBridge canvasDrawImage 6-arg call leaves has_source_rect=false",
          "[view][bridge][canvas][issue-1739][canvas2d]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'bridge-6arg-canvas';
        c.width = 64; c.height = 64;
        document.body.appendChild(c);
        // 6 args — dst-only form. has_source_rect must stay false.
        canvasDrawImage(c._id, '/icons/save.png',
                        4, 4, 32, 32);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "bridge-6arg-canvas");
    REQUIRE(canvas != nullptr);
    REQUIRE(canvas->command_count() == 1);

    const auto& cmd = canvas->commands().front();
    REQUIRE(cmd.type == CanvasDrawCmd::Type::draw_image);
    REQUIRE(cmd.text == "/icons/save.png");
    REQUIRE_THAT(cmd.x, WithinAbs(4.0f, 1e-5f));
    REQUIRE_THAT(cmd.y, WithinAbs(4.0f, 1e-5f));
    REQUIRE_THAT(cmd.w, WithinAbs(32.0f, 1e-5f));
    REQUIRE_THAT(cmd.h, WithinAbs(32.0f, 1e-5f));
    // Source-rect branch did NOT fire.
    REQUIRE(cmd.has_source_rect == false);
    REQUIRE_THAT(cmd.x2, WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(cmd.y2, WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(cmd.x3, WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(cmd.y3, WithinAbs(0.0f, 1e-5f));
}

// ── 4-arg canvasFillText preserves prior state ─────────────────────────────
//
// The original 4-arg shim (#1899) unconditionally injected a `set_font`
// (system-ui 14px) ahead of every fillText cmd AND overwrote
// `cmd.color` to white. That stomped any prior `fillStyle = "..."` or
// `font = "..."` set by the caller. The fix gates each default on whether any
// prior fill-style / font-state cmd has been recorded on the canvas; if so, we
// propagate that state into the fill_text cmd rather than reset to defaults.

TEST_CASE("WidgetBridge 4-arg canvasFillText preserves prior fillStyle color",
          "[view][bridge][canvas][issue-1901][canvas2d]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // Drive the bridge directly so the assertions pin the
    // widget_bridge.cpp 4-arg branch without an intermediary JS shim.
    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'bridge-4arg-fill-color';
        c.width = 128; c.height = 64;
        document.body.appendChild(c);
        // Establish a fillStyle first, then call the 4-arg fillText
        // form: canvasFillText(id, x, y, text).
        canvasSetFillColor(c._id, 'red');
        canvasFillText(c._id, 10, 20, 'hello');
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "bridge-4arg-fill-color");
    REQUIRE(canvas != nullptr);

    // Expected recorded stream: set_fill_color(red), set_font(default),
    // fill_text(...). The set_font is the still-needed default-font
    // injection (no canvasSetFont was issued). The fill_text cmd's
    // color MUST be red — not the hard-coded #fff that the original
    // 4-arg shim wrote unconditionally.
    const auto& cmds = canvas->commands();
    REQUIRE(cmds.size() == 3);
    REQUIRE(cmds[0].type == CanvasDrawCmd::Type::set_fill_color);
    REQUIRE(cmds[1].type == CanvasDrawCmd::Type::set_font);
    REQUIRE(cmds[2].type == CanvasDrawCmd::Type::fill_text);
    REQUIRE(cmds[2].text == "hello");
    // red = #FF0000 → r=1.0, g=0.0, b=0.0. Channel-wise compare.
    REQUIRE_THAT(cmds[2].color.r, WithinAbs(1.0f, 1e-5f));
    REQUIRE_THAT(cmds[2].color.g, WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(cmds[2].color.b, WithinAbs(0.0f, 1e-5f));
}

TEST_CASE("WidgetBridge 4-arg canvasFillText preserves prior set_font state",
          "[view][bridge][canvas][issue-1901][canvas2d]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'bridge-4arg-fill-font';
        c.width = 128; c.height = 64;
        document.body.appendChild(c);
        // Establish a font first, then call the 4-arg fillText form.
        // The 4-arg shim previously injected a `set_font system-ui 14px`
        // ahead of every call — stomping the prior serif 20px state.
        canvasSetFont(c._id, 'serif', 20);
        canvasFillText(c._id, 5, 15, 'world');
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "bridge-4arg-fill-font");
    REQUIRE(canvas != nullptr);

    // Expected stream: set_font(serif 20), fill_text(...). The default
    // set_font MUST NOT be injected because a prior set_font already
    // established the canvas's font state.
    const auto& cmds = canvas->commands();
    REQUIRE(cmds.size() == 2);
    REQUIRE(cmds[0].type == CanvasDrawCmd::Type::set_font);
    REQUIRE(cmds[0].text == "serif");
    REQUIRE_THAT(cmds[0].extra, WithinAbs(20.0f, 1e-5f));
    REQUIRE(cmds[1].type == CanvasDrawCmd::Type::fill_text);
    REQUIRE(cmds[1].text == "world");
}

TEST_CASE("WidgetBridge canvasPutImageData records pixel buffer for paint replay",
          "[view][bridge][canvas][issue-916]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // 2x2 RGBA — bright red, green, blue, white. Round-trip through
    // putImageData → bridge base64 decode → write_pixels command.
    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'put-canvas';
        c.width = 4; c.height = 4;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var img = {
            width: 2, height: 2,
            data: new Uint8ClampedArray([
                255,   0,   0, 255,
                  0, 255,   0, 255,
                  0,   0, 255, 255,
                255, 255, 255, 255
            ])
        };
        ctx.putImageData(img, 1, 1);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "put-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    int writeIndex = -1;
    int idx = 0;
    for (auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::write_pixels) {
            writeIndex = idx;
            REQUIRE(static_cast<int>(cmd.f[0]) == 2); // width
            REQUIRE(static_cast<int>(cmd.f[1]) == 2); // height
            REQUIRE(static_cast<int>(cmd.f[2]) == 1); // dx
            REQUIRE(static_cast<int>(cmd.f[3]) == 1); // dy
            REQUIRE(cmd.text.size() == 16);            // 2*2*4 RGBA bytes
            // First pixel — opaque red.
            REQUIRE(static_cast<unsigned char>(cmd.text[0]) == 255);
            REQUIRE(static_cast<unsigned char>(cmd.text[1]) == 0);
            REQUIRE(static_cast<unsigned char>(cmd.text[2]) == 0);
            REQUIRE(static_cast<unsigned char>(cmd.text[3]) == 255);
        }
        ++idx;
    }
    REQUIRE(writeIndex >= 0);
}

// putImageData(imageData, dx, dy, dirtyX, dirtyY, dirtyW, dirtyH) sub-rect
// form. Pre-fix the JS shim accepted the 7-arg signature but silently dropped
// dirtyX/Y/W/H and wrote the entire ImageData. Per HTML5 spec only the
// (dirtyX, dirtyY)→(dirtyX+dirtyW, dirtyY+dirtyH) sub-rect of the source
// ImageData is written, at destination top-left (dx + dirtyX, dy + dirtyY).
//
// Test drives a 4×4 ImageData with a recognisable color pattern and
// asks the shim to write only the 2×2 bottom-right sub-rect at
// (dx=10, dy=20). The recorded write_pixels command must have:
//   * width  == 2 (sliced to dirtyW)
//   * height == 2 (sliced to dirtyH)
//   * dx     == 10 + 2 == 12 (dx + dirtyX)
//   * dy     == 20 + 2 == 22 (dy + dirtyY)
//   * 16 RGBA bytes carrying the bottom-right 2×2 colors.
TEST_CASE("WidgetBridge canvasPutImageData 7-arg sub-rect slices on the JS side",
          "[view][bridge][canvas][issue-916][issue-1737]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // 4x4 RGBA: every pixel is color-coded so the slice can be verified
    // unambiguously. Color scheme: R = column*64, G = row*64, B = 128.
    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'put-rect-canvas';
        c.width = 32; c.height = 32;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var bytes = new Uint8ClampedArray(4*4*4);
        for (var row = 0; row < 4; ++row) {
            for (var col = 0; col < 4; ++col) {
                var off = (row * 4 + col) * 4;
                bytes[off+0] = col * 64;   // R
                bytes[off+1] = row * 64;   // G
                bytes[off+2] = 128;        // B
                bytes[off+3] = 255;        // A
            }
        }
        var img = { width: 4, height: 4, data: bytes };
        // 7-arg form: only the (sx=2, sy=2, sw=2, sh=2) sub-rect goes
        // to (dx + sx, dy + sy) = (10 + 2, 20 + 2) = (12, 22).
        ctx.putImageData(img, 10, 20, 2, 2, 2, 2);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "put-rect-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    int writeIndex = -1;
    int idx = 0;
    for (auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::write_pixels) {
            writeIndex = idx;
            REQUIRE(static_cast<int>(cmd.f[0]) == 2);  // sliced width
            REQUIRE(static_cast<int>(cmd.f[1]) == 2);  // sliced height
            REQUIRE(static_cast<int>(cmd.f[2]) == 12); // dx + dirtyX
            REQUIRE(static_cast<int>(cmd.f[3]) == 22); // dy + dirtyY
            REQUIRE(cmd.text.size() == 16);             // 2*2*4 RGBA bytes
            // First pixel of the slice = source (col=2, row=2) →
            //   R=128, G=128, B=128, A=255
            REQUIRE(static_cast<unsigned char>(cmd.text[0]) == 128);
            REQUIRE(static_cast<unsigned char>(cmd.text[1]) == 128);
            REQUIRE(static_cast<unsigned char>(cmd.text[2]) == 128);
            REQUIRE(static_cast<unsigned char>(cmd.text[3]) == 255);
            // Second pixel of the slice = source (col=3, row=2) →
            //   R=192, G=128, B=128, A=255
            REQUIRE(static_cast<unsigned char>(cmd.text[4]) == 192);
            REQUIRE(static_cast<unsigned char>(cmd.text[5]) == 128);
            REQUIRE(static_cast<unsigned char>(cmd.text[6]) == 128);
            REQUIRE(static_cast<unsigned char>(cmd.text[7]) == 255);
            // Fourth pixel = source (col=3, row=3) → R=192, G=192, B=128
            REQUIRE(static_cast<unsigned char>(cmd.text[12]) == 192);
            REQUIRE(static_cast<unsigned char>(cmd.text[13]) == 192);
            REQUIRE(static_cast<unsigned char>(cmd.text[14]) == 128);
            REQUIRE(static_cast<unsigned char>(cmd.text[15]) == 255);
        }
        ++idx;
    }
    REQUIRE(writeIndex >= 0);
}

// putImageData with an empty dirty rect (e.g. dirtyW <= 0 after clamping) is a
// no-op per HTML5 spec. Pre-fix the JS shim dropped the dirty args entirely, so
// an empty dirty rect would still blast the whole ImageData onto the canvas.
// Post-fix the shim recognizes the empty case and bails before encoding/sending.
TEST_CASE("WidgetBridge canvasPutImageData 7-arg empty dirty rect is a no-op",
          "[view][bridge][canvas][issue-916][issue-1737]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'put-empty-canvas';
        c.width = 32; c.height = 32;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var img = {
            width: 4, height: 4,
            data: new Uint8ClampedArray(4*4*4)
        };
        // dirtyW=0 means the dirty rect is empty — must be a no-op.
        ctx.putImageData(img, 0, 0, 0, 0, 0, 4);
    )");
    root.layout_children();

    auto* canvas = canvasFromBridge(bridge, engine, "put-empty-canvas");
    REQUIRE(canvas != nullptr);

    pulp::canvas::RecordingCanvas rec;
    canvas->paint(rec);

    bool any_write = false;
    for (auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::write_pixels) {
            any_write = true;
        }
    }
    REQUIRE_FALSE(any_write);
}

TEST_CASE("WidgetBridge canvasGetImageData returns a TextMetrics-like object "
          "with width/height/data even when no surface is rasterized",
          "[view][bridge][canvas][issue-916]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    root.set_theme(Theme::dark());
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        var c = document.createElement('canvas');
        c.id = 'get-canvas';
        c.width = 64; c.height = 64;
        document.body.appendChild(c);
        var ctx = c.getContext('2d');
        var img = ctx.getImageData(0, 0, 4, 4);
        window.__got = img;
    )");

    auto width  = engine.evaluate("window.__got.width");
    auto height = engine.evaluate("window.__got.height");
    auto length = engine.evaluate("window.__got.data.length");

    REQUIRE(width.getWithDefault<double>(-1.0)  == 4);
    REQUIRE(height.getWithDefault<double>(-1.0) == 4);
    // 4*4*4 == 64 bytes for the RGBA array.
    REQUIRE(length.getWithDefault<double>(-1.0) == 64);
}
