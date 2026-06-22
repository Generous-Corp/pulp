// Inspector GPU render-pass attribution tests.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/render/render_pass.hpp>
#include <pulp/view/inspector.hpp>

#include <string_view>
#include <tuple>
#include <vector>

using namespace pulp::view;
using namespace pulp::inspect;

namespace {

using pulp::render::RenderPassManager;
using pulp::render::RenderPassType;

// Drive one synthetic render frame through the manager: begin a frame,
// emit each (type, time_ms, draw_calls) pass, end the frame.
void render_synthetic_frame(
    RenderPassManager& rpm,
    const std::vector<std::tuple<RenderPassType, float, int>>& passes) {
    rpm.begin_frame();
    for (auto& [type, ms, dc] : passes) {
        rpm.begin_pass(type);
        rpm.end_pass(ms, dc);
    }
    rpm.end_frame();
}

// Count RecordingCanvas fill_text commands whose text contains `needle`.
int count_text_containing(const pulp::canvas::RecordingCanvas& canvas,
                          std::string_view needle) {
    int n = 0;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_text &&
            cmd.text.find(needle) != std::string::npos)
            ++n;
    }
    return n;
}

} // namespace

TEST_CASE("InspectorOverlay Phase 6.1: capture_pass_frame is a no-op without an RPM",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);

    REQUIRE_FALSE(overlay.capture_pass_frame());
    REQUIRE(overlay.pass_frames_captured() == 0);
    // Every pass entry reports zero history.
    for (const auto& a : overlay.pass_attribution())
        REQUIRE(a.samples == 0);
}

TEST_CASE("InspectorOverlay Phase 6.1: capture accumulates per-pass history",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);

    RenderPassManager rpm;
    overlay.set_render_pass_manager(&rpm);

    // Frame 1: background + content passes.
    render_synthetic_frame(rpm, {
        {RenderPassType::background, 1.0f, 3},
        {RenderPassType::content,    4.0f, 12},
    });
    REQUIRE(overlay.capture_pass_frame());
    REQUIRE(overlay.pass_frames_captured() == 1);

    // Frame 2: content gets heavier, an overlay pass appears.
    render_synthetic_frame(rpm, {
        {RenderPassType::background, 1.0f, 3},
        {RenderPassType::content,    8.0f, 20},
        {RenderPassType::overlay,    2.0f, 5},
    });
    REQUIRE(overlay.capture_pass_frame());
    REQUIRE(overlay.pass_frames_captured() == 2);

    auto attrib = overlay.pass_attribution();
    REQUIRE(attrib.size() == 5);  // one entry per RenderPassType.

    // background: two samples, steady at 1.0ms.
    const auto& bg = attrib[static_cast<int>(RenderPassType::background)];
    REQUIRE(bg.samples == 2);
    REQUIRE(bg.present);
    REQUIRE_THAT(bg.last_cpu_ms, Catch::Matchers::WithinAbs(1.0, 0.001));
    REQUIRE_THAT(bg.avg_cpu_ms, Catch::Matchers::WithinAbs(1.0, 0.001));
    REQUIRE(bg.last_draw_calls == 3);

    // content: avg of 4 and 8 = 6, peak 8, last 8.
    const auto& content = attrib[static_cast<int>(RenderPassType::content)];
    REQUIRE(content.samples == 2);
    REQUIRE_THAT(content.last_cpu_ms, Catch::Matchers::WithinAbs(8.0, 0.001));
    REQUIRE_THAT(content.avg_cpu_ms, Catch::Matchers::WithinAbs(6.0, 0.001));
    REQUIRE_THAT(content.peak_cpu_ms, Catch::Matchers::WithinAbs(8.0, 0.001));
    REQUIRE(content.peak_draw_calls == 20);

    // overlay: only one sample (absent from frame 1).
    const auto& overlay_pass = attrib[static_cast<int>(RenderPassType::overlay)];
    REQUIRE(overlay_pass.samples == 1);
    REQUIRE(overlay_pass.present);

    // effects + post never rendered — no history, not present.
    const auto& effects = attrib[static_cast<int>(RenderPassType::effects)];
    REQUIRE(effects.samples == 0);
    REQUIRE_FALSE(effects.present);
}

