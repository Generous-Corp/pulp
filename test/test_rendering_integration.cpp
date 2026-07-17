#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/view_effect.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/geometry.hpp>
#include <pulp/view/sprite_strip.hpp>
#include <pulp/view/theme_editor.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/state/edit_history.hpp>
#include <pulp/render/render_pass.hpp>
#include <pulp/render/render_loop.hpp>
#include <pulp/render/draco_decoder.hpp>
#include <pulp/render/ktx2_decoder.hpp>
#include <atomic>
#include <chrono>
#include <thread>

using namespace pulp;

// ── ViewEffect tests ────────────────────────────────────────────────────

TEST_CASE("GpuBlurEffect configures layer with blur", "[render][effect]") {
    canvas::RecordingCanvas rc;
    canvas::GpuBlurEffect blur;
    blur.radius_x = 8.0f;
    REQUIRE(blur.needs_layer());
    blur.configure_layer(rc, 0, 0, 100, 100);
    REQUIRE(rc.command_count() > 0);  // save_layer recorded
}

// GpuBloomEffect must route through the real bloom layer API, not a plain
// save_layer. RecordingCanvas records save_layer_with_bloom as its own command
// carrying intensity / threshold / radius, so a headless test can prove the
// effect asked for a bloom (not just "some command was emitted").
TEST_CASE("GpuBloomEffect emits a bloom layer with its parameters",
          "[render][effect]") {
    canvas::RecordingCanvas rc;
    canvas::GpuBloomEffect bloom;
    bloom.intensity = 0.7f;
    bloom.threshold = 0.9f;
    bloom.radius = 12.0f;
    bloom.configure_layer(rc, 0, 0, 200, 200);

    REQUIRE(rc.count(canvas::DrawCommand::Type::save_layer_bloom) == 1);
    REQUIRE(rc.count(canvas::DrawCommand::Type::save_layer) == 0);  // not a plain layer
    const auto& cmd = rc.commands().back();
    REQUIRE(cmd.f[4] == Catch::Approx(0.7f));   // intensity
    REQUIRE(cmd.f[5] == Catch::Approx(0.9f));   // threshold
    REQUIRE(cmd.floats.size() == 1);
    REQUIRE(cmd.floats[0] == Catch::Approx(12.0f));  // radius
}

TEST_CASE("VignetteEffect has meaningful intensity", "[render][effect]") {
    canvas::VignetteEffect vignette;
    vignette.intensity = 0.8f;
    REQUIRE(vignette.intensity == Catch::Approx(0.8f));
    REQUIRE(vignette.needs_layer());
}

TEST_CASE("ChromaticAberrationEffect has offset", "[render][effect]") {
    canvas::ChromaticAberrationEffect ca;
    ca.offset = 3.0f;
    REQUIRE(ca.offset == Catch::Approx(3.0f));
}

TEST_CASE("EffectChain composes multiple effects", "[render][effect]") {
    auto chain = std::make_shared<canvas::EffectChain>();
    chain->add(std::make_shared<canvas::GpuBlurEffect>());
    chain->add(std::make_shared<canvas::VignetteEffect>());
    REQUIRE(chain->effects().size() == 2);
    REQUIRE(chain->needs_layer());
}

// An effect must push exactly `layer_count()` layers, because View::paint_all
// pops that many. EffectChain pushes one per child, so it must report the sum —
// reporting 1 (the base default) leaked every layer past the first, on every
// paint, corrupting the canvas save stack for the rest of the frame.
TEST_CASE("EffectChain reports one layer per effect", "[render][effect]") {
    auto chain = std::make_shared<canvas::EffectChain>();
    REQUIRE(chain->layer_count() == 0);
    REQUIRE_FALSE(chain->needs_layer());

    chain->add(std::make_shared<canvas::GpuBlurEffect>());
    REQUIRE(chain->layer_count() == 1);

    chain->add(std::make_shared<canvas::VignetteEffect>());
    chain->add(std::make_shared<canvas::ChromaticAberrationEffect>());
    REQUIRE(chain->layer_count() == 3);

    // Nested chains sum through.
    auto outer = std::make_shared<canvas::EffectChain>();
    outer->add(chain);
    outer->add(std::make_shared<canvas::GpuBlurEffect>());
    REQUIRE(outer->layer_count() == 4);
}

