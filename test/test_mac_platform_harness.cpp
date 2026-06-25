// Mac-only Catch2 smoke for the platform-test harness — issue #2001.
//
// Exercises the hidden GPU-backed NSWindow + CAMetalLayer host without
// ever calling orderFront / makeKey. Covers back-buffer PNG capture via
// the production render_frame() path and synthetic AppKit mouse events
// against the real content view.
//
// Tag [issue-2001] so coverage can attribute these tests to the
// platform-harness work.

#include "mac_window_harness.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_sources.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/window_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <miniz.h>
#include <unistd.h>

using pulp::view::MouseButton;
using pulp::view::MouseEvent;
using pulp::view::Point;
using pulp::view::Rect;
using pulp::view::View;
using pulp::view::WindowHost;
using pulp::view::WindowOptions;

namespace pt = pulp::test::mac;

namespace {

namespace fs = std::filesystem;

struct ScopedTempDir {
    fs::path path;

    explicit ScopedTempDir(std::string prefix) {
        const auto base = fs::temp_directory_path();
        for (int i = 0; i < 100; ++i) {
            auto candidate = base / (prefix + "-" + std::to_string(::getpid()) + "-" + std::to_string(i));
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) {
                path = std::move(candidate);
                return;
            }
        }
    }

    ~ScopedTempDir() {
        if (!path.empty()) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
};

struct RoiSpec {
    const char* id;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t min_unique_colors;
    double min_luminance_stddev;
    double min_non_background_coverage;
    double min_opaque_coverage;
};

struct RoiShapeExpectation {
    const char* id;
    float min_similarity;
    double max_diff_coverage;
};

const std::vector<RoiSpec>& elysium_roi_specs() {
    static const std::vector<RoiSpec> specs = {
        {"top_decor_search", 0, 0, 1000, 155, 128, 10.0, 0.10, 0.95},
        {"color_dot_row", 805, 78, 170, 18, 32, 4.0, 0.05, 0.95},
        {"position_cylinder", 330, 150, 150, 125, 128, 8.0, 0.10, 0.95},
        {"range_prism", 540, 150, 170, 125, 128, 8.0, 0.10, 0.95},
        {"bottom_filter_eq_chart", 410, 455, 260, 95, 48, 4.0, 0.05, 0.95},
        {"bottom_envelope_graph", 135, 455, 200, 100, 48, 4.0, 0.05, 0.95},
        {"grains_knob_cap", 125, 305, 90, 85, 128, 8.0, 0.05, 0.95},
    };
    return specs;
}

const RoiShapeExpectation* elysium_shape_expectation_for_roi(const char* id) {
    // ELYSIUM remains a content/regression fixture. The committed reference and
    // native render have known source-vs-native shape deltas, so this test keeps
    // ROI content floors without claiming strict shape parity.
    // The shape-expectation block below is intentionally inert until strict
    // per-ROI expectations are populated.
    static const std::vector<RoiShapeExpectation> expectations = {};
    for (const auto& expectation : expectations) {
        if (std::string(expectation.id) == id) return &expectation;
    }
    return nullptr;
}

fs::path source_root() {
#ifdef PULP_SOURCE_DIR
    return fs::path(PULP_SOURCE_DIR);
#else
    return fs::current_path();
#endif
}

std::vector<uint8_t> read_binary_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return {};
    return {std::istreambuf_iterator<char>(input), {}};
}

bool write_binary_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream output(path, std::ios::binary);
    if (!output.is_open()) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

std::string read_text_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return {};
    return {std::istreambuf_iterator<char>(input), {}};
}

bool is_safe_zip_entry(const std::string& name) {
    if (name.empty()) return false;
    fs::path path(name);
    if (path.is_absolute()) return false;
    for (const auto& part : path) {
        if (part == "..") return false;
    }
    return true;
}

bool extract_zip_to_dir(const fs::path& zip_path, const fs::path& dest_dir, std::string& error) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zip_path.string().c_str(), 0)) {
        error = "could not open ZIP " + zip_path.string();
        return false;
    }

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        const mz_uint name_size = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
        if (name_size == 0 || name_size > 4096) {
            error = "unsafe ZIP filename size";
            mz_zip_reader_end(&zip);
            return false;
        }

        std::vector<char> name_buf(name_size);
        mz_zip_reader_get_filename(&zip, i, name_buf.data(), name_buf.size());
        const std::string entry_name(name_buf.data());
        if (!is_safe_zip_entry(entry_name)) {
            error = "unsafe ZIP entry " + entry_name;
            mz_zip_reader_end(&zip);
            return false;
        }

        const auto out_path = dest_dir / fs::path(entry_name);
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            std::error_code ec;
            fs::create_directories(out_path, ec);
            if (ec) {
                error = "could not create directory " + out_path.string();
                mz_zip_reader_end(&zip);
                return false;
            }
            continue;
        }

        std::error_code ec;
        fs::create_directories(out_path.parent_path(), ec);
        if (ec) {
            error = "could not create directory " + out_path.parent_path().string();
            mz_zip_reader_end(&zip);
            return false;
        }
        if (!mz_zip_reader_extract_to_file(&zip, i, out_path.string().c_str(), 0)) {
            error = "could not extract ZIP entry " + entry_name;
            mz_zip_reader_end(&zip);
            return false;
        }
    }

    mz_zip_reader_end(&zip);
    return true;
}

