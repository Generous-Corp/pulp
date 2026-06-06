// ELYSIUM standalone — imports the real figma-plugin ELYSIUM .pulp.zip from
// scratch and renders it as native Pulp UI in a GPU window. The GRAINS knobs
// come up as the design's captured metallic discs (sprite-skinned) with a
// native rotating indicator notch, and they TURN under the mouse.
//
// This is the production wiring of the sprite-knob path: the import pipeline
// calls hoist_captured_art_knobs() (which promotes a captured-art knob to a
// skinned-but-turnable native Knob) BEFORE enrich_imported_image_asset_metadata,
// then materializes via build_native_view_tree — the same path proven by the
// mac-platform-harness, now in a launchable app.
//
//   pulp-elysium-standalone                       # open the window, turn knobs
//   pulp-elysium-standalone /path/to/scene.pulp.zip
//   pulp-elysium-standalone --screenshot=out.png  # headless capture
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/window_host.hpp>

#include <algorithm>
#include <functional>
#include <tuple>

#include <miniz.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// A zip entry is safe when it stays inside the destination (no absolute paths,
// no `..` traversal). The committed fixture is trusted; this guards against a
// malformed user-supplied zip.
bool is_safe_zip_entry(const std::string& name) {
    if (name.empty() || name.front() == '/') return false;
    if (name.find("..") != std::string::npos) return false;
    return true;
}

bool extract_zip_to_dir(const fs::path& zip_path, const fs::path& dest_dir,
                        std::string& error) {
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
            continue;
        }
        std::error_code ec;
        fs::create_directories(out_path.parent_path(), ec);
        if (!mz_zip_reader_extract_to_file(&zip, i, out_path.string().c_str(), 0)) {
            error = "could not extract ZIP entry " + entry_name;
            mz_zip_reader_end(&zip);
            return false;
        }
    }
    mz_zip_reader_end(&zip);
    return true;
}

void absolutize_asset_paths(pulp::view::IRAssetManifest& manifest,
                            const fs::path& base_dir) {
    for (auto& asset : manifest.assets) {
        if (!asset.local_path || asset.local_path->empty()) continue;
        fs::path path(*asset.local_path);
        if (path.is_relative()) path = base_dir / path;
        asset.local_path = path.lexically_normal().string();
    }
}

std::string read_text_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// Generic post-import wiring (NOT baked into the importer): pair each upper
// illustration shape with its column knob by laid-out x-position, then drive the
// shape's value-driven silhouette fill from the knob's value. This is exactly
// the kind of customization an "after-import" step would do — the import only
// provides the generic ImageView::set_fill_value primitive, and the per-shape
// gradient (each shape's OWN colors) flows through automatically because the
// materializer already called set_fill_gradient from the importer-sampled
// shape_fill_gradient. Requires layout to have run (real bounds).
void apply_shape_knob_fills(pulp::view::View* root, float design_h) {
    using namespace pulp::view;
    std::vector<std::pair<float, Knob*>> knobs;
    std::vector<std::pair<float, ImageView*>> shapes;
    std::function<void(View*, float, float)> walk =
        [&](View* v, float ox, float oy) {
            const auto bn = v->bounds();
            const float gx = ox + bn.x, gy = oy + bn.y;
            if (auto* k = dynamic_cast<Knob*>(v)) {
                if (bn.width > 45.0f) knobs.emplace_back(gx + bn.width * 0.5f, k);
            } else if (auto* img = dynamic_cast<ImageView*>(v)) {
                if (bn.width > 50.0f && bn.height > 50.0f && gy < design_h * 0.45f)
                    shapes.emplace_back(gx + bn.width * 0.5f, img);
            }
            for (std::size_t i = 0; i < v->child_count(); ++i)
                walk(v->child_at(i), gx, gy);
        };
    walk(root, 0.0f, 0.0f);
    std::sort(knobs.begin(), knobs.end(),
              [](auto& a, auto& b) { return a.first < b.first; });
    std::sort(shapes.begin(), shapes.end(),
              [](auto& a, auto& b) { return a.first < b.first; });
    const std::size_t n = std::min(knobs.size(), shapes.size());
    for (std::size_t i = 0; i < n; ++i)
        shapes[i].second->set_fill_value(knobs[i].second->value());
}

}  // namespace

