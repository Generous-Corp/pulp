#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/vellum_components/inspector_overlay.hpp>
#include <pulp/vellum_components/render_pass_stats.hpp>

#include <vellum/canvas/recording_canvas.hpp>
#include <vellum/render/render_pass.hpp>
#include <vellum/view/view.hpp>

#include "render_helpers.hpp"

#include <string_view>
#include <tuple>
#include <vector>

using namespace pulp::vellum_components;
using namespace pulp::vellum_components::testing;
using Catch::Matchers::WithinAbs;
using ::vellum::canvas::DrawCommand;
using ::vellum::canvas::RecordingCanvas;
using ::vellum::render::RenderPassManager;
using ::vellum::render::RenderPassType;
using ::vellum::view::KeyCode;
using ::vellum::view::KeyEvent;

namespace {

constexpr auto kBackground = static_cast<std::size_t>(RenderPassType::background);
constexpr auto kContent = static_cast<std::size_t>(RenderPassType::content);
constexpr auto kEffects = static_cast<std::size_t>(RenderPassType::effects);
constexpr auto kOverlayPass = static_cast<std::size_t>(RenderPassType::overlay);

// One synthetic frame through the manager: begin, each (type, ms, draws), end.
void render_synthetic_frame(RenderPassManager& manager,
                            const std::vector<std::tuple<RenderPassType, float, int>>& passes) {
    manager.begin_frame();
    for (const auto& [type, ms, draws] : passes) {
        manager.begin_pass(type);
        manager.end_pass(ms, draws);
    }
    manager.end_frame();
}

// A source with no manager behind it, so a test controls every number.
class FakeSource : public overlay::RenderPassSource {
  public:
    std::uint64_t frame = 0;
    bool over = false;
    float total = 0.0f;
    float budget = 16.67f;
    std::vector<overlay::PassSample> samples;

    std::uint64_t frame_count() const override {
        return frame;
    }
    bool over_budget() const override {
        return over;
    }
    float total_time_ms() const override {
        return total;
    }
    float budget_ms() const override {
        return budget;
    }
    std::vector<overlay::PassSample> passes() const override {
        return samples;
    }
};

int count_text_containing(const RecordingCanvas& canvas, std::string_view needle) {
    int n = 0;
    for (const auto& cmd : canvas.commands())
        if (cmd.type == DrawCommand::Type::fill_text && cmd.text.find(needle) != std::string::npos)
            ++n;
    return n;
}

KeyEvent key(KeyCode code, std::uint16_t modifiers = 0) {
    KeyEvent e;
    e.key = code;
    e.modifiers = modifiers;
    e.is_down = true;
    return e;
}

// An overlay over a root, reading a real manager through the adapter.
struct PassScene {
    View root;
    RenderPassManager manager;
    overlay::ManagerPassSource source{manager};
    InspectorOverlay overlay{root};
    explicit PassScene(float w = 500, float h = 320) {
        root.set_bounds({0, 0, w, h});
        overlay.set_active(true);
        overlay.set_render_pass_source(&source);
    }
};

} // namespace

TEST_CASE("capture is a no-op without a source", "[pulp-vellum-components][render-pass]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);
    REQUIRE_FALSE(overlay.capture_pass_frame());
    REQUIRE(overlay.pass_frames_captured() == 0);
    for (const auto& a : overlay.pass_attribution())
        REQUIRE(a.samples == 0);
}

TEST_CASE("capture accumulates per-pass history", "[pulp-vellum-components][render-pass]") {
    PassScene s;
    render_synthetic_frame(s.manager, {
                                          {RenderPassType::background, 1.0f, 3},
                                          {RenderPassType::content, 4.0f, 12},
                                      });
    REQUIRE(s.overlay.capture_pass_frame());
    REQUIRE(s.overlay.pass_frames_captured() == 1);

    render_synthetic_frame(s.manager, {
                                          {RenderPassType::background, 1.0f, 3},
                                          {RenderPassType::content, 8.0f, 20},
                                          {RenderPassType::overlay, 2.0f, 5},
                                      });
    REQUIRE(s.overlay.capture_pass_frame());
    REQUIRE(s.overlay.pass_frames_captured() == 2);

    const auto attrib = s.overlay.pass_attribution();
    REQUIRE(attrib.size() == 5);

    const auto& bg = attrib[kBackground];
    REQUIRE(bg.samples == 2);
    REQUIRE(bg.present);
    REQUIRE_THAT(bg.last_cpu_ms, WithinAbs(1.0, 0.001));
    REQUIRE_THAT(bg.avg_cpu_ms, WithinAbs(1.0, 0.001));
    REQUIRE(bg.last_draw_calls == 3);

    // content: mean of 4 and 8, peak 8, last 8.
    const auto& content = attrib[kContent];
    REQUIRE(content.samples == 2);
    REQUIRE_THAT(content.last_cpu_ms, WithinAbs(8.0, 0.001));
    REQUIRE_THAT(content.avg_cpu_ms, WithinAbs(6.0, 0.001));
    REQUIRE_THAT(content.peak_cpu_ms, WithinAbs(8.0, 0.001));
    REQUIRE(content.peak_draw_calls == 20);

    // overlay ran in the second frame only.
    REQUIRE(attrib[kOverlayPass].samples == 1);
    REQUIRE(attrib[kOverlayPass].present);

    // effects and post never ran.
    REQUIRE(attrib[kEffects].samples == 0);
    REQUIRE_FALSE(attrib[kEffects].present);
}