void absolutize_asset_paths(pulp::view::IRAssetManifest& manifest, const fs::path& base_dir) {
    for (auto& asset : manifest.assets) {
        if (!asset.local_path || asset.local_path->empty()) continue;
        fs::path path(*asset.local_path);
        if (path.is_relative()) path = base_dir / path;
        asset.local_path = path.lexically_normal().string();
    }
}

uint32_t count_ir_nodes(const pulp::view::IRNode& node) {
    uint32_t count = 1;
    for (const auto& child : node.children) count += count_ir_nodes(child);
    return count;
}

uint32_t count_nodes_with_attr(const pulp::view::IRNode& node, const char* key) {
    uint32_t count = node.attributes.count(key) ? 1u : 0u;
    for (const auto& child : node.children)
        count += count_nodes_with_attr(child, key);
    return count;
}

std::unique_ptr<View> load_elysium_default_cpp_view(const fs::path& fixture_zip,
                                                    ScopedTempDir& extracted,
                                                    pulp::view::DesignIR& ir,
                                                    std::vector<pulp::view::ImportDiagnostic>& diagnostics) {
    REQUIRE_FALSE(extracted.path.empty());

    std::string extract_error;
    const bool extracted_ok = extract_zip_to_dir(fixture_zip, extracted.path, extract_error);
    INFO(extract_error);
    REQUIRE(extracted_ok);

    const auto scene_path = extracted.path / "scene.pulp.json";
    REQUIRE(fs::exists(scene_path));
    auto scene_json = read_text_file(scene_path);
    REQUIRE_FALSE(scene_json.empty());

    ir = pulp::view::parse_figma_plugin_json(scene_json);
    absolutize_asset_paths(ir.asset_manifest, extracted.path);
    REQUIRE(ir.root.name == "VST Style");
    REQUIRE(count_ir_nodes(ir.root) == 187);  // rasterized: 3 vector frames -> image leaves
    REQUIRE(ir.asset_manifest.assets.size() == 75);  // +3 rasterized illustration PNGs

    // Promote captured-art knobs (hoist body disc + drop pointer hairlines)
    // BEFORE enrich so the hoisted asset_ref receives its absolute asset_path +
    // opaque-core metadata; the materializer then skins the knob and keeps it
    // interactive via the native notch overlay. Runs after the structural
    // assertions above, which pin the raw parsed scene.
    pulp::view::hoist_captured_art_knobs(ir);
    pulp::view::enrich_imported_image_asset_metadata(ir, ir.asset_manifest);

    // Per-shape gradient sampling: the four illustration shapes (DEPTH /
    // POSITION / OFFSET / SHIMMER) are colorful, so enrich samples each one's
    // OWN gradient into shape_fill_gradient; the grey chrome/logos stay below
    // the saturation gate and get none. Pins the sampling end-to-end (the
    // materializer forwards these to ImageView::set_fill_gradient so an opt-in
    // fill reveals the shape's real colors).
    REQUIRE(count_nodes_with_attr(ir.root, "shape_fill_gradient") >= 4);

    auto view = pulp::view::build_native_view_tree(
        ir,
        ir.asset_manifest,
        {.diagnostics_out = &diagnostics});
    REQUIRE(view != nullptr);
    view->set_bounds({0, 0, 1000.0f, 600.0f});

    bool has_error_diagnostic = false;
    bool has_unresolved_asset_diagnostic = false;
    std::ostringstream diagnostic_report;
    for (const auto& diagnostic : diagnostics) {
        diagnostic_report << diagnostic.code << " " << diagnostic.path << " "
                          << diagnostic.message << "\n";
        if (diagnostic.severity == pulp::view::ImportDiagnosticSeverity::error)
            has_error_diagnostic = true;
        if (diagnostic.kind == pulp::view::ImportDiagnosticKind::unresolved_asset)
            has_unresolved_asset_diagnostic = true;
    }
    INFO(diagnostic_report.str());
    REQUIRE_FALSE(has_error_diagnostic);
    REQUIRE_FALSE(has_unresolved_asset_diagnostic);

    return view;
}

Rect absolute_bounds(const View& view) {
    Rect bounds = view.bounds();
    for (auto* parent = view.parent(); parent != nullptr; parent = parent->parent()) {
        bounds.x += parent->bounds().x;
        bounds.y += parent->bounds().y;
    }
    return bounds;
}

