#include <pulp/state/store.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

using namespace pulp::view;

namespace {

struct ScreenshotFixture {
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    DesignIR ir;
};

IRNode frame(std::string id,
             float width,
             float height,
             LayoutDirection direction,
             std::string background = {}) {
    IRNode node;
    node.type = "frame";
    node.name = id;
    node.stable_anchor_id = id;
    node.style.width = width;
    node.style.height = height;
    node.layout.direction = direction;
    if (!background.empty()) node.style.background_color = std::move(background);
    return node;
}

IRNode text(std::string id,
            std::string value,
            float width,
            float height,
            std::string color = "#d6d7df",
            float font_size = 14.0f) {
    IRNode node;
    node.type = "text";
    node.name = id;
    node.stable_anchor_id = id;
    node.text_content = std::move(value);
    node.style.width = width;
    node.style.height = height;
    node.style.color = std::move(color);
    node.style.font_size = font_size;
    return node;
}

void padding(IRNode& node, float value) {
    node.layout.padding_top = value;
    node.layout.padding_right = value;
    node.layout.padding_bottom = value;
    node.layout.padding_left = value;
}

void radius(IRNode& node, float value) {
    node.style.border_radius = value;
}

DesignIR fixture_ir(std::string adapter, IRNode root) {
    DesignIR ir;
    ir.source = DesignSource::jsx;
    ir.source_adapter = std::move(adapter);
    ir.root = std::move(root);
    return ir;
}

IRNode control_card(std::string id, std::string label, std::string value) {
    auto card = frame(id, 104.0f, 84.0f, LayoutDirection::column, "#171b28");
    padding(card, 7.0f);
    radius(card, 6.0f);
    card.layout.gap = 5.0f;
    card.children.push_back(text(id + "-label", std::move(label), 90.0f, 18.0f, "#9aa3b8", 12.0f));
    card.children.push_back(text(id + "-value", std::move(value), 90.0f, 24.0f, "#f5f7ff", 17.0f));
    return card;
}

ScreenshotFixture make_control_strip_fixture() {
    auto root = frame("phase6-control-strip", 360.0f, 160.0f, LayoutDirection::column, "#10131d");
    padding(root, 12.0f);
    root.layout.gap = 10.0f;

    auto header = frame("phase6-control-header", 336.0f, 28.0f, LayoutDirection::row);
    header.layout.justify = LayoutAlign::space_between;
    header.layout.align = LayoutAlign::center;
    header.children.push_back(text("phase6-title", "Cloud Chorus", 160.0f, 24.0f, "#eef2ff", 18.0f));
    header.children.push_back(text("phase6-state", "-12 dB", 76.0f, 24.0f, "#82aaff", 15.0f));

    auto controls = frame("phase6-control-cards", 336.0f, 84.0f, LayoutDirection::row);
    controls.layout.gap = 12.0f;
    controls.children.push_back(control_card("phase6-depth", "Depth", "67%"));
    controls.children.push_back(control_card("phase6-rate", "Rate", "1.8 Hz"));
    controls.children.push_back(control_card("phase6-mix", "Mix", "42%"));

    root.children.push_back(std::move(header));
    root.children.push_back(std::move(controls));
    return {"control-strip", 360, 160, fixture_ir("phase6-control-strip", std::move(root))};
}

ScreenshotFixture make_meter_ladder_fixture() {
    auto root = frame("phase6-meter-ladder", 300.0f, 180.0f, LayoutDirection::column, "#07111e");
    padding(root, 12.0f);
    root.layout.gap = 8.0f;
    root.children.push_back(text("phase6-meter-title", "Level snapshot", 180.0f, 24.0f, "#dff6ff", 17.0f));

    auto row = frame("phase6-meter-row", 276.0f, 118.0f, LayoutDirection::row);
    row.layout.gap = 9.0f;
    row.layout.align = LayoutAlign::flex_end;

    const std::vector<float> heights{38.0f, 72.0f, 94.0f, 56.0f, 106.0f};
    const std::vector<std::string> colors{"#3bd671", "#82d173", "#f6d365", "#ff9364", "#f2545b"};
    for (std::size_t i = 0; i < heights.size(); ++i) {
        auto lane = frame("phase6-meter-lane-" + std::to_string(i), 46.0f, 112.0f,
                          LayoutDirection::column, "#0d2336");
        lane.layout.justify = LayoutAlign::flex_end;
        lane.layout.align = LayoutAlign::center;
        padding(lane, 5.0f);
        radius(lane, 4.0f);

        auto bar = frame("phase6-meter-bar-" + std::to_string(i), 28.0f, heights[i],
                         LayoutDirection::column, colors[i]);
        radius(bar, 3.0f);
        lane.children.push_back(std::move(bar));
        row.children.push_back(std::move(lane));
    }

    root.children.push_back(std::move(row));
    return {"meter-ladder", 300, 180, fixture_ir("phase6-meter-ladder", std::move(root))};
}

ScreenshotFixture make_settings_grid_fixture() {
    auto root = frame("phase6-settings-grid", 400.0f, 176.0f, LayoutDirection::row, "#f5f2ea");
    padding(root, 14.0f);
    root.layout.gap = 16.0f;

    auto left = frame("phase6-settings-left", 170.0f, 148.0f, LayoutDirection::column, "#ffffff");
    padding(left, 10.0f);
    left.layout.gap = 7.0f;
    radius(left, 8.0f);
    left.children.push_back(text("phase6-settings-title", "Performance", 140.0f, 24.0f, "#1b1b24", 18.0f));
    left.children.push_back(text("phase6-settings-copy", "Oversampling and timing", 140.0f, 40.0f, "#4f5565", 13.0f));

    auto right = frame("phase6-settings-right", 186.0f, 148.0f, LayoutDirection::column, "#e9edf5");
    padding(right, 10.0f);
    right.layout.gap = 8.0f;
    radius(right, 8.0f);
    for (const auto& row_data : std::vector<std::pair<std::string, std::string>>{
             {"Quality", "High"},
             {"Latency", "2.7 ms"},
             {"CPU", "12%"}}) {
        auto row = frame("phase6-settings-row-" + row_data.first, 166.0f, 28.0f, LayoutDirection::row, "#f8fafc");
        row.layout.justify = LayoutAlign::space_between;
        row.layout.align = LayoutAlign::center;
        padding(row, 5.0f);
        radius(row, 5.0f);
        row.children.push_back(text("phase6-settings-key-" + row_data.first, row_data.first, 78.0f, 18.0f, "#475569", 12.0f));
        row.children.push_back(text("phase6-settings-value-" + row_data.first, row_data.second, 64.0f, 18.0f, "#0f172a", 12.0f));
        right.children.push_back(std::move(row));
    }

    root.children.push_back(std::move(left));
    root.children.push_back(std::move(right));
    return {"settings-grid", 400, 176, fixture_ir("phase6-settings-grid", std::move(root))};
}

ScreenshotFixture make_clipped_stack_fixture() {
    auto root = frame("phase6-clipped-stack", 320.0f, 124.0f, LayoutDirection::column, "#181a20");
    root.style.overflow = "hidden";
    padding(root, 8.0f);
    root.layout.gap = 6.0f;

    for (int i = 0; i < 6; ++i) {
        auto row = frame("phase6-clip-row-" + std::to_string(i), 304.0f, 25.0f,
                         LayoutDirection::row, i % 2 == 0 ? "#242936" : "#1f2330");
        row.layout.align = LayoutAlign::center;
        row.layout.justify = LayoutAlign::space_between;
        padding(row, 5.0f);
        radius(row, 4.0f);
        row.children.push_back(text("phase6-clip-name-" + std::to_string(i), "Band " + std::to_string(i + 1),
                                    92.0f, 16.0f, "#cbd5e1", 12.0f));
        row.children.push_back(text("phase6-clip-value-" + std::to_string(i), std::to_string(120 + i * 35) + " Hz",
                                    84.0f, 16.0f, "#8fb3ff", 12.0f));
        root.children.push_back(std::move(row));
    }

    return {"clipped-stack", 320, 124, fixture_ir("phase6-clipped-stack", std::move(root))};
}

ScreenshotFixture make_typography_fixture() {
    auto root = frame("phase6-typography", 360.0f, 150.0f, LayoutDirection::column, "#241e32");
    padding(root, 14.0f);
    root.layout.gap = 7.0f;

    auto headline = text("phase6-typography-headline", "MOTION", 240.0f, 34.0f, "#f6f1ff", 26.0f);
    headline.style.font_weight = 700;
    headline.style.letter_spacing = 1.0f;
    root.children.push_back(std::move(headline));

    auto sub = text("phase6-typography-sub", "Depth-linked animation bus", 270.0f, 22.0f, "#cbbcf6", 15.0f);
    sub.style.font_style = "italic";
    root.children.push_back(std::move(sub));

    auto pills = frame("phase6-typography-pills", 332.0f, 40.0f, LayoutDirection::row);
    pills.layout.gap = 8.0f;
    for (const auto& pair : std::vector<std::pair<std::string, std::string>>{
             {"phase6-pill-a", "LOCKED"},
             {"phase6-pill-b", "BPM SYNC"},
             {"phase6-pill-c", "SMOOTH"}}) {
        auto pill = frame(pair.first, 96.0f, 32.0f, LayoutDirection::column, "#362a4a");
        pill.layout.justify = LayoutAlign::center;
        pill.layout.align = LayoutAlign::center;
        radius(pill, 6.0f);
        auto label = text(pair.first + "-label", pair.second, 82.0f, 18.0f, "#f2e7ff", 12.0f);
        label.style.text_align = "center";
        pill.children.push_back(std::move(label));
        pills.children.push_back(std::move(pill));
    }
    root.children.push_back(std::move(pills));

    return {"typography", 360, 150, fixture_ir("phase6-typography", std::move(root))};
}

ScreenshotFixture make_opacity_layers_fixture() {
    auto root = frame("phase6-opacity-layers", 340.0f, 170.0f, LayoutDirection::column, "#0f1320");
    padding(root, 12.0f);
    root.layout.gap = 10.0f;
    root.children.push_back(text("phase6-opacity-title", "Blend bus", 170.0f, 24.0f, "#eff6ff", 18.0f));

    auto row = frame("phase6-opacity-row", 316.0f, 104.0f, LayoutDirection::row);
    row.layout.gap = 10.0f;
    row.layout.align = LayoutAlign::center;

    auto muted = frame("phase6-opacity-muted", 148.0f, 92.0f, LayoutDirection::column, "#ff5a5f");
    muted.style.opacity = 0.62f;
    muted.layout.justify = LayoutAlign::center;
    muted.layout.align = LayoutAlign::center;
    radius(muted, 7.0f);
    muted.children.push_back(text("phase6-opacity-muted-label", "ducked", 100.0f, 22.0f, "#ffffff", 16.0f));

    auto bright = frame("phase6-opacity-bright", 148.0f, 92.0f, LayoutDirection::column, "#1fd1a5");
    bright.style.opacity = 0.88f;
    bright.layout.justify = LayoutAlign::center;
    bright.layout.align = LayoutAlign::center;
    radius(bright, 7.0f);
    bright.children.push_back(text("phase6-opacity-bright-label", "return", 100.0f, 22.0f, "#06131a", 16.0f));

    row.children.push_back(std::move(muted));
    row.children.push_back(std::move(bright));
    root.children.push_back(std::move(row));

    return {"opacity-layers", 340, 170, fixture_ir("phase6-opacity-layers", std::move(root))};
}

std::vector<ScreenshotFixture> make_fixtures() {
    return {
        make_control_strip_fixture(),
        make_meter_ladder_fixture(),
        make_settings_grid_fixture(),
        make_clipped_stack_fixture(),
        make_typography_fixture(),
        make_opacity_layers_fixture(),
    };
}

std::unique_ptr<View> build_live_view(const ScreenshotFixture& fixture) {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, static_cast<float>(fixture.width), static_cast<float>(fixture.height)});

    ScriptEngine engine;
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, *root, store);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.include_comments = false;
    opts.root_variable = "fixture";
    bridge.load_script(generate_pulp_js(fixture.ir, opts));
    return root;
}