TEST_CASE("a pass that skips the latest frame is no longer present",
          "[pulp-vellum-components][render-pass]") {
    PassScene s;
    render_synthetic_frame(s.manager, {{RenderPassType::overlay, 2.0f, 5}});
    s.overlay.capture_pass_frame();
    render_synthetic_frame(s.manager, {{RenderPassType::content, 2.0f, 5}});
    s.overlay.capture_pass_frame();
    const auto attrib = s.overlay.pass_attribution();
    REQUIRE(attrib[kOverlayPass].samples == 1);
    REQUIRE_FALSE(attrib[kOverlayPass].present);
    REQUIRE(attrib[kContent].present);
}

TEST_CASE("a frame is captured once however often it is offered",
          "[pulp-vellum-components][render-pass]") {
    PassScene s;
    render_synthetic_frame(s.manager, {{RenderPassType::content, 5.0f, 10}});
    REQUIRE(s.overlay.capture_pass_frame());
    REQUIRE_FALSE(s.overlay.capture_pass_frame());
    REQUIRE(s.overlay.pass_frames_captured() == 1);
    REQUIRE(s.overlay.pass_attribution()[kContent].samples == 1);
}

TEST_CASE("a source that has run no frame records nothing",
          "[pulp-vellum-components][render-pass]") {
    PassScene s;
    REQUIRE_FALSE(s.overlay.capture_pass_frame());
    REQUIRE(s.overlay.pass_frames_captured() == 0);
}

TEST_CASE("the same pass type twice in a frame records its sum",
          "[pulp-vellum-components][render-pass]") {
    PassScene s;
    render_synthetic_frame(s.manager, {
                                          {RenderPassType::overlay, 1.5f, 4},
                                          {RenderPassType::overlay, 0.5f, 2},
                                      });
    REQUIRE(s.overlay.capture_pass_frame());
    const auto attrib = s.overlay.pass_attribution();
    const auto& pass = attrib[kOverlayPass];
    REQUIRE(pass.samples == 1);
    REQUIRE_THAT(pass.last_cpu_ms, WithinAbs(2.0, 0.001));
    REQUIRE(pass.last_draw_calls == 6);
}

TEST_CASE("the history keeps the last kPassHistoryFrames and the counter keeps counting",
          "[pulp-vellum-components][render-pass]") {
    PassScene s;
    const std::size_t total = overlay::kPassHistoryFrames + 25;
    for (std::size_t i = 0; i < total; ++i) {
        render_synthetic_frame(s.manager,
                               {{RenderPassType::content, static_cast<float>(i % 7), 1}});
        REQUIRE(s.overlay.capture_pass_frame());
    }
    REQUIRE(s.overlay.pass_frames_captured() == total);
    REQUIRE(s.overlay.pass_attribution()[kContent].samples == overlay::kPassHistoryFrames);
}

TEST_CASE("the history reads oldest first across the ring wrap",
          "[pulp-vellum-components][render-pass]") {
    FakeSource source;
    overlay::PassHistory history;
    const std::size_t total = overlay::kPassHistoryFrames + 5;
    for (std::size_t i = 0; i < total; ++i) {
        source.frame = i + 1;
        source.samples = {{kContent, 1, static_cast<float>(i)}};
        REQUIRE(history.capture(source));
    }
    const auto series = history.cpu_ms_history(kContent);
    REQUIRE(series.size() == overlay::kPassHistoryFrames);
    // The five oldest samples have been overwritten: 5 .. total-1 remain.
    REQUIRE_THAT(series.front(), WithinAbs(5.0, 0.001));
    REQUIRE_THAT(series.back(), WithinAbs(static_cast<double>(total - 1), 0.001));
    for (std::size_t k = 1; k < series.size(); ++k)
        REQUIRE(series[k] > series[k - 1]);
    REQUIRE(history.cpu_ms_history(kEffects).empty());
    REQUIRE(history.cpu_ms_history(99).empty());
}