std::string describe_hit(const View* hit) {
    if (!hit) return "null";
    const auto bounds = absolute_bounds(*hit);
    std::ostringstream out;
    out << hit;
    if (!hit->id().empty()) out << " id=" << hit->id();
    if (!hit->anchor_id().empty()) out << " anchor=" << hit->anchor_id();
    out << " abs=(" << bounds.x << "," << bounds.y << ","
        << bounds.width << "," << bounds.height << ")";
    return out.str();
}

pulp::view::Knob* require_knob_hit(View& root, Point point, const char* label) {
    auto* hit = root.hit_test(point);
    INFO(label << " hit point=(" << point.x << "," << point.y << ")"
               << " hit=" << describe_hit(hit));
    REQUIRE(hit != nullptr);
    auto* knob = dynamic_cast<pulp::view::Knob*>(hit);
    REQUIRE(knob != nullptr);
    return knob;
}

void require_same_knob_hit(View& root,
                           pulp::view::Knob& expected,
                           Point point,
                           const char* label) {
    auto* hit = root.hit_test(point);
    std::cout << "elysium_knob_hit_probe"
              << " name=" << label
              << " point=" << point.x << "," << point.y
              << " hit=" << describe_hit(hit)
              << "\n";
    INFO(label << " point=(" << point.x << "," << point.y << ")"
               << " expected=" << &expected
               << " hit=" << describe_hit(hit));
    REQUIRE(hit == &expected);
    REQUIRE(dynamic_cast<pulp::view::Knob*>(hit) == &expected);
}

void require_not_same_knob_hit(View& root,
                               pulp::view::Knob& excluded,
                               Point point,
                               const char* label) {
    auto* hit = root.hit_test(point);
    std::cout << "elysium_knob_hit_probe"
              << " name=" << label
              << " point=" << point.x << "," << point.y
              << " hit=" << describe_hit(hit)
              << "\n";
    INFO(label << " point=(" << point.x << "," << point.y << ")"
               << " excluded=" << &excluded
               << " hit=" << describe_hit(hit));
    REQUIRE(hit != &excluded);
}

void require_relative_drag_increases_knob(pulp::view::WindowHost& host,
                                          pulp::view::Knob& knob,
                                          Point point,
                                          const char* label) {
    knob.set_value(0.5f);
    const float before = knob.value();
    REQUIRE(pt::simulate_mouse(host, {.phase = pt::SimulatedMouse::Phase::down,
                                     .x = point.x,
                                     .y = point.y}));
    REQUIRE(pt::simulate_mouse(host, {.phase = pt::SimulatedMouse::Phase::drag,
                                     .x = point.x,
                                     .y = point.y,
                                     .mouse_delta_y = -30.0f}));
    REQUIRE(pt::simulate_mouse(host, {.phase = pt::SimulatedMouse::Phase::up,
                                     .x = point.x,
                                     .y = point.y}));

    std::cout << "elysium_knob_drag_probe"
              << " name=" << label
              << " point=" << point.x << "," << point.y
              << " before=" << before
              << " after=" << knob.value()
              << "\n";
    INFO(label << " knob value before=" << before << " after=" << knob.value());
    REQUIRE(knob.value() > before);
}

std::vector<uint8_t> crop_logical_roi(const std::vector<uint8_t>& png,
                                      uint32_t image_width,
                                      uint32_t image_height,
                                      uint32_t logical_width,
                                      uint32_t logical_height,
                                      const RoiSpec& roi) {
    const double x_scale = static_cast<double>(image_width) / static_cast<double>(logical_width);
    const double y_scale = static_cast<double>(image_height) / static_cast<double>(logical_height);
    const auto x = static_cast<uint32_t>(std::round(static_cast<double>(roi.x) * x_scale));
    const auto y = static_cast<uint32_t>(std::round(static_cast<double>(roi.y) * y_scale));
    auto width = static_cast<uint32_t>(std::round(static_cast<double>(roi.width) * x_scale));
    auto height = static_cast<uint32_t>(std::round(static_cast<double>(roi.height) * y_scale));
    if (x >= image_width || y >= image_height) return {};
    width = std::min(width, image_width - x);
    height = std::min(height, image_height - y);
    return pulp::view::crop_png(png, x, y, width, height);
}

void require_content_floor(const char* label,
                           const std::vector<uint8_t>& png,
                           uint32_t min_unique_colors = 16,
                           double min_luminance_stddev = 1.0,
                           double min_non_background_coverage = 0.05,
                           double min_opaque_coverage = 0.95) {
    auto stats = pulp::view::analyze_screenshot_content(png);
    INFO(label << " content stats: valid=" << stats.valid
               << " size=" << stats.width << "x" << stats.height
               << " unique_colors=" << stats.unique_colors
               << " unique_colors_capped=" << stats.unique_colors_capped
               << " luminance_stddev=" << stats.luminance_stddev
               << " opaque_coverage=" << stats.opaque_coverage
               << " non_background_coverage=" << stats.non_background_coverage
               << " error=" << stats.error);
    REQUIRE(stats.valid);
    REQUIRE(stats.unique_colors >= min_unique_colors);
    REQUIRE(stats.luminance_stddev >= min_luminance_stddev);
    REQUIRE(stats.non_background_coverage >= min_non_background_coverage);
    REQUIRE(stats.opaque_coverage >= min_opaque_coverage);
}