TEST_CASE("Simple effects push exactly one layer", "[render][effect]") {
    REQUIRE(canvas::GpuBlurEffect{}.layer_count() == 1);
    REQUIRE(canvas::GpuBloomEffect{}.layer_count() == 1);
    REQUIRE(canvas::VignetteEffect{}.layer_count() == 1);
    REQUIRE(canvas::ChromaticAberrationEffect{}.layer_count() == 1);
}

// The save stack a View leaves behind must be exactly as deep as it found it.
// paint_all() pops `layer_count()` layers; before that existed it always popped
// one, so a 3-effect chain leaked 2 layers on every paint and every subsequent
// draw in the frame ran against a corrupted stack.
TEST_CASE("A View with a multi-effect chain leaves the save stack balanced",
          "[render][effect][view]") {
    auto chain = std::make_shared<canvas::EffectChain>();
    chain->add(std::make_shared<canvas::GpuBlurEffect>());
    chain->add(std::make_shared<canvas::VignetteEffect>());
    chain->add(std::make_shared<canvas::GpuBloomEffect>());

    view::View root;
    root.set_bounds({0, 0, 100, 100});
    root.set_effect(chain);

    canvas::RecordingCanvas canvas;
    const int depth_before = canvas.save_count();
    root.paint_all(canvas);
    REQUIRE(canvas.save_count() == depth_before);
}

// A single-effect View was always balanced; keep it that way.
TEST_CASE("A View with one effect leaves the save stack balanced",
          "[render][effect][view]") {
    view::View root;
    root.set_bounds({0, 0, 100, 100});
    root.set_effect(std::make_shared<canvas::GpuBlurEffect>());

    canvas::RecordingCanvas canvas;
    const int depth_before = canvas.save_count();
    root.paint_all(canvas);
    REQUIRE(canvas.save_count() == depth_before);
}

// An effect that wants no layer (an empty EffectChain) must not swallow the
// layer that opacity still needs. paint_all() takes the effect branch only when
// the effect reports needs_layer(); otherwise opacity would be silently dropped
// — and, before layer_count() existed, the unmatched restore() underflowed the
// save stack.
TEST_CASE("An empty EffectChain does not swallow the opacity layer",
          "[render][effect][view]") {
    view::View root;
    root.set_bounds({0, 0, 100, 100});
    root.set_opacity(0.5f);
    root.set_effect(std::make_shared<canvas::EffectChain>());  // no children

    canvas::RecordingCanvas canvas;
    const int depth_before = canvas.save_count();
    root.paint_all(canvas);

    // Balanced, and the opacity layer was actually pushed. RecordingCanvas now
    // records the opacity compositing layer as a distinct save_layer command
    // rather than a bare save(), so assert on that.
    REQUIRE(canvas.save_count() == depth_before);
    REQUIRE(canvas.count(canvas::DrawCommand::Type::save_layer) >= 1);
}

// ── Dimension tests ─────────────────────────────────────────────────────

TEST_CASE("Dimension parse px", "[view][dimension]") {
    auto d = view::Dimension::parse("100px");
    REQUIRE(d.unit == view::DimensionUnit::px);
    REQUIRE(d.value == Catch::Approx(100.0f));
    REQUIRE(d.resolve(0, 0, 0) == Catch::Approx(100.0f));
}

TEST_CASE("Dimension parse percent", "[view][dimension]") {
    auto d = view::Dimension::parse("50%");
    REQUIRE(d.unit == view::DimensionUnit::percent);
    REQUIRE(d.resolve(200, 0, 0) == Catch::Approx(100.0f));
}

