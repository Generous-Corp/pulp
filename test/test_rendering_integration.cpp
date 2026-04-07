#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/view_effect.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/geometry.hpp>
#include <pulp/view/sprite_strip.hpp>
#include <pulp/render/render_pass.hpp>

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

TEST_CASE("GpuBloomEffect configures with intensity", "[render][effect]") {
    canvas::RecordingCanvas rc;
    canvas::GpuBloomEffect bloom;
    bloom.intensity = 0.7f;
    bloom.threshold = 0.9f;
    bloom.configure_layer(rc, 0, 0, 200, 200);
    REQUIRE(rc.command_count() > 0);
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

TEST_CASE("CustomShaderEffect stores SkSL", "[render][effect]") {
    canvas::CustomShaderEffect cse;
    cse.sksl = "half4 main(float2 c) { return half4(1); }";
    cse.value = 0.5f;
    REQUIRE_FALSE(cse.sksl.empty());
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