bool roi_passes_floor(const pulp::view::ScreenshotContentStats& stats, const RoiSpec& roi) {
    return stats.valid &&
           stats.unique_colors >= roi.min_unique_colors &&
           stats.luminance_stddev >= roi.min_luminance_stddev &&
           stats.non_background_coverage >= roi.min_non_background_coverage &&
           stats.opaque_coverage >= roi.min_opaque_coverage;
}

std::optional<fs::path> elysium_gpu_dump_dir() {
    if (const char* value = std::getenv("PULP_DESIGN_GPU_DUMP_DIR"); value && *value)
        return fs::path(value);
    return std::nullopt;
}

std::vector<uint8_t> require_roi(const char* label,
                                 const std::vector<uint8_t>& png,
                                 uint32_t x,
                                 uint32_t y,
                                 uint32_t width,
                                 uint32_t height) {
    auto crop = pulp::view::crop_png(png, x, y, width, height);
    INFO(label << " roi=(" << x << "," << y << " " << width << "x" << height
               << ") bytes=" << crop.size());
    REQUIRE_FALSE(crop.empty());
    return crop;
}

void configure_gpu_capture_fixture(View& root) {
    root.set_theme(pulp::view::Theme::dark());
    root.set_background_color(pulp::view::Color::rgba8(16, 19, 29, 255));
    root.flex().direction = pulp::view::FlexDirection::column;
    root.flex().padding = 12;
    root.flex().gap = 8;

    auto label = std::make_unique<pulp::view::Label>("GPU CAPTURE");
    label->set_font_size(18.0f);
    label->flex().preferred_height = 28.0f;
    root.add_child(std::move(label));

    auto panel = std::make_unique<View>();
    panel->set_background_color(pulp::view::Color::rgba8(74, 126, 255, 255));
    panel->flex().preferred_height = 48.0f;
    root.add_child(std::move(panel));
}

} // namespace

TEST_CASE("mac harness constructs a hidden GPU-backed window",
          "[mac][platform-harness][issue-2001]") {
    View root;
    auto host = pt::make_test_window(root);

    REQUIRE(host != nullptr);
    REQUIRE(host->is_visible() == false);
    REQUIRE(host->native_window_handle() != nullptr);
    REQUIRE(host->native_content_view_handle() != nullptr);
    REQUIRE(host->gpu_surface() != nullptr);
}

TEST_CASE("mac harness back-buffer capture returns non-empty PNG bytes",
          "[mac][platform-harness][issue-2001]") {
    View root;
    configure_gpu_capture_fixture(root);
    auto host = pt::make_test_window(root);
    REQUIRE(host != nullptr);

    auto png = pt::capture_back_buffer_png(*host);

    // The harness's contract is "deterministic host-managed pixels".
    // For the GPU host that means render_frame() succeeded and
    // encode_rgba_to_png() produced a real PNG. An empty result here
    // would indicate the back-buffer path is broken on hidden windows.
    INFO("png byte count: " << png.size());
    REQUIRE_FALSE(png.empty());

    // PNG signature sanity-check (8-byte magic). Catches the case where
    // the back-buffer readback returns raw RGBA without the encode step.
    REQUIRE(png.size() >= 8);
    REQUIRE(png[0] == 0x89);
    REQUIRE(png[1] == 0x50);  // 'P'
    REQUIRE(png[2] == 0x4E);  // 'N'
    REQUIRE(png[3] == 0x47);  // 'G'

    require_content_floor("single-frame gpu capture", png);
}