TEST_CASE("InspectorOverlay Phase 6.1: capture de-dups within a single frame",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);

    RenderPassManager rpm;
    overlay.set_render_pass_manager(&rpm);

    render_synthetic_frame(rpm, {{RenderPassType::content, 5.0f, 10}});

    // First capture records the frame; a second capture of the SAME
    // frame (no begin_frame between) must be a no-op so paint() running
    // multiple times per frame doesn't inflate history.
    REQUIRE(overlay.capture_pass_frame());
    REQUIRE_FALSE(overlay.capture_pass_frame());
    REQUIRE(overlay.pass_frames_captured() == 1);

    auto attrib = overlay.pass_attribution();
    REQUIRE(attrib[static_cast<int>(RenderPassType::content)].samples == 1);
}

TEST_CASE("InspectorOverlay Phase 6.1: same pass type twice in a frame sums",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);

    RenderPassManager rpm;
    overlay.set_render_pass_manager(&rpm);

    // Two overlay passes in one frame — the history sample is the
    // frame's TOTAL overlay cost (1.5 + 0.5 = 2.0ms, 4 + 2 = 6 draws).
    render_synthetic_frame(rpm, {
        {RenderPassType::overlay, 1.5f, 4},
        {RenderPassType::overlay, 0.5f, 2},
    });
    REQUIRE(overlay.capture_pass_frame());

    auto attrib = overlay.pass_attribution();
    const auto& ov = attrib[static_cast<int>(RenderPassType::overlay)];
    REQUIRE(ov.samples == 1);
    REQUIRE_THAT(ov.last_cpu_ms, Catch::Matchers::WithinAbs(2.0, 0.001));
    REQUIRE(ov.last_draw_calls == 6);
}

TEST_CASE("InspectorOverlay Phase 6.1: history ring caps at kPassHistoryFrames",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);

    RenderPassManager rpm;
    overlay.set_render_pass_manager(&rpm);

    // Push more frames than the ring holds; only the last N count
    // toward per-pass detail, but the lifetime counter keeps growing.
    const std::size_t extra = 25;
    const std::size_t total = InspectorOverlay::kPassHistoryFrames + extra;
    for (std::size_t i = 0; i < total; ++i) {
        render_synthetic_frame(rpm, {{RenderPassType::content,
                                      static_cast<float>(i % 7), 1}});
        REQUIRE(overlay.capture_pass_frame());
    }
    REQUIRE(overlay.pass_frames_captured() == total);

    auto attrib = overlay.pass_attribution();
    const auto& content = attrib[static_cast<int>(RenderPassType::content)];
    REQUIRE(content.samples == InspectorOverlay::kPassHistoryFrames);
}

TEST_CASE("InspectorOverlay Phase 6.1: budget overruns are counted",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);

    RenderPassManager rpm;
    rpm.set_budget(5.0f);  // tight 5ms budget.
    overlay.set_render_pass_manager(&rpm);

    // Frame 1: under budget (3ms).
    render_synthetic_frame(rpm, {{RenderPassType::content, 3.0f, 4}});
    REQUIRE(overlay.capture_pass_frame());
    REQUIRE(overlay.budget_overrun_count() == 0);

    // Frame 2: over budget (9ms).
    render_synthetic_frame(rpm, {{RenderPassType::content, 9.0f, 4}});
    REQUIRE(overlay.capture_pass_frame());
    REQUIRE(overlay.budget_overrun_count() == 1);

    // Frame 3: over budget again (12ms).
    render_synthetic_frame(rpm, {{RenderPassType::content, 12.0f, 4}});
    REQUIRE(overlay.capture_pass_frame());
    REQUIRE(overlay.budget_overrun_count() == 2);
}