TEST_CASE("Dimension parse vw", "[view][dimension]") {
    auto d = view::Dimension::parse("25vw");
    REQUIRE(d.unit == view::DimensionUnit::vw);
    REQUIRE(d.resolve(0, 800, 600) == Catch::Approx(200.0f));
}

TEST_CASE("Dimension parse vh", "[view][dimension]") {
    auto d = view::Dimension::parse("50vh");
    REQUIRE(d.unit == view::DimensionUnit::vh);
    REQUIRE(d.resolve(0, 800, 600) == Catch::Approx(300.0f));
}

TEST_CASE("Dimension parse vmin", "[view][dimension]") {
    auto d = view::Dimension::parse("10vmin");
    REQUIRE(d.unit == view::DimensionUnit::vmin);
    REQUIRE(d.resolve(0, 800, 600) == Catch::Approx(60.0f));
}

TEST_CASE("Dimension parse vmax", "[view][dimension]") {
    auto d = view::Dimension::parse("10vmax");
    REQUIRE(d.unit == view::DimensionUnit::vmax);
    REQUIRE(d.resolve(0, 800, 600) == Catch::Approx(80.0f));
}

TEST_CASE("Dimension parse auto", "[view][dimension]") {
    auto d = view::Dimension::parse("auto");
    REQUIRE(d.unit == view::DimensionUnit::auto_);
}

TEST_CASE("Dimension parse trims supported units and rejects unknown suffixes",
          "[view][dimension]") {
    auto px = view::Dimension::parse(" 12px \t");
    REQUIRE(px.unit == view::DimensionUnit::px);
    REQUIRE(px.value == Catch::Approx(12.0f));

    auto bare = view::Dimension::parse(" 8 ");
    REQUIRE(bare.unit == view::DimensionUnit::px);
    REQUIRE(bare.value == Catch::Approx(8.0f));

    auto aut = view::Dimension::parse(" auto ");
    REQUIRE(aut.unit == view::DimensionUnit::auto_);

    auto rem = view::Dimension::parse("100rem");
    REQUIRE(rem.unit == view::DimensionUnit::px);
    REQUIRE(rem.value == Catch::Approx(0.0f));

    auto junk = view::Dimension::parse("12pxjunk");
    REQUIRE(junk.unit == view::DimensionUnit::px);
    REQUIRE(junk.value == Catch::Approx(0.0f));
}

TEST_CASE("Dimension DPI scaling", "[view][dimension]") {
    auto d = view::Dimension::parse("10px");
    REQUIRE(d.resolve(0, 0, 0, 2.0f) == Catch::Approx(20.0f));
}

// ── Text direction tests ────────────────────────────────────────────────

TEST_CASE("Label text direction property", "[view][label]") {
    view::Label label("Test");
    REQUIRE(label.text_direction() == canvas::TextDirection::left_to_right);

    label.set_text_direction(canvas::TextDirection::top_to_bottom);
    REQUIRE(label.text_direction() == canvas::TextDirection::top_to_bottom);
}

TEST_CASE("Label vertical alignment property", "[view][label]") {
    view::Label label("Test");
    label.set_vertical_align(canvas::TextVerticalAlign::bottom);
    REQUIRE(label.vertical_align() == canvas::TextVerticalAlign::bottom);
}

TEST_CASE("Label paints with vertical text", "[view][label]") {
    view::Label label("Vertical");
    label.set_bounds({0, 0, 100, 200});
    label.set_text_direction(canvas::TextDirection::top_to_bottom);

    canvas::RecordingCanvas rc;
    label.paint(rc);
    REQUIRE(rc.command_count() > 0);
}

// ── RenderPassManager tests ─────────────────────────────────────────────

TEST_CASE("RenderPassManager tracks passes", "[render][pass]") {
    render::RenderPassManager pm;
    pm.begin_frame();
    pm.begin_pass(render::RenderPassType::background);
    pm.end_pass(2.0f, 5);
    pm.begin_pass(render::RenderPassType::content);
    pm.end_pass(8.0f, 20);
    pm.end_frame();

    REQUIRE(pm.passes().size() == 2);
    REQUIRE(pm.total_time_ms() == Catch::Approx(10.0f));
    REQUIRE_FALSE(pm.over_budget());
}