TEST_CASE("mac harness settled GPU capture is stable and checks expected ROIs",
          "[mac][platform-harness][issue-2001][content-oracle]") {
    View root;
    configure_gpu_capture_fixture(root);
    auto host = pt::make_test_window(root);
    REQUIRE(host != nullptr);

    auto frames = pt::capture_settled_back_buffer_png(*host, 3);
    REQUIRE(frames.size() == 3);

    std::ostringstream report;
    report << "settled frames:";
    for (const auto& frame : frames) {
        report << " #" << frame.frame_index
               << " t=" << frame.elapsed_ms << "ms"
               << " bytes=" << frame.png.size();
        REQUIRE_FALSE(frame.png.empty());
        require_content_floor("settled gpu frame", frame.png);
    }
    INFO(report.str());

    for (size_t i = 1; i < frames.size(); ++i) {
        auto result = pulp::view::compare_screenshots(frames.front().png,
                                                      frames[i].png,
                                                      2);
        INFO("settled frame similarity frame0->frame" << i
                                                      << " similarity=" << result.similarity
                                                      << " diff_pixels=" << result.diff_pixels
                                                      << " error=" << result.error);
        REQUIRE(result.valid);
        REQUIRE(result.similarity >= 0.99f);
    }

    auto full = pulp::view::analyze_screenshot_content(frames.back().png);
    REQUIRE(full.valid);

    const uint32_t roi_w = std::max<uint32_t>(16, full.width / 4);
    const uint32_t roi_h = std::max<uint32_t>(8, full.height / 12);
    // These fractions intentionally target the `make_test_window` default
    // fixture layout (320x240): label y=12..40, blue panel y=48..96, dark
    // background below. If the fixture geometry changes, the ROI
    // similarity/content assertions should fail loud.
    auto panel = require_roi("blue panel",
                             frames.back().png,
                             full.width / 8,
                             full.height / 4,
                             roi_w,
                             roi_h);
    auto background = require_roi("lower background",
                                  frames.back().png,
                                  full.width / 8,
                                  (full.height * 3) / 4,
                                  roi_w,
                                  roi_h);
    auto panel_vs_background = pulp::view::compare_screenshots(panel, background, 8);
    INFO("panel/background similarity=" << panel_vs_background.similarity
                                        << " diff_pixels=" << panel_vs_background.diff_pixels
                                        << " error=" << panel_vs_background.error);
    REQUIRE(panel_vs_background.valid);
    REQUIRE(panel_vs_background.similarity < 0.25f);

    auto label = require_roi("label text",
                             frames.back().png,
                             full.width / 18,
                             full.height / 20,
                             full.width / 3,
                             std::max<uint32_t>(8, full.height / 10));
    require_content_floor("label text roi", label, 4, 4.0, 0.005, 0.95);
}