std::unique_ptr<View> build_baked_view(const ScreenshotFixture& fixture,
                                       std::vector<ImportDiagnostic>& diagnostics) {
    auto baked = std::make_unique<View>();
    baked->set_bounds({0, 0, static_cast<float>(fixture.width), static_cast<float>(fixture.height)});

    auto materialized = build_native_view_tree(
        fixture.ir,
        fixture.ir.asset_manifest,
        {.diagnostics_out = &diagnostics});
    REQUIRE(materialized != nullptr);
    baked->add_child(std::move(materialized));
    return baked;
}

bool diagnostics_contain_error(const std::vector<ImportDiagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == ImportDiagnosticSeverity::error) return true;
    }
    return false;
}

std::string diagnostics_to_string(const std::vector<ImportDiagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.code << " " << diagnostic.path << " " << diagnostic.message << '\n';
    }
    return out.str();
}

bool screenshot_parity_supported() {
#if defined(__APPLE__) && (!defined(TARGET_OS_IOS) || !TARGET_OS_IOS)
    return true;
#else
    return false;
#endif
}

std::vector<uint8_t> render_probe_png(uint32_t width, uint32_t height) {
    View probe;
    probe.set_bounds({0, 0, static_cast<float>(width), static_cast<float>(height)});
    probe.set_background_color(Color::rgba8(255, 0, 255));
    return render_to_png(probe, width, height, 1.0f);
}