TEST_CASE("a history short of the ring reads oldest first from its start",
          "[pulp-vellum-components][render-pass]") {
    FakeSource source;
    overlay::PassHistory history;
    for (std::size_t i = 1; i <= 3; ++i) {
        source.frame = i;
        source.samples = {{kContent, 1, static_cast<float>(i)}};
        history.capture(source);
    }
    const auto series = history.cpu_ms_history(kContent);
    REQUIRE(series.size() == 3);
    REQUIRE_THAT(series[0], WithinAbs(1.0, 0.001));
    REQUIRE_THAT(series[1], WithinAbs(2.0, 0.001));
    REQUIRE_THAT(series[2], WithinAbs(3.0, 0.001));
}

TEST_CASE("a frame count that falls back to zero records nothing",
          "[pulp-vellum-components][render-pass]") {
    FakeSource source;
    overlay::PassHistory history;
    source.frame = 7;
    source.samples = {{kContent, 1, 2.0f}};
    REQUIRE(history.capture(source));
    // A source that restarts reports zero frames until it runs one again.
    source.frame = 0;
    REQUIRE_FALSE(history.capture(source));
    REQUIRE(history.frames_captured() == 1);
}

TEST_CASE("a pass type past the last is ignored", "[pulp-vellum-components][render-pass]") {
    FakeSource source;
    source.frame = 1;
    source.samples = {{overlay::kPassTypeCount, 9, 9.0f}, {kContent, 1, 2.0f}};
    overlay::PassHistory history;
    REQUIRE(history.capture(source));
    const auto attrib = history.attribution();
    REQUIRE(attrib.size() == overlay::kPassTypeCount);
    REQUIRE(attrib[kContent].samples == 1);
    std::size_t recorded = 0;
    for (const auto& a : attrib)
        recorded += a.samples;
    REQUIRE(recorded == 1);
}

TEST_CASE("budget overruns are counted", "[pulp-vellum-components][render-pass]") {
    PassScene s;
    s.manager.set_budget(5.0f);
    render_synthetic_frame(s.manager, {{RenderPassType::content, 3.0f, 4}});
    REQUIRE(s.overlay.capture_pass_frame());
    REQUIRE(s.overlay.budget_overrun_count() == 0);
    render_synthetic_frame(s.manager, {{RenderPassType::content, 9.0f, 4}});
    REQUIRE(s.overlay.capture_pass_frame());
    REQUIRE(s.overlay.budget_overrun_count() == 1);
    render_synthetic_frame(s.manager, {{RenderPassType::content, 12.0f, 4}});
    REQUIRE(s.overlay.capture_pass_frame());
    REQUIRE(s.overlay.budget_overrun_count() == 2);
}

TEST_CASE("the manager adapter forwards every number", "[pulp-vellum-components][render-pass]") {
    RenderPassManager manager;
    manager.set_budget(8.0f);
    render_synthetic_frame(manager, {
                                        {RenderPassType::effects, 6.0f, 7},
                                        {RenderPassType::post_effects, 4.0f, 2},
                                    });
    const overlay::ManagerPassSource source(manager);
    REQUIRE(source.frame_count() == 1);
    REQUIRE(source.over_budget());
    REQUIRE_THAT(source.total_time_ms(), WithinAbs(10.0, 0.001));
    REQUIRE_THAT(source.budget_ms(), WithinAbs(8.0, 0.001));
    const auto passes = source.passes();
    REQUIRE(passes.size() == 2);
    REQUIRE(passes[0].type == kEffects);
    REQUIRE(passes[0].draw_calls == 7);
    REQUIRE(passes[1].type == 4);
}

TEST_CASE("pass type names follow the compositing order", "[pulp-vellum-components][render-pass]") {
    REQUIRE(std::string_view(overlay::pass_type_name(0)) == "background");
    REQUIRE(std::string_view(overlay::pass_type_name(1)) == "content");
    REQUIRE(std::string_view(overlay::pass_type_name(2)) == "effects");
    REQUIRE(std::string_view(overlay::pass_type_name(3)) == "overlay");
    REQUIRE(std::string_view(overlay::pass_type_name(4)) == "post");
    REQUIRE(std::string_view(overlay::pass_type_name(5)) == "unknown");
}