TEST_CASE("RenderPassManager detects over budget", "[render][pass]") {
    render::RenderPassManager pm;
    pm.set_budget(5.0f);
    pm.begin_frame();
    pm.begin_pass(render::RenderPassType::content);
    pm.end_pass(10.0f, 100);
    pm.end_frame();

    REQUIRE(pm.over_budget());
}

TEST_CASE("RenderPassManager resets per-frame stats and preserves frame count",
          "[render][pass]") {
    render::RenderPassManager pm;

    REQUIRE(pm.frame_count() == 0);
    REQUIRE(pm.current_pass() == render::RenderPassType::background);

    pm.begin_frame();
    REQUIRE(pm.frame_count() == 1);
    pm.begin_pass(render::RenderPassType::overlay);
    REQUIRE(pm.current_pass() == render::RenderPassType::overlay);
    pm.end_pass(1.5f, 3);
    pm.end_frame();

    REQUIRE(pm.passes().size() == 1);
    REQUIRE(pm.passes().front().type == render::RenderPassType::overlay);
    REQUIRE(pm.passes().front().draw_calls == 3);

    pm.begin_frame();
    REQUIRE(pm.frame_count() == 2);
    REQUIRE(pm.passes().empty());
    REQUIRE(pm.total_time_ms() == Catch::Approx(0.0f));
}

TEST_CASE("RenderPassManager handles empty passes and disabled budget",
          "[render][pass]") {
    render::RenderPassManager pm;

    pm.begin_frame();
    pm.end_pass(99.0f, 100);
    pm.end_frame();
    REQUIRE(pm.passes().empty());
    REQUIRE(pm.total_time_ms() == Catch::Approx(0.0f));
    REQUIRE_FALSE(pm.over_budget());

    pm.set_budget(0.0f);
    REQUIRE(pm.budget() == Catch::Approx(0.0f));
    pm.begin_frame();
    pm.begin_pass(render::RenderPassType::post_effects);
    pm.end_pass(250.0f, 1);
    pm.end_frame();

    REQUIRE(pm.total_time_ms() == Catch::Approx(250.0f));
    REQUIRE_FALSE(pm.over_budget());
}

// ── SpriteStrip on Knob ─────────────────────────────────────────────────

TEST_CASE("Knob with sprite strip set", "[view][widget]") {
    view::Knob knob;
    auto strip = std::make_shared<view::SpriteStrip>();
    std::vector<uint8_t> data(32 * 320 * 4, 128);
    strip->load(data.data(), data.size(), 32, 320, 10);

    knob.set_sprite_strip(strip);
    REQUIRE(knob.sprite_strip() != nullptr);
    REQUIRE(knob.sprite_strip()->loaded());
}

TEST_CASE("Fader with sprite strip set", "[view][widget]") {
    view::Fader fader;
    auto strip = std::make_shared<view::SpriteStrip>();
    std::vector<uint8_t> data(64 * 640 * 4, 200);
    strip->load(data.data(), data.size(), 64, 640, 10);

    fader.set_sprite_strip(strip);
    REQUIRE(fader.sprite_strip()->frame_count() == 10);
}

// ── Gradient tests ──────────────────────────────────────────────────────

TEST_CASE("ConicGradient construction", "[canvas][gradient]") {
    canvas::ConicGradient cg;
    cg.cx = 100;
    cg.cy = 100;
    cg.start_angle = 0;
    cg.stops = {{0.0f, canvas::Color::rgba(1, 0, 0)}, {1.0f, canvas::Color::rgba(0, 0, 1)}};
    REQUIRE(cg.stops.size() == 2);
}

TEST_CASE("FillStyle solid color", "[canvas][gradient]") {
    canvas::FillStyle fs(canvas::Color::rgba(1, 0, 0));
    REQUIRE(fs.is_solid());
    REQUIRE_FALSE(fs.is_linear());
    REQUIRE_FALSE(fs.is_conic());
}