void require_meaningful_content(const std::string& label,
                                const std::vector<uint8_t>& png,
                                uint32_t min_unique_colors = 16,
                                double min_luminance_stddev = 1.0,
                                double min_non_background_coverage = 0.05,
                                double min_opaque_coverage = 0.95) {
    auto stats = analyze_screenshot_content(png);
    INFO(label
         << " content: valid=" << stats.valid
         << " size=" << stats.width << "x" << stats.height
         << " total_pixels=" << stats.total_pixels
         << " unique_colors=" << stats.unique_colors
         << " unique_colors_capped=" << stats.unique_colors_capped
         << " luminance_mean=" << stats.luminance_mean
         << " luminance_stddev=" << stats.luminance_stddev
         << " alpha_mean=" << stats.alpha_mean
         << " opaque_coverage=" << stats.opaque_coverage
         << " non_background_coverage=" << stats.non_background_coverage
         << " error=" << stats.error);

    REQUIRE(stats.valid);
    REQUIRE(stats.unique_colors >= min_unique_colors);
    REQUIRE(stats.luminance_stddev >= min_luminance_stddev);
    REQUIRE(stats.non_background_coverage >= min_non_background_coverage);
    REQUIRE(stats.opaque_coverage >= min_opaque_coverage);
}

} // namespace