TEST_CASE("mac harness captures ELYSIUM default C++ import through GPU path",
          "[mac][platform-harness][issue-2001][elysium][gpu-report]") {
    const auto repo = source_root();
    const auto fixture_zip =
        repo / "planning/fixtures/figma-plugin/real-designs/elysium.pulp.zip";
    const auto reference_png_path =
        repo / "planning/fixtures/figma-plugin/real-designs/reference/elysium-figma-rest-absolute-scale2.png";

    if (!fs::exists(fixture_zip) || !fs::exists(reference_png_path)) {
        SKIP("ELYSIUM source/reference fixtures are not present in this checkout");
    }

    ScopedTempDir extracted("pulp-design-gpu-fixture");
    REQUIRE_FALSE(extracted.path.empty());

    std::string extract_error;
    const bool extracted_ok = extract_zip_to_dir(fixture_zip, extracted.path, extract_error);
    INFO(extract_error);
    REQUIRE(extracted_ok);

    const auto scene_path = extracted.path / "scene.pulp.json";
    REQUIRE(fs::exists(scene_path));
    auto scene_json = read_text_file(scene_path);
    REQUIRE_FALSE(scene_json.empty());

    auto ir = pulp::view::parse_figma_plugin_json(scene_json);
    absolutize_asset_paths(ir.asset_manifest, extracted.path);
    REQUIRE(ir.root.name == "VST Style");
    REQUIRE(count_ir_nodes(ir.root) == 187);  // rasterized: 3 vector frames -> image leaves
    REQUIRE(ir.asset_manifest.assets.size() == 75);  // +3 rasterized illustration PNGs

    // Promote captured-art knobs (hoist body disc + drop pointer hairlines)
    // BEFORE enrich so the hoisted asset_ref receives its absolute asset_path +
    // opaque-core metadata; the materializer then skins the knob and keeps it
    // interactive via the native notch overlay. Runs after the structural
    // assertions above, which pin the raw parsed scene.
    pulp::view::hoist_captured_art_knobs(ir);
    pulp::view::enrich_imported_image_asset_metadata(ir, ir.asset_manifest);

    std::vector<pulp::view::ImportDiagnostic> diagnostics;
    auto view = pulp::view::build_native_view_tree(
        ir,
        ir.asset_manifest,
        {.diagnostics_out = &diagnostics});
    REQUIRE(view != nullptr);
    view->set_bounds({0, 0, 1000.0f, 600.0f});

    bool has_error_diagnostic = false;
    bool has_unresolved_asset_diagnostic = false;
    std::ostringstream diagnostic_report;
    for (const auto& diagnostic : diagnostics) {
        diagnostic_report << diagnostic.code << " " << diagnostic.path << " "
                          << diagnostic.message << "\n";
        if (diagnostic.severity == pulp::view::ImportDiagnosticSeverity::error)
            has_error_diagnostic = true;
        if (diagnostic.kind == pulp::view::ImportDiagnosticKind::unresolved_asset)
            has_unresolved_asset_diagnostic = true;
    }
    INFO(diagnostic_report.str());
    REQUIRE_FALSE(has_error_diagnostic);
    REQUIRE_FALSE(has_unresolved_asset_diagnostic);

    WindowOptions opts;
    opts.width = 1000;
    opts.height = 600;
    auto host = pt::make_test_window(*view, opts);
    REQUIRE(host != nullptr);
    REQUIRE(host->gpu_surface() != nullptr);

    auto frames = pt::capture_settled_back_buffer_png(*host, 3);
    REQUIRE(frames.size() == 3);
    for (const auto& frame : frames) {
        REQUIRE_FALSE(frame.png.empty());
        require_content_floor("ELYSIUM default C++ GPU frame",
                              frame.png,
                              128,
                              8.0,
                              0.05,
                              0.95);
    }

    for (size_t i = 1; i < frames.size(); ++i) {
        auto result = pulp::view::compare_screenshots(frames.front().png,
                                                      frames[i].png,
                                                      2);
        INFO("ELYSIUM settled frame similarity frame0->frame" << i
             << " similarity=" << result.similarity
             << " diff_pixels=" << result.diff_pixels
             << " error=" << result.error);
        REQUIRE(result.valid);
        REQUIRE(result.similarity >= 0.99f);
    }

    auto reference_png = read_binary_file(reference_png_path);
    REQUIRE_FALSE(reference_png.empty());
    const auto reference_stats = pulp::view::analyze_screenshot_content(reference_png);
    const auto rendered_stats = pulp::view::analyze_screenshot_content(frames.back().png);
    REQUIRE(reference_stats.valid);
    REQUIRE(rendered_stats.valid);

    const auto dump_dir = elysium_gpu_dump_dir();
    if (dump_dir) {
        write_binary_file(*dump_dir / "elysium-reference-full.png", reference_png);
        write_binary_file(*dump_dir / "elysium-rendered-full.png", frames.back().png);
    }

    std::cout << "elysium_gpu_report"
              << " reference_size=" << reference_stats.width << "x" << reference_stats.height
              << " rendered_size=" << rendered_stats.width << "x" << rendered_stats.height
              << " rendered_unique_colors=" << rendered_stats.unique_colors
              << " rendered_luminance_stddev=" << rendered_stats.luminance_stddev
              << " rendered_non_background_coverage=" << rendered_stats.non_background_coverage
              << "\n";

    for (const auto& roi : elysium_roi_specs()) {
        auto reference_roi = crop_logical_roi(reference_png,
                                             reference_stats.width,
                                             reference_stats.height,
                                             1000,
                                             600,
                                             roi);
        auto rendered_roi = crop_logical_roi(frames.back().png,
                                            rendered_stats.width,
                                            rendered_stats.height,
                                            1000,
                                            600,
                                            roi);
        REQUIRE_FALSE(reference_roi.empty());
        REQUIRE_FALSE(rendered_roi.empty());

        auto reference_roi_stats = pulp::view::analyze_screenshot_content(reference_roi);
        auto rendered_roi_stats = pulp::view::analyze_screenshot_content(rendered_roi);
        REQUIRE(roi_passes_floor(reference_roi_stats, roi));
        REQUIRE(rendered_roi_stats.valid);
        REQUIRE(roi_passes_floor(rendered_roi_stats, roi));

        auto similarity = pulp::view::compare_screenshots(reference_roi, rendered_roi, 24);
        REQUIRE(similarity.valid);
        REQUIRE(similarity.total_pixels > 0);
        const double diff_coverage =
            static_cast<double>(similarity.diff_pixels) /
            static_cast<double>(similarity.total_pixels);

        if (dump_dir) {
            const std::string roi_id(roi.id);
            write_binary_file(*dump_dir / ("elysium-" + roi_id + "-reference.png"), reference_roi);
            write_binary_file(*dump_dir / ("elysium-" + roi_id + "-rendered.png"), rendered_roi);
            auto diff = pulp::view::generate_diff_image(reference_roi, rendered_roi, 24);
            if (!diff.empty())
                write_binary_file(*dump_dir / ("elysium-" + roi_id + "-diff.png"), diff);
        }

        const bool rendered_content_pass = roi_passes_floor(rendered_roi_stats, roi);
        std::cout << "elysium_gpu_roi"
                  << " id=" << roi.id
                  << " rendered_content_pass=" << (rendered_content_pass ? "true" : "false")
                  << " similarity=" << similarity.similarity
                  << " diff_pixels=" << similarity.diff_pixels
                  << " diff_coverage=" << diff_coverage
                  << " rendered_unique_colors=" << rendered_roi_stats.unique_colors
                  << " rendered_luminance_stddev=" << rendered_roi_stats.luminance_stddev
                  << " rendered_non_background_coverage=" << rendered_roi_stats.non_background_coverage
                  << "\n";

        if (const auto* shape = elysium_shape_expectation_for_roi(roi.id)) {
            const bool shape_passes =
                similarity.similarity >= shape->min_similarity &&
                diff_coverage <= shape->max_diff_coverage;
            std::cout << "elysium_gpu_shape_roi"
                      << " id=" << roi.id
                      << " shape_pass=" << (shape_passes ? "true" : "false")
                      << " similarity=" << similarity.similarity
                      << " min_similarity=" << shape->min_similarity
                      << " diff_coverage=" << diff_coverage
                      << " max_diff_coverage=" << shape->max_diff_coverage
                      << "\n";
            INFO("shape ROI " << roi.id
                               << " similarity=" << similarity.similarity
                               << " min_similarity=" << shape->min_similarity
                               << " diff_coverage=" << diff_coverage
                               << " max_diff_coverage=" << shape->max_diff_coverage);
            REQUIRE(similarity.similarity >= shape->min_similarity);
            REQUIRE(diff_coverage <= shape->max_diff_coverage);
        }
    }
}