TEST_CASE("FillStyle linear gradient", "[canvas][gradient]") {
    canvas::LinearGradient lg{0, 0, 100, 0, {{0, canvas::Color::rgba(1,0,0)}, {1, canvas::Color::rgba(0,0,1)}}};
    canvas::FillStyle fs(lg);
    REQUIRE(fs.is_linear());
    REQUIRE_FALSE(fs.is_solid());
}

TEST_CASE("FillStyle radial gradient exposes focal point and stops",
          "[canvas][gradient]") {
    canvas::RadialGradient rg{10.0f, 20.0f, 30.0f, 0.0f, 0.0f, {}};
    rg.focal_x = 12.0f;
    rg.focal_y = 18.0f;
    rg.stops = {{0.0f, canvas::Color::rgba(1, 1, 1)},
                {1.0f, canvas::Color::rgba(0, 0, 0)}};

    canvas::FillStyle fs(rg);

    REQUIRE(fs.is_radial());
    REQUIRE_FALSE(fs.is_linear());
    REQUIRE_FALSE(fs.is_conic());
    REQUIRE(fs.radial().cx == 10.0f);
    REQUIRE(fs.radial().cy == 20.0f);
    REQUIRE(fs.radial().radius == 30.0f);
    REQUIRE(fs.radial().focal_x == 12.0f);
    REQUIRE(fs.radial().focal_y == 18.0f);
    REQUIRE(fs.radial().stops.size() == 2);
}

TEST_CASE("FillStyle conic gradient", "[canvas][gradient]") {
    canvas::ConicGradient cg;
    cg.cx = 50; cg.cy = 50; cg.start_angle = 0;
    canvas::FillStyle fs(cg);
    REQUIRE(fs.is_conic());
}

TEST_CASE("GradientTileMode on FillStyle", "[canvas][gradient]") {
    canvas::FillStyle fs(canvas::Color::rgba(1, 1, 1));
    fs.set_tile_mode(canvas::GradientTileMode::repeat);
    REQUIRE(fs.tile_mode() == canvas::GradientTileMode::repeat);
}

// ── ThemeEditor tests ───────────────────────────────────────────────────

TEST_CASE("ThemeEditor set and get theme", "[view][theme-editor]") {
    view::ThemeEditor editor;
    auto theme = view::Theme::dark();
    editor.set_theme(theme);

    auto names = editor.token_names();
    REQUIRE_FALSE(names.empty());
    REQUIRE(editor.editing_theme().colors.size() > 0);
}

TEST_CASE("ThemeEditor select token", "[view][theme-editor]") {
    view::ThemeEditor editor;
    editor.set_theme(view::Theme::dark());
    editor.select_token("accent.primary");
    REQUIRE(editor.selected_token() == "accent.primary");
}

TEST_CASE("ThemeEditor export JSON", "[view][theme-editor]") {
    view::ThemeEditor editor;
    editor.set_theme(view::Theme::dark());
    auto json = editor.export_json();
    REQUIRE_FALSE(json.empty());
    REQUIRE(json.find("accent.primary") != std::string::npos);
}

TEST_CASE("ThemeEditor paint renders", "[view][theme-editor]") {
    view::ThemeEditor editor;
    editor.set_theme(view::Theme::dark());
    editor.set_bounds({0, 0, 400, 300});

    canvas::RecordingCanvas rc;
    editor.paint_all(rc);
    REQUIRE(rc.command_count() > 0);
}

// ── EditHistory tests ───────────────────────────────────────────────────

TEST_CASE("EditHistory undo redo", "[state][undo]") {
    state::EditHistory history;
    int val = 0;
    history.perform([&]{ val = 42; }, [&]{ val = 0; }, "set 42");
    REQUIRE(val == 42);
    REQUIRE(history.can_undo());

    history.undo();
    REQUIRE(val == 0);
    REQUIRE(history.can_redo());

    history.redo();
    REQUIRE(val == 42);
}

