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
#include <pulp/view/design_import.hpp>
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

}  // namespace

int main(int argc, char** argv) {
    std::string screenshot_path;
    fs::path zip_path = PULP_ELYSIUM_DEFAULT_ZIP;  // committed fixture (CMake)
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        constexpr std::string_view prefix = "--screenshot=";
        if (arg.rfind(prefix, 0) == 0)
            screenshot_path = arg.substr(prefix.size());
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

    // 3) Open a GPU window (or capture a screenshot headlessly).
    const float design_w = ir.root.style.width.value_or(1000.0f);
    const float design_h = ir.root.style.height.value_or(600.0f);

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

    // Item 3 demo wiring: pair each upper illustration shape with its column
    // knob, then drive the shape's value-driven silhouette fill from the knob.
    // Turning a knob fills/empties its shape (the prism / cylinder / pentagon /
    // cube) through the new ImageView::set_fill_value + canvas url() alpha mask.
    // The pairing runs lazily once layout has produced real bounds.
    struct FillPair { pulp::view::ImageView* shape; pulp::view::Knob* knob; };
    auto fill_pairs = std::make_shared<std::vector<FillPair>>();
    auto wired = std::make_shared<bool>(false);
    auto wire_fills = [root = root.get(), design_h, fill_pairs]() -> bool {
        std::vector<std::pair<float, pulp::view::Knob*>> knobs;
        std::vector<std::pair<float, pulp::view::ImageView*>> shapes;
        std::function<void(pulp::view::View*, float, float)> walk =
            [&](pulp::view::View* v, float ox, float oy) {
                const auto bn = v->bounds();
                const float gx = ox + bn.x, gy = oy + bn.y;
                if (auto* k = dynamic_cast<pulp::view::Knob*>(v)) {
                    if (bn.width > 45.0f)  // the big column knobs, not the VALUE minis
                        knobs.emplace_back(gx + bn.width * 0.5f, k);
                } else if (auto* img = dynamic_cast<pulp::view::ImageView*>(v)) {
                    if (bn.width > 50.0f && bn.height > 50.0f &&
                        gy < design_h * 0.45f)  // the upper illustration band
                        shapes.emplace_back(gx + bn.width * 0.5f, img);
                }
                for (std::size_t i = 0; i < v->child_count(); ++i)
                    walk(v->child_at(i), gx, gy);
            };
        walk(root, 0.0f, 0.0f);
        if (knobs.empty() || shapes.empty()) return false;  // layout not ready yet
        std::sort(knobs.begin(), knobs.end(),
                  [](auto& a, auto& b) { return a.first < b.first; });
        std::sort(shapes.begin(), shapes.end(),
                  [](auto& a, auto& b) { return a.first < b.first; });
        const std::size_t n = std::min(knobs.size(), shapes.size());
        for (std::size_t i = 0; i < n; ++i)
            fill_pairs->push_back({shapes[i].second, knobs[i].second});
        return !fill_pairs->empty();
    };

    int frame_count = 0;
    const bool capture = !screenshot_path.empty();
    window->set_idle_callback([&, fill_pairs, wired] {
        if (!*wired) *wired = wire_fills();
        // In headless capture mode, stagger the knob values once wired so the
        // single shot shows clearly different fill levels; the live window
        // instead reflects whatever the user turns the knobs to.
        if (capture && *wired)
            for (std::size_t i = 0; i < fill_pairs->size(); ++i)
                (*fill_pairs)[i].knob->set_value(
                    0.2f + 0.25f * static_cast<float>(i));
        for (const auto& p : *fill_pairs)
            p.shape->set_fill_value(p.knob->value());
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