int main(int argc, char** argv) {
    std::string screenshot_path;
    std::string raster_path;  // headless Skia-raster preview (no GPU window)
    fs::path zip_path = PULP_ELYSIUM_DEFAULT_ZIP;  // committed fixture (CMake)
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        constexpr std::string_view shot = "--screenshot=";
        constexpr std::string_view raster = "--raster=";
        if (arg.rfind(shot, 0) == 0)
            screenshot_path = arg.substr(shot.size());
        else if (arg.rfind(raster, 0) == 0)
            raster_path = arg.substr(raster.size());
        else if (!arg.empty() && arg[0] != '-')
            zip_path = arg;
    }

    if (!fs::exists(zip_path)) {
        std::cerr << "scene zip not found: " << zip_path << "\n";
        return 1;
    }

    // 1) Extract the .pulp.zip (scene.pulp.json + assets/).
    const fs::path extract_dir =
        fs::temp_directory_path() / "pulp-elysium-standalone";
    std::error_code ec;
    fs::remove_all(extract_dir, ec);
    fs::create_directories(extract_dir, ec);
    std::string err;
    if (!extract_zip_to_dir(zip_path, extract_dir, err)) {
        std::cerr << "extract failed: " << err << "\n";
        return 1;
    }
    const auto scene_json = read_text_file(extract_dir / "scene.pulp.json");
    if (scene_json.empty()) {
        std::cerr << "scene.pulp.json missing or empty in " << zip_path << "\n";
        return 1;
    }

    // 2) Import: parse -> hoist sprite-knob art -> enrich -> materialize.
    auto ir = pulp::view::parse_figma_plugin_json(scene_json);
    absolutize_asset_paths(ir.asset_manifest, extract_dir);
    pulp::view::hoist_captured_art_knobs(ir);  // skin captured-art knobs, keep them turnable
    pulp::view::enrich_imported_image_asset_metadata(ir, ir.asset_manifest);

    std::vector<pulp::view::ImportDiagnostic> diagnostics;
    auto root = pulp::view::build_native_view_tree(
        ir, ir.asset_manifest, {.diagnostics_out = &diagnostics});
    if (!root) {
        std::cerr << "materialize failed\n";
        return 1;
    }
    root->set_requires_gpu_host(true);

    float design_w = ir.root.style.width.value_or(1000.0f);
    float design_h = ir.root.style.height.value_or(600.0f);
    // Faithful-vector import (Plan B): the materializer returns a DesignFrameView
    // that crops to its own PANEL. Size the window + design viewport to the panel
    // (not the full frame), so the design fills the window with no letterbox and
    // the view's shared paint/hit transform maps 1:1 — knobs turn where you click.
    if (auto* frame = dynamic_cast<pulp::view::DesignFrameView*>(root.get())) {
        design_w = frame->panel_width();
        design_h = frame->panel_height();
    }

    // Headless Skia-RASTER preview: renders the imported tree (sprite knobs,
    // gradients, shapes — same SkiaCanvas paint as the GPU host) to a PNG with
    // no GPU window. Useful when no GPU surface is available; the sprite knobs
    // still come up as the design's captured disc art.
    if (!raster_path.empty()) {
        // Optional --knob-value=0..1 sets every Knob (preview off-center states,
        // e.g. to check the sprite-knob indicator at non-default angles).
        if (const char* kv = std::getenv("PULP_KNOB_VALUE")) {
            const float v = std::strtof(kv, nullptr);
            std::function<void(pulp::view::View*)> set_knobs =
                [&](pulp::view::View* n) {
                    if (auto* k = dynamic_cast<pulp::view::Knob*>(n)) k->set_value(v);
                    for (std::size_t i = 0; i < n->child_count(); ++i)
                        set_knobs(n->child_at(i));
                };
            set_knobs(root.get());
        }
        // Lay out so bounds exist, then wire the shape fills to the knobs (the
        // generic post-import customization) before rendering.
        root->set_bounds({0, 0, design_w, design_h});
        root->layout_children();
        apply_shape_knob_fills(root.get(), design_h);
        if (pulp::view::render_to_file(
                *root, static_cast<uint32_t>(design_w),
                static_cast<uint32_t>(design_h), raster_path, 2.0f,
                pulp::view::ScreenshotBackend::skia)) {
            std::cout << "wrote raster preview: " << raster_path << "\n";
            return 0;
        }
        std::cerr << "raster render failed (is the Skia screenshot backend "
                     "available?)\n";
        return 1;
    }

    // 3) Open a GPU window (or capture a screenshot headlessly).

    pulp::view::WindowOptions options;
    options.title = "ELYSIUM — imported native (turn the knobs)";
    options.width = design_w;
    options.height = design_h;
    options.min_width = design_w * 0.6f;
    options.min_height = design_h * 0.6f;
    options.resizable = true;
    options.use_gpu = true;
    options.initially_hidden = !screenshot_path.empty();

    auto window = pulp::view::WindowHost::create(*root, options);
    if (!window) {
        std::cerr << "failed to create GPU window host (is Skia linked?)\n";
        return 1;
    }
    window->set_design_viewport(design_w, design_h);
    window->set_fixed_aspect_ratio(design_w / design_h);
    window->set_close_callback([] {});

    // Item 3 demo: each frame, drive the upper illustration shapes' value-driven
    // silhouette fills from their column knobs (the generic post-import wiring),
    // so turning a knob fills/empties its shape through ImageView::set_fill_value.
    int frame_count = 0;
    const bool capture = !screenshot_path.empty();
    window->set_idle_callback([&] {
        apply_shape_knob_fills(root.get(), design_h);
        if (!capture) return;
        if (++frame_count < 6) return;  // let the GPU surface settle
        auto png = window->capture_back_buffer_png();
        std::ofstream out(screenshot_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png.data()),
                  static_cast<std::streamsize>(png.size()));
        window->request_close();
    });

    window->run_event_loop();
    return 0;
}