TEST_CASE("EditHistory depth limit", "[state][undo]") {
    state::EditHistory history(3);
    int v = 0;
    for (int i = 0; i < 5; ++i)
        history.perform([&, i]{ v = i; }, [&]{ v = 0; });
    REQUIRE(history.undo_count() == 3);
}

// ── DRACO decoder ───────────────────────────────────────────────────────

TEST_CASE("DRACO decoder returns empty for invalid data", "[render][draco]") {
    auto result = render::decode_draco(nullptr, 0);
    REQUIRE_FALSE(result.success);

    uint8_t garbage[] = {0, 1, 2, 3};
    auto result2 = render::decode_draco(garbage, 4);
    REQUIRE_FALSE(result2.success);

    render::DracoAttributeIds attribute_ids;
    attribute_ids.position = 7;
    auto result3 = render::decode_draco(garbage, 4, attribute_ids);
    REQUIRE_FALSE(result3.success);
    (void)render::draco_decoder_available();
}

// ── KTX2 decoder ────────────────────────────────────────────────────────

TEST_CASE("KTX2 decoder validates magic bytes", "[render][ktx2]") {
    uint8_t not_ktx2[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    auto result = render::decode_ktx2(not_ktx2, 13);
    REQUIRE_FALSE(result.success);
}

TEST_CASE("KTX2 decoder rejects incomplete headers - issue-646",
          "[render][ktx2][issue-646]") {
    REQUIRE_FALSE(render::decode_ktx2(nullptr, 0).success);

    uint8_t short_input[] = {0xAB, 0x4B, 0x54, 0x58};
    auto short_result = render::decode_ktx2(short_input, sizeof(short_input));
    REQUIRE_FALSE(short_result.success);

    uint8_t magic_only[] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB,
        0x0D, 0x0A, 0x1A, 0x0A
    };
    auto magic_result = render::decode_ktx2(magic_only, sizeof(magic_only));
    REQUIRE_FALSE(magic_result.success);
    REQUIRE(magic_result.width == 0);
    REQUIRE(magic_result.height == 0);
}