TEST_CASE("design import screenshot parity content oracle rejects stable empty captures",
          "[view][import][screenshot-parity][content-oracle]") {
    if (!screenshot_parity_supported()) {
        const char* reason =
            "screenshot content oracle unavailable on this platform: Apple PNG decode backend is not available";
        std::cout << reason << '\n';
        SKIP(reason);
    }

    auto flat_png = render_probe_png(64, 64);
    REQUIRE_FALSE(flat_png.empty());

    auto stats = analyze_screenshot_content(flat_png);
    INFO("flat content: valid=" << stats.valid
                                << " unique_colors=" << stats.unique_colors
                                << " unique_colors_capped=" << stats.unique_colors_capped
                                << " luminance_stddev=" << stats.luminance_stddev
                                << " opaque_coverage=" << stats.opaque_coverage
                                << " non_background_coverage=" << stats.non_background_coverage
                                << " error=" << stats.error);
    REQUIRE(stats.valid);
    REQUIRE_FALSE(stats.passes_content_floor());
}

TEST_CASE("design import screenshot parity compares live and baked native fixtures",
          "[view][import][screenshot-parity][phase-6]") {
    if (!screenshot_parity_supported()) {
        const char* reason =
            "screenshot parity unavailable on this platform: Apple PNG decode backend is not available";
        std::cout << reason << '\n';
        SKIP(reason);
    }

    const auto fixtures = make_fixtures();
    REQUIRE(fixtures.size() >= 5);

    for (const auto& fixture : fixtures) {
        CAPTURE(fixture.name);

        auto live = build_live_view(fixture);
        std::vector<ImportDiagnostic> diagnostics;
        auto baked = build_baked_view(fixture, diagnostics);

        INFO(diagnostics_to_string(diagnostics));
        REQUIRE_FALSE(diagnostics_contain_error(diagnostics));

        auto live_png = render_to_png(*live, fixture.width, fixture.height, 1.0f);
        auto baked_png = render_to_png(*baked, fixture.width, fixture.height, 1.0f);
        REQUIRE_FALSE(live_png.empty());
        REQUIRE_FALSE(baked_png.empty());

        require_meaningful_content(fixture.name + " live", live_png);
        require_meaningful_content(fixture.name + " baked", baked_png);

        auto probe_png = render_probe_png(fixture.width, fixture.height);
        REQUIRE_FALSE(probe_png.empty());
        auto probe_result = compare_screenshots(probe_png, live_png, 8);
        INFO("probe similarity=" << probe_result.similarity
                                  << " diff_pixels=" << probe_result.diff_pixels
                                  << " total_pixels=" << probe_result.total_pixels
                                  << " error=" << probe_result.error);
        REQUIRE(probe_result.valid);
        REQUIRE(probe_result.similarity < 0.99f);

        auto baked_probe_result = compare_screenshots(probe_png, baked_png, 8);
        INFO("baked probe similarity=" << baked_probe_result.similarity
                                       << " diff_pixels=" << baked_probe_result.diff_pixels
                                       << " total_pixels=" << baked_probe_result.total_pixels
                                       << " error=" << baked_probe_result.error);
        REQUIRE(baked_probe_result.valid);
        REQUIRE(baked_probe_result.similarity < 0.99f);

        auto result = compare_screenshots(live_png, baked_png, 16);
        INFO("similarity=" << result.similarity
                           << " diff_pixels=" << result.diff_pixels
                           << " total_pixels=" << result.total_pixels
                           << " mean_error=" << result.mean_error
                           << " error=" << result.error);
        REQUIRE(result.valid);
        REQUIRE(result.passes(0.97f));
    }
}