TEST_CASE("the panel grows by a fixed step for each pass entry",
          "[pulp-vellum-components][render-pass]") {
    const float none = overlay::pass_panel_height(0, true);
    const float one = overlay::pass_panel_height(1, true);
    const float two = overlay::pass_panel_height(2, true);
    // Each entry is a name line, a draw-call line and a sparkline.
    REQUIRE(two - one == 50.0f);
    // An empty history shows one "awaiting" line instead of an entry.
    REQUIRE(one - none == 35.0f);
    // Without a source the panel is one short line.
    REQUIRE(overlay::pass_panel_height(0, false) < none);
}

TEST_CASE("P toggles the viewer while the overlay is on and only then",
          "[pulp-vellum-components][render-pass]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorOverlay overlay(root);
    REQUIRE_FALSE(overlay.handle_key_event(key(KeyCode::p)));
    REQUIRE_FALSE(overlay.pass_viewer_enabled());

    overlay.set_active(true);
    REQUIRE(overlay.handle_key_event(key(KeyCode::p)));
    REQUIRE(overlay.pass_viewer_enabled());
    REQUIRE(overlay.handle_key_event(key(KeyCode::p)));
    REQUIRE_FALSE(overlay.pass_viewer_enabled());

    // A modified P is not the toggle.
    REQUIRE_FALSE(overlay.handle_key_event(key(KeyCode::p, ::vellum::view::kModShift)));
    REQUIRE_FALSE(overlay.pass_viewer_enabled());
}

TEST_CASE("an open edit keeps P for itself", "[pulp-vellum-components][render-pass]") {
    PassScene s;
    auto label = std::make_unique<::vellum::view::Label>("copy");
    label->set_bounds({10, 10, 80, 20});
    View* label_ptr = label.get();
    s.root.add_child(std::move(label));
    s.overlay.set_selected_view(label_ptr);
    REQUIRE(s.overlay.begin_field_edit("layout.padding", 0.0f));
    REQUIRE_FALSE(s.overlay.handle_key_event(key(KeyCode::p)));
    REQUIRE_FALSE(s.overlay.pass_viewer_enabled());
    s.overlay.cancel_field_edit();

    REQUIRE(s.overlay.begin_text_edit(label_ptr));
    REQUIRE_FALSE(s.overlay.handle_key_event(key(KeyCode::p)));
    REQUIRE_FALSE(s.overlay.pass_viewer_enabled());
}

TEST_CASE("the viewer paints a row for each pass that ran",
          "[pulp-vellum-components][render-pass]") {
    PassScene s(500, 720);
    for (int i = 0; i < 3; ++i) {
        render_synthetic_frame(s.manager,
                               {
                                   {RenderPassType::background, 1.0f, 2},
                                   {RenderPassType::content, 5.0f + static_cast<float>(i), 14},
                               });
        if (i == 0)
            s.overlay.set_pass_viewer_enabled(true);
        RecordingCanvas warmup;
        s.overlay.paint(warmup);
    }
    RecordingCanvas canvas;
    s.overlay.paint(canvas);
    REQUIRE(canvas.command_count() > 0);
    REQUIRE(count_text_containing(canvas, "Render Passes") >= 1);
    REQUIRE(count_text_containing(canvas, "background") >= 1);
    REQUIRE(count_text_containing(canvas, "content") >= 1);
    // The note that the timings are CPU time.
    REQUIRE(count_text_containing(canvas, "wall-clock, not GPU") >= 1);
    // effects never ran, so it has no row.
    REQUIRE(count_text_containing(canvas, "effects") == 0);
}

TEST_CASE("the viewer is silent while it is off", "[pulp-vellum-components][render-pass]") {
    PassScene s;
    render_synthetic_frame(s.manager, {{RenderPassType::content, 4.0f, 8}});
    RecordingCanvas canvas;
    s.overlay.paint(canvas);
    REQUIRE(count_text_containing(canvas, "Render Passes") == 0);
    // It still records, so turning it on shows history already gathered.
    REQUIRE(s.overlay.pass_frames_captured() == 1);
}

TEST_CASE("the viewer says so when no source is attached",
          "[pulp-vellum-components][render-pass]") {
    View root;
    root.set_bounds({0, 0, 500, 320});
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_pass_viewer_enabled(true);
    RecordingCanvas canvas;
    overlay.paint(canvas);
    REQUIRE(count_text_containing(canvas, "wall-clock, not GPU") >= 1);
    REQUIRE(count_text_containing(canvas, "No render pass source") >= 1);
}