TEST_CASE("KTX2 decoder parses a minimal header - issue-646",
          "[render][ktx2][issue-646]") {
    uint8_t header[80] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB,
        0x0D, 0x0A, 0x1A, 0x0A
    };

    auto write_u32 = [&](size_t offset, uint32_t value) {
        header[offset] = static_cast<uint8_t>(value & 0xFFu);
        header[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
        header[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
        header[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
    };

    write_u32(20, 64);  // pixelWidth
    write_u32(24, 32);  // pixelHeight
    write_u32(32, 0);   // levelCount clamps to one mip level

    auto result = render::decode_ktx2(header, sizeof(header));
    REQUIRE(result.success);
    REQUIRE(result.width == 64);
    REQUIRE(result.height == 32);
    REQUIRE(result.mip_levels == 1);
    REQUIRE_FALSE(result.format.empty());
    REQUIRE(result.pixels.empty());
    REQUIRE_FALSE(result.compressed);
}

TEST_CASE("KTX2 optimal_gpu_format returns non-empty", "[render][ktx2]") {
    auto fmt = render::optimal_gpu_format();
    REQUIRE_FALSE(fmt.empty());
}

TEST_CASE("KTX2 availability check", "[render][ktx2]") {
    // Without libktx linked, should return false
    // With libktx, should return true
    auto avail = render::ktx2_available();
    (void)avail;  // Just verify it doesn't crash
}

// ── RenderLoop lifecycle — issue-646 ────────────────────────────────────
//
// These tests drive the platform render loop (CVDisplayLink on macOS,
// TimerRenderLoop elsewhere) through the full start / request_frame /
// stop cycle. They exercise the factory + public lifecycle surface
// without requiring a real GPU surface or window handle.

#if !defined(__APPLE__)
namespace {
bool wait_for_frames(std::atomic<int>& frames, int target) {
    for (int i = 0; i < 200; ++i) {
        if (frames.load(std::memory_order_relaxed) >= target) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return frames.load(std::memory_order_relaxed) >= target;
}
} // namespace
#endif

TEST_CASE("RenderLoop factory returns a loop that starts and stops - issue-646",
          "[render][loop][issue-646]") {
    auto loop = render::RenderLoop::create();
    REQUIRE(loop != nullptr);
    REQUIRE_FALSE(loop->is_running());

    std::atomic<int> frames{0};
    loop->start([&]() { frames.fetch_add(1, std::memory_order_relaxed); });
    REQUIRE(loop->is_running());

    // Request a frame; the callback may fire asynchronously on the
    // backend's pacing (main queue on macOS, worker thread elsewhere).
    // We don't assert it fires because the macOS path dispatches onto
    // the main queue and this unit test doesn't pump the run loop —
    // that's an integration concern. We only assert the loop accepts
    // the request without error.
    loop->request_frame();

    loop->stop();
    REQUIRE_FALSE(loop->is_running());
}

TEST_CASE("RenderLoop stop is idempotent - issue-646",
          "[render][loop][issue-646]") {
    auto loop = render::RenderLoop::create();
    REQUIRE(loop != nullptr);

    loop->start([]() {});
    loop->stop();
    REQUIRE_FALSE(loop->is_running());

    // Calling stop() a second time must be safe (no hang, no crash).
    loop->stop();
    REQUIRE_FALSE(loop->is_running());
}

TEST_CASE("RenderLoop destructor stops a running loop - issue-646",
          "[render][loop][issue-646]") {
    std::atomic<int> frames{0};
    {
        auto loop = render::RenderLoop::create();
        REQUIRE(loop != nullptr);
        loop->start([&]() { frames.fetch_add(1, std::memory_order_relaxed); });
        REQUIRE(loop->is_running());
        loop->request_frame();
        // Let the worker (on non-macOS) pick up the frame request before teardown.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // Falls out of scope → destructor calls stop() → join worker thread.
    }
    // If the destructor didn't stop cleanly, this test would hang / crash
    // under ASan. Reaching this point is the assertion.
    SUCCEED("RenderLoop destructor joined cleanly");
}

TEST_CASE("RenderLoop request_frame before start is a safe no-op - issue-646",
          "[render][loop][issue-646]") {
    auto loop = render::RenderLoop::create();
    REQUIRE(loop != nullptr);
    // Must not crash when called on a never-started loop.
    loop->request_frame();
    REQUIRE_FALSE(loop->is_running());
}

#if !defined(__APPLE__)
TEST_CASE("RenderLoop timer backend invokes requested frames - issue-646",
          "[render][loop][issue-646]") {
    auto loop = render::RenderLoop::create();
    REQUIRE(loop != nullptr);

    std::atomic<int> frames{0};
    loop->start([&]() { frames.fetch_add(1, std::memory_order_relaxed); });
    REQUIRE(loop->is_running());

    REQUIRE(wait_for_frames(frames, 1));
    const auto before_request = frames.load(std::memory_order_relaxed);
    loop->request_frame();
    REQUIRE(wait_for_frames(frames, before_request + 1));

    loop->stop();
    REQUIRE_FALSE(loop->is_running());
}

TEST_CASE("RenderLoop timer backend restarts after stop - issue-646",
          "[render][loop][issue-646]") {
    auto loop = render::RenderLoop::create();
    REQUIRE(loop != nullptr);

    std::atomic<int> frames{0};
    loop->start([&]() { frames.fetch_add(1, std::memory_order_relaxed); });
    REQUIRE(wait_for_frames(frames, 1));
    loop->stop();
    REQUIRE_FALSE(loop->is_running());

    loop->start([&]() { frames.fetch_add(1, std::memory_order_relaxed); });
    REQUIRE(loop->is_running());
    REQUIRE(wait_for_frames(frames, 2));
    loop->stop();
    REQUIRE_FALSE(loop->is_running());
}
#endif