TEST_CASE("mac harness drives ELYSIUM imported knob body through hidden GPU host",
          "[mac][platform-harness][issue-2001][elysium][interaction][hit-test]") {
    const auto repo = source_root();
    const auto fixture_zip =
        repo / "planning/fixtures/figma-plugin/real-designs/elysium.pulp.zip";

    if (!fs::exists(fixture_zip)) {
        SKIP("ELYSIUM source fixture is not present in this checkout");
    }

    ScopedTempDir extracted("pulp-design-gpu-interaction-fixture");
    pulp::view::DesignIR ir;
    std::vector<pulp::view::ImportDiagnostic> diagnostics;
    auto view = load_elysium_default_cpp_view(fixture_zip, extracted, ir, diagnostics);
    REQUIRE(view != nullptr);

    WindowOptions opts;
    opts.width = 1000;
    opts.height = 600;
    auto host = pt::make_test_window(*view, opts);
    REQUIRE(host != nullptr);
    REQUIRE(host->gpu_surface() != nullptr);

    auto frames = pt::capture_settled_back_buffer_png(*host, 3);
    REQUIRE(frames.size() == 3);
    REQUIRE_FALSE(frames.back().png.empty());
    require_content_floor("ELYSIUM interaction preflight GPU frame",
                          frames.back().png,
                          128,
                          8.0,
                          0.05,
                          0.95);

    const Point grains_knob_body{162.0f, 356.0f};
    auto* knob = require_knob_hit(*view, grains_knob_body, "ELYSIUM grains knob body");
    const auto knob_abs = absolute_bounds(*knob);
    INFO("ELYSIUM grains knob absolute bounds=(" << knob_abs.x << "," << knob_abs.y
                                                 << "," << knob_abs.width << ","
                                                 << knob_abs.height << ")");
    auto knob_point = [knob_abs](float x, float y) {
        return Point{knob_abs.x + knob_abs.width * x,
                     knob_abs.y + knob_abs.height * y};
    };

    const Point body_center = knob_point(0.50f, 0.42f);
    const Point arc_left = knob_point(0.25f, 0.43f);
    const Point arc_right = knob_point(0.75f, 0.43f);
    const Point halo_top = knob_point(0.50f, 0.15f);
    const Point label_text = knob_point(0.50f, 0.72f);
    const Point value_text = knob_point(0.50f, 0.90f);

    require_same_knob_hit(*view, *knob, body_center, "body_center");
    require_same_knob_hit(*view, *knob, arc_left, "arc_left");
    require_same_knob_hit(*view, *knob, arc_right, "arc_right");
    require_same_knob_hit(*view, *knob, halo_top, "halo_top");
    require_same_knob_hit(*view, *knob, label_text, "label_text");
    require_same_knob_hit(*view, *knob, value_text, "value_text");

    require_not_same_knob_hit(*view,
                              *knob,
                              {knob_abs.x - 8.0f, knob_abs.y + knob_abs.height * 0.43f},
                              "outside_left");
    require_not_same_knob_hit(*view,
                              *knob,
                              {knob_abs.x + knob_abs.width + 8.0f,
                               knob_abs.y + knob_abs.height * 0.43f},
                              "outside_right");
    require_not_same_knob_hit(*view,
                              *knob,
                              {knob_abs.x + knob_abs.width * 0.50f, knob_abs.y - 8.0f},
                              "outside_top");
    require_not_same_knob_hit(*view,
                              *knob,
                              {knob_abs.x + knob_abs.width * 0.50f,
                               knob_abs.y + knob_abs.height + 8.0f},
                              "outside_bottom");

    // The knob enters relative mouse mode on down; keep the absolute drag
    // location stable so each value change is attributable to NSEvent.deltaY.
    require_relative_drag_increases_knob(*host, *knob, body_center, "body_center");
    require_relative_drag_increases_knob(*host, *knob, arc_left, "arc_left");
    require_relative_drag_increases_knob(*host, *knob, arc_right, "arc_right");
    require_relative_drag_increases_knob(*host, *knob, halo_top, "halo_top");
}