TEST_CASE("InspectorOverlay Phase 6.1: P key toggles the attribution viewer",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);
    overlay.set_active(true);

    REQUIRE_FALSE(overlay.pass_viewer_enabled());

    KeyEvent p;
    p.key = KeyCode::p;
    p.is_down = true;
    p.modifiers = 0;
    REQUIRE(overlay.handle_key_event(p));
    REQUIRE(overlay.pass_viewer_enabled());

    REQUIRE(overlay.handle_key_event(p));
    REQUIRE_FALSE(overlay.pass_viewer_enabled());
}

TEST_CASE("InspectorOverlay Phase 6.1: P key ignored when inspector inactive",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);
    // Not active — the P hotkey must not fire so it can't collide with
    // a plain text field in the host UI.

    KeyEvent p;
    p.key = KeyCode::p;
    p.is_down = true;
    p.modifiers = 0;
    REQUIRE_FALSE(overlay.handle_key_event(p));
    REQUIRE_FALSE(overlay.pass_viewer_enabled());
}

TEST_CASE("InspectorOverlay Phase 6.1: viewer renders per-pass rows",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_id("root");
    // Tall enough that the panel's lower section fits both pass rows
    // (heading + summary + two ~56px rows). The lower section gets
    // roughly half the window height minus the stats bar.
    root.set_bounds({0, 0, 500, 720});
    InspectorOverlay overlay(root);
    overlay.set_active(true);

    RenderPassManager rpm;
    overlay.set_render_pass_manager(&rpm);

    // Drive a few frames with two distinct pass types.
    for (int i = 0; i < 3; ++i) {
        render_synthetic_frame(rpm, {
            {RenderPassType::background, 1.0f, 2},
            {RenderPassType::content,    5.0f + static_cast<float>(i), 14},
        });
        // paint() captures the frame; toggle viewer on first.
        if (i == 0) overlay.set_pass_viewer_enabled(true);
        pulp::canvas::RecordingCanvas warmup;
        overlay.paint(warmup);
    }

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(canvas.command_count() > 0);

    // The viewer heading and both rendered pass names must appear.
    REQUIRE(count_text_containing(canvas, "Render Passes") >= 1);
    REQUIRE(count_text_containing(canvas, "background") >= 1);
    REQUIRE(count_text_containing(canvas, "content") >= 1);
    // Honesty note about CPU vs GPU timing is surfaced.
    REQUIRE(count_text_containing(canvas, "wall-clock, not GPU") >= 1);
    // "effects" never rendered — must NOT show a row for it.
    REQUIRE(count_text_containing(canvas, "effects") == 0);
}

TEST_CASE("InspectorOverlay Phase 6.1: viewer reports missing RPM gracefully",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 500, 320});
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_pass_viewer_enabled(true);
    // No RenderPassManager attached.

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(count_text_containing(canvas, "wall-clock, not GPU") >= 1);
    REQUIRE(count_text_containing(canvas, "No RenderPassManager") >= 1);
}

TEST_CASE("InspectorOverlay Phase 6.1: paint() drives capture automatically",
          "[inspect][overlay][phase6]") {
    View root;
    root.set_bounds({0, 0, 500, 320});
    InspectorOverlay overlay(root);
    overlay.set_active(true);

    RenderPassManager rpm;
    overlay.set_render_pass_manager(&rpm);

    render_synthetic_frame(rpm, {{RenderPassType::content, 4.0f, 8}});

    pulp::canvas::RecordingCanvas canvas;
    overlay.paint(canvas);  // should capture the frame internally.
    REQUIRE(overlay.pass_frames_captured() == 1);

    // A second paint of the SAME frame does not re-capture.
    overlay.paint(canvas);
    REQUIRE(overlay.pass_frames_captured() == 1);

    // A new frame, then paint, captures again.
    render_synthetic_frame(rpm, {{RenderPassType::content, 6.0f, 9}});
    overlay.paint(canvas);
    REQUIRE(overlay.pass_frames_captured() == 2);
}
