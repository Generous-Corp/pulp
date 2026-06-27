// Design-import standalone — imports a figma-plugin .pulp.zip from scratch and
// renders it as native Pulp UI in a GPU window. Captured-art knobs come up as
// the design's discs (sprite-skinned) with a native rotating indicator notch,
// and they TURN under the mouse.
//
// This is the production wiring of the sprite-knob path: the import pipeline
// calls hoist_captured_art_knobs() (which promotes a captured-art knob to a
// skinned-but-turnable native Knob) BEFORE enrich_imported_image_asset_metadata,
// then materializes via build_native_view_tree — the same path proven by the
// mac-platform-harness, now in a launchable app.
//
// The default fixture is a PRIVATE, dev-only test design (the pulp-planning
// submodule; not shipped). Pass any .pulp.zip to import your own design:
//   pulp-design-import-standalone                       # open the window, turn knobs
//   pulp-design-import-standalone /path/to/scene.pulp.zip
//   pulp-design-import-standalone --screenshot=out.png  # headless capture
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/window_host.hpp>

#include <functional>

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

// NOTE: the knobs are intentionally INERT with respect to the upper illustration
// shapes. An earlier demo (apply_shape_knob_fills) drove each shape's
// value-driven silhouette fill from a column knob, paired by laid-out
// x-position. That proved the native gradient-fill capability
// (ImageView::set_fill_value + the canvas url() mask) but drove it the wrong way:
// the mask was the captured PNG's own alpha (so the fill bled onto the baked
// drop shadow), the overlay color fought the gradient already baked into the
// PNG, and the positional knob<->shape pairing was off by one (the header hatch
// counted as a "shape"). Those are structural to guessing on a flattened,
// pre-colored raster — not polish bugs. The capability stays; the correct way to
// drive it is design-authored: recognize the gradient-fill layer and binding,
// capture an empty-interior outline, and clip to the vector outline. Until that
// import path exists, the knobs simply turn and the shapes render their faithful
// static captured art.

}  // namespace

int main(int argc, char** argv) {
    std::string screenshot_path;
    std::string raster_path;  // headless Skia-raster preview (no GPU window)
    // Demo-state flags: showcase the faithful-vector overlays in a headless
    // capture — focus the search field, and open a dropdown's popup.
    bool demo_focus_search = false;
    std::string demo_open_dropdown;  // open the dropdown whose value contains this
    int demo_select_tab = -1;        // select this tab index on every tab group
    fs::path zip_path = PULP_DESIGN_IMPORT_DEFAULT_ZIP;  // private dev fixture (CMake)
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        constexpr std::string_view shot = "--screenshot=";
        constexpr std::string_view raster = "--raster=";
        constexpr std::string_view open_dd = "--open-dropdown=";
        if (arg.rfind(shot, 0) == 0)
            screenshot_path = arg.substr(shot.size());
        else if (arg.rfind(raster, 0) == 0)
            raster_path = arg.substr(raster.size());
        else if (arg == "--focus-search")
            demo_focus_search = true;
        else if (arg.rfind(open_dd, 0) == 0)
            demo_open_dropdown = arg.substr(open_dd.size());
        else if (arg.rfind("--select-tab=", 0) == 0)
            demo_select_tab = std::atoi(arg.substr(std::string_view("--select-tab=").size()).c_str());
        else if (!arg.empty() && arg[0] != '-')
            zip_path = arg;
    }

    if (!fs::exists(zip_path)) {
        std::cerr << "scene zip not found: " << zip_path << "\n";
        return 1;
    }

    // 1) Extract the .pulp.zip (scene.pulp.json + assets/).
    const fs::path extract_dir =
        fs::temp_directory_path() / "pulp-design-import-standalone";
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

    // Demo-state: focus the search field / open a dropdown's popup, so a headless
    // capture (raster OR GPU) showcases the faithful-vector overlays in their
    // active states. Call AFTER layout_children() so widget bounds exist.
    auto apply_demo_state = [&] {
        auto* frame = dynamic_cast<pulp::view::DesignFrameView*>(root.get());
        if (!frame) return;
        for (int i = 0; i < frame->element_count(); ++i) {
            auto* w = frame->overlay_widget(i);
            if (!w) continue;
            if (demo_focus_search)
                if (auto* ed = dynamic_cast<pulp::view::TextEditor*>(w)) ed->set_focus(true);
            if (demo_select_tab >= 0)
                if (auto* tg = dynamic_cast<pulp::view::DesignTabGroup*>(w))
                    if (demo_select_tab < tg->tab_count()) {
                        const auto tb = tg->local_bounds();
                        pulp::view::Point p{
                            (demo_select_tab + 0.5f) * tb.width / static_cast<float>(tg->tab_count()),
                            tb.height * 0.5f};
                        tg->on_mouse_down(p);
                    }
            if (!demo_open_dropdown.empty())
                if (auto* cb = dynamic_cast<pulp::view::ComboBox*>(w))
                    if (cb->selected_text().find(demo_open_dropdown) != std::string::npos
                        && !cb->is_open()) {
                        // Open the popup the public way: a header click.
                        const auto cb_b = cb->local_bounds();
                        pulp::view::MouseEvent ev;
                        ev.position = {cb_b.width * 0.5f, cb_b.height * 0.5f};
                        ev.is_down = true;  // a header click toggles the popup open
                        cb->on_mouse_event(ev);
                    }
        }
    };

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
        // Lay out so bounds exist before rendering. Shape fills are inert for
        // the design-captured art path above.
        root->set_bounds({0, 0, design_w, design_h});
        root->layout_children();
        apply_demo_state();  // focus search / open dropdown for the capture
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
    options.title = "Design Import — native (turn the knobs)";
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

    // The knobs turn (sprite indicator) but do NOT drive the shape fills — the
    // value-driven gradient fill is a proven capability that needs a
    // design-authored binding to drive it correctly, not the raster-guess
    // heuristic this demo used to run each frame.
    int frame_count = 0;
    const bool capture = !screenshot_path.empty();
    window->set_idle_callback([&] {
        if (!capture) return;
        if (++frame_count == 2) apply_demo_state();  // after first layout/paint
        if (frame_count < 6) return;  // let the GPU surface settle
        auto png = window->capture_back_buffer_png();
        std::ofstream out(screenshot_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png.data()),
                  static_cast<std::streamsize>(png.size()));
        window->request_close();
    });

    window->run_event_loop();
    return 0;
}