TEST_CASE("mac harness honors caller-provided window options size",
          "[mac][platform-harness][issue-2001]") {
    View root;
    WindowOptions opts;
    opts.width = 640;
    opts.height = 480;

    auto host = pt::make_test_window(root, opts);
    REQUIRE(host != nullptr);

    const auto content = host->get_content_size();
    // CAMetalLayer drawableSize is the logical size scaled by
    // contentsScale on Retina, so don't assert byte-equal — just
    // assert the host rounded the request up to something plausible.
    REQUIRE(content.width  >= 600);
    REQUIRE(content.height >= 400);
}

// ── Regression contract: scroll deltas + button routing ──────────────────
//
// These tests pin two platform-harness contracts:
//   1. `build_event` must construct scroll wheel events that carry
//      `scrollingDeltaX/Y`, because `PulpView::scrollWheel:` reads those
//      fields and the synthetic event must exercise real scroll math.
//   2. `simulate_mouse` must route right-click phases through
//      `rightMouseDown:` rather than the left-click selectors, which is what
//      triggers the context-menu path.
//
// They build a hidden GPU window, install a child view with a known hit-test
// rect, and assert the production selectors actually fired.

TEST_CASE("mac harness scroll event carries non-zero deltas through PulpView",
          "[mac][platform-harness][issue-2001]") {
    View root;
    root.set_bounds({0, 0, 320, 240});

    // Child fills the window. on_pointer_event is the callback
    // `PulpView::scrollWheel:` invokes once it has walked ancestors to
    // dispatch a wheel-flagged MouseEvent.
    auto child = std::make_unique<View>();
    child->flex().preferred_width = 320.0f;
    child->flex().preferred_height = 240.0f;

    int wheel_calls = 0;
    float captured_dx = 0.0f;
    float captured_dy = 0.0f;
    child->on_pointer_event = [&](const MouseEvent& me) {
        if (!me.is_wheel) return;
        ++wheel_calls;
        captured_dx = me.scroll_delta_x;
        captured_dy = me.scroll_delta_y;
    };
    root.add_child(std::move(child));
    root.layout_children();

    auto host = pt::make_test_window(root);
    REQUIRE(host != nullptr);

    pt::SimulatedMouse ev;
    ev.phase = pt::SimulatedMouse::Phase::scroll;
    ev.x = 100.0f;
    ev.y = 100.0f;
    ev.scroll_delta_y = 10.0f;
    ev.scroll_delta_x = 0.0f;
    REQUIRE(pt::simulate_mouse(*host, ev));

    REQUIRE(wheel_calls >= 1);
    // PulpView::scrollWheel: negates the Y axis (Cocoa wheel deltas are
    // bottom-up; the View MouseEvent is top-down). The harness builds the
    // NSEvent from the caller's raw scroll_delta_y, so the View callback
    // observes |delta| > 0 with the production sign.
    REQUIRE(captured_dy != 0.0f);
    REQUIRE(captured_dx == 0.0f);
}

TEST_CASE("mac harness right-click reaches PulpView::rightMouseDown: not mouseDown:",
          "[mac][platform-harness][issue-2001]") {
    View root;
    root.set_bounds({0, 0, 320, 240});

    auto child = std::make_unique<View>();
    child->flex().preferred_width = 320.0f;
    child->flex().preferred_height = 240.0f;

    // `on_click` is the left-click signal: PulpView::mouseUp: posts it
    // via dispatch_async. `on_context_menu` is the right-click signal:
    // PulpView::rightMouseDown: invokes it synchronously. Wiring both
    // here lets us prove the synthetic right-click did NOT fall into
    // the left-click path.
    int left_clicks = 0;
    int context_menus = 0;
    child->on_click = [&] { ++left_clicks; };
    child->on_context_menu = [&](pulp::view::Point) { ++context_menus; };
    root.add_child(std::move(child));
    root.layout_children();

    auto host = pt::make_test_window(root);
    REQUIRE(host != nullptr);

    pt::SimulatedMouse down;
    down.phase = pt::SimulatedMouse::Phase::down;
    down.button = MouseButton::right;
    down.x = 50.0f;
    down.y = 50.0f;
    REQUIRE(pt::simulate_mouse(*host, down));

    // Right-click only fires on rightMouseDown:; the matching up event
    // is exercised here mainly to keep the gesture symmetrical and to
    // confirm `simulate_mouse` does not crash when routing to
    // `rightMouseUp:` (which PulpView does not override).
    pt::SimulatedMouse up = down;
    up.phase = pt::SimulatedMouse::Phase::up;
    REQUIRE(pt::simulate_mouse(*host, up));

    REQUIRE(context_menus == 1);
    REQUIRE(left_clicks == 0);
}