TEST_CASE("the viewer says it is waiting before the first frame",
          "[pulp-vellum-components][render-pass]") {
    PassScene s;
    s.overlay.set_pass_viewer_enabled(true);
    RecordingCanvas canvas;
    s.overlay.paint(canvas);
    REQUIRE(count_text_containing(canvas, "Awaiting render frames") >= 1);
}

TEST_CASE("paint drives the capture, once per frame", "[pulp-vellum-components][render-pass]") {
    PassScene s;
    render_synthetic_frame(s.manager, {{RenderPassType::content, 4.0f, 8}});
    RecordingCanvas canvas;
    s.overlay.paint(canvas);
    REQUIRE(s.overlay.pass_frames_captured() == 1);
    s.overlay.paint(canvas);
    REQUIRE(s.overlay.pass_frames_captured() == 1);
    render_synthetic_frame(s.manager, {{RenderPassType::content, 6.0f, 9}});
    s.overlay.paint(canvas);
    REQUIRE(s.overlay.pass_frames_captured() == 2);
}

TEST_CASE("a warning-coloured total shows when the frame went over budget",
          "[pulp-vellum-components][render-pass]") {
    PassScene s;
    s.manager.set_budget(5.0f);
    render_synthetic_frame(s.manager, {{RenderPassType::content, 9.0f, 4}});
    s.overlay.set_pass_viewer_enabled(true);
    RecordingCanvas canvas;
    s.overlay.paint(canvas);
    // The total line is the one carrying "budget"; its fill is the warning
    // colour, set just before the text.
    const DrawCommand* totals = nullptr;
    const DrawCommand* before = nullptr;
    const DrawCommand* previous_fill = nullptr;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::set_fill_color)
            previous_fill = &cmd;
        if (cmd.type == DrawCommand::Type::fill_text &&
            cmd.text.find("budget") != std::string::npos) {
            totals = &cmd;
            before = previous_fill;
        }
    }
    REQUIRE(totals != nullptr);
    REQUIRE(before != nullptr);
    REQUIRE(before->color.g < 0.5f);
}

TEST_CASE("a root shorter than the panel still paints without overrunning it",
          "[pulp-vellum-components][render-pass]") {
    PassScene s(300, 60);
    render_synthetic_frame(s.manager, {{RenderPassType::content, 4.0f, 8}});
    s.overlay.set_pass_viewer_enabled(true);
    RecordingCanvas canvas;
    REQUIRE_NOTHROW(s.overlay.paint(canvas));
    REQUIRE(count_text_containing(canvas, "Render Passes") >= 1);
}

TEST_CASE("the viewer renders its panel at the top right with a colour chip per pass",
          "[pulp-vellum-components][render-pass][pixels]") {
    View root;
    root.set_bounds({0, 0, 500, 320});
    RenderPassManager manager;
    overlay::ManagerPassSource source(manager);
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    overlay.set_render_pass_source(&source);
    overlay.set_pass_viewer_enabled(true);
    install_inspector_hooks(overlay);

    // Three frames with a background and a content pass, so each has history.
    for (int i = 0; i < 3; ++i) {
        render_synthetic_frame(manager,
                               {
                                   {RenderPassType::background, 1.0f, 2},
                                   {RenderPassType::content, 5.0f + static_cast<float>(i), 14},
                               });
        RecordingCanvas warm;
        overlay.paint(warm);
    }
    // A new frame for the render itself to record.
    render_synthetic_frame(manager, {
                                        {RenderPassType::background, 1.0f, 2},
                                        {RenderPassType::content, 8.0f, 14},
                                    });
    const auto frame = render(root, 500, 320);
    maybe_write_png(root, 500, 320, "inspector-overlay-pass-viewer.png");
    uninstall_inspector_hooks();

    // The panel is a dark rounded rectangle docked to the top-right corner and
    // nothing paints in the bottom-left.
    const auto panel_bg = frame.at(500 - 12 - 6, 12 + 90);
    REQUIRE(panel_bg.r < 60);
    REQUIRE(panel_bg.b < 70);

    // Each pass that ran has its chip colour: the blue background chip and the
    // green content chip, and no amber effects chip.
    int blue = 0;
    int green = 0;
    int amber = 0;
    for (std::uint32_t y = 12; y < 200; ++y) {
        for (std::uint32_t x = 500 - 12 - 260; x < 500 - 12; ++x) {
            const auto p = frame.at(x, y);
            if (near(p, 102, 140, 217, 6))
                ++blue;
            if (near(p, 115, 204, 140, 6))
                ++green;
            if (near(p, 230, 178, 77, 6))
                ++amber;
        }
    }
    REQUIRE(blue >= 40);
    REQUIRE(green >= 40);
    REQUIRE(amber == 0);
}
