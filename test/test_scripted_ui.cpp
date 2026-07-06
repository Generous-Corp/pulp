#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#if __has_include(<pulp/render/gpu_surface.hpp>)
#include <pulp/render/gpu_surface.hpp>
#define PULP_TEST_HAS_GPU_SURFACE 1
#else
#define PULP_TEST_HAS_GPU_SURFACE 0
#endif
#include <pulp/view/scripted_ui.hpp>
#include <pulp/format/reload/scripted_ui_swap_unit.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/widgets.hpp>
#include <algorithm>
#include <chrono>
#include <vector>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

namespace {

fs::path make_temp_dir(const char* stem) {
    auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto dir = fs::temp_directory_path() / (std::string(stem) + "-" + unique);
    fs::create_directories(dir);
    return dir;
}

void write_text(const fs::path& path, const std::string& content) {
    std::ofstream file(path);
    file << content;
    file.close();
}

bool wait_for_reload(const std::function<bool()>& poller) {
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(125));
        if (poller()) {
            return true;
        }
    }
    return poller();
}

#if PULP_TEST_HAS_GPU_SURFACE
class TestGpuSurface final : public pulp::render::GpuSurface {
public:
    explicit TestGpuSurface(AdapterInfo info) : info_(std::move(info)) {}

    bool initialize(const Config& config) override {
        initialized_ = true;
        width_ = config.width;
        height_ = config.height;
        has_surface_ = config.native_surface_handle != nullptr;
        return true;
    }
    void resize(uint32_t width, uint32_t height) override {
        width_ = width;
        height_ = height;
    }
    bool begin_frame() override { return false; }
    void end_frame() override {}
    bool is_initialized() const override { return initialized_; }
    bool has_surface() const override { return has_surface_; }
    uint32_t width() const override { return width_; }
    uint32_t height() const override { return height_; }
    void* dawn_device_handle() const override { return nullptr; }
    void* dawn_queue_handle() const override { return nullptr; }
    void* dawn_instance_handle() const override { return nullptr; }
    void* current_texture_handle() const override { return nullptr; }
    AdapterInfo adapter_info() const override { return info_; }

private:
    AdapterInfo info_;
    bool initialized_ = false;
    bool has_surface_ = false;
    uint32_t width_ = 1;
    uint32_t height_ = 1;
};

static pulp::render::GpuSurface::AdapterInfo test_gpu_info() {
    pulp::render::GpuSurface::AdapterInfo info;
    info.available = true;
    info.native_bridge = false;
    info.backend = "Dawn/WebGPU";
    info.backend_type = "Mock";
    info.name = "Pulp Test Mock Adapter";
    info.vendor = "Pulp";
    info.architecture = "unavailable";
    info.description = info.name;
    info.preferred_canvas_format = "bgra8unorm";
    return info;
}
#endif

} // namespace

TEST_CASE("ScriptedUiSession preserves widget state across script reload", "[view][scripted-ui][hotreload]") {
    const auto temp_dir = make_temp_dir("pulp-scripted-ui");
    const auto script_path = temp_dir / "main.js";

    write_text(script_path, R"(
        createKnob('gain', 10, 10, 48, 48);
        createCheckbox('armed', '');
        createToggleButton('bypass', '');
        createXYPad('position', '');
        createLabel('status', 'v1', '');
    )");

    View root;
    root.set_bounds({0, 0, 320, 240});
    root.set_theme(Theme::dark());

    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script_path,
        .enable_hot_reload = true,
        .enable_theme_reload = true,
    });

    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(error.empty());

    auto* initial_knob = dynamic_cast<Knob*>(session.bridge()->widget("gain"));
    REQUIRE(initial_knob != nullptr);
    initial_knob->set_value(0.72f);
    auto* initial_checkbox = dynamic_cast<Checkbox*>(session.bridge()->widget("armed"));
    REQUIRE(initial_checkbox != nullptr);
    initial_checkbox->set_checked(true);
    auto* initial_toggle_button = dynamic_cast<ToggleButton*>(session.bridge()->widget("bypass"));
    REQUIRE(initial_toggle_button != nullptr);
    initial_toggle_button->set_on(true);
    auto* initial_xy = dynamic_cast<XYPad*>(session.bridge()->widget("position"));
    REQUIRE(initial_xy != nullptr);
    initial_xy->set_x(0.81f);
    initial_xy->set_y(0.23f);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_text(script_path, R"(
        createKnob('gain', 10, 10, 48, 48);
        createCheckbox('armed', '');
        createToggleButton('bypass', '');
        createXYPad('position', '');
        createLabel('status', 'v2', '');
        createLabel('after-reload', 'ready', '');
    )");

    REQUIRE(wait_for_reload([&] {
        std::string poll_error;
        return session.poll(&poll_error);
    }));

    auto* knob_after_reload = dynamic_cast<Knob*>(session.bridge()->widget("gain"));
    REQUIRE(knob_after_reload != nullptr);
    REQUIRE_THAT(knob_after_reload->value(), WithinAbs(0.72f, 0.001f));
    auto* checkbox_after_reload = dynamic_cast<Checkbox*>(session.bridge()->widget("armed"));
    REQUIRE(checkbox_after_reload != nullptr);
    REQUIRE(checkbox_after_reload->is_checked());
    auto* toggle_button_after_reload = dynamic_cast<ToggleButton*>(session.bridge()->widget("bypass"));
    REQUIRE(toggle_button_after_reload != nullptr);
    REQUIRE(toggle_button_after_reload->is_on());
    auto* xy_after_reload = dynamic_cast<XYPad*>(session.bridge()->widget("position"));
    REQUIRE(xy_after_reload != nullptr);
    REQUIRE_THAT(xy_after_reload->x_value(), WithinAbs(0.81f, 0.001f));
    REQUIRE_THAT(xy_after_reload->y_value(), WithinAbs(0.23f, 0.001f));

    auto* after_reload = dynamic_cast<Label*>(session.bridge()->widget("after-reload"));
    REQUIRE(after_reload != nullptr);
    REQUIRE(after_reload->text() == "ready");

    fs::remove_all(temp_dir);
}

TEST_CASE("ScriptedUiSession reapplies sibling theme.json overrides", "[view][scripted-ui][theme]") {
    const auto temp_dir = make_temp_dir("pulp-scripted-theme");
    const auto script_path = temp_dir / "main.js";
    const auto theme_path = temp_dir / "theme.json";

    write_text(script_path, "createLabel('status', 'theme', '');");
    write_text(theme_path, R"({
        "colors": {
            "bg.primary": "#ff0000"
        }
    })");

    View root;
    root.set_bounds({0, 0, 320, 240});
    root.set_theme(Theme::dark());

    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script_path,
        .enable_hot_reload = true,
        .enable_theme_reload = true,
    });

    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(error.empty());

    auto initial_bg = root.theme().color("bg.primary");
    REQUIRE(initial_bg.has_value());
    REQUIRE(initial_bg->r8() == 0xFF);
    REQUIRE(initial_bg->g8() == 0x00);
    REQUIRE(initial_bg->b8() == 0x00);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_text(theme_path, R"({
        "colors": {
            "bg.primary": "#00ff00"
        }
    })");

    REQUIRE(wait_for_reload([&] {
        std::string poll_error;
        return session.poll(&poll_error);
    }));

    auto updated_bg = root.theme().color("bg.primary");
    REQUIRE(updated_bg.has_value());
    REQUIRE(updated_bg->r8() == 0x00);
    REQUIRE(updated_bg->g8() == 0xFF);
    REQUIRE(updated_bg->b8() == 0x00);

    fs::remove_all(temp_dir);
}

TEST_CASE("ScriptedUiSession applies an explicit kit token theme path", "[view][scripted-ui][theme]") {
    const auto temp_dir = make_temp_dir("pulp-scripted-kit-theme");
    const auto script_path = temp_dir / "ui" / "main.js";
    const auto theme_path = temp_dir / "pulp-kits" / "kit" / "ui" / "tokens.json";

    fs::create_directories(script_path.parent_path());
    fs::create_directories(theme_path.parent_path());
    write_text(script_path, "createLabel('status', 'kit-theme', '');");
    write_text(theme_path, R"({
        "colors": {
            "color.control.accent": "#4b8aef"
        }
    })");

    View root;
    root.set_bounds({0, 0, 320, 240});
    root.set_theme(Theme::dark());

    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script_path,
        .theme_path = theme_path,
        .enable_hot_reload = false,
        .enable_theme_reload = true,
    });

    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(error.empty());

    auto accent = root.theme().color("color.control.accent");
    REQUIRE(accent.has_value());
    REQUIRE(accent->r8() == 0x4b);
    REQUIRE(accent->g8() == 0x8a);
    REQUIRE(accent->b8() == 0xef);

    fs::remove_all(temp_dir);
}

TEST_CASE("ScriptedUiSession resolves reviewed kit asset roots", "[view][scripted-ui][assets]") {
    const auto temp_dir = make_temp_dir("pulp-scripted-kit-assets");
    const auto script_path = temp_dir / "ui" / "main.js";
    const auto asset_root = temp_dir / "pulp-kits" / "kit" / "assets";

    fs::create_directories(script_path.parent_path());
    fs::create_directories(asset_root / "meta");
    write_text(asset_root / "meta" / "info.json", R"({"name":"kit asset"})");
    write_text(temp_dir / "outside.json", R"({"name":"outside"})");
    write_text(script_path, R"JS(
        var asset = __loadAssetSync__('meta/info.json');
        var traversal = __loadAssetSync__('../outside.json');
        createLabel('asset', asset.ok ? asset.text : 'missing', '');
        createLabel('traversal', traversal.ok ? 'leaked' : String(traversal.status), '');
    )JS");

    View root;
    root.set_bounds({0, 0, 320, 240});
    root.set_theme(Theme::dark());

    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script_path,
        .asset_roots = {asset_root},
        .enable_hot_reload = false,
        .enable_theme_reload = false,
    });

    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(error.empty());

    auto* asset = dynamic_cast<Label*>(session.bridge()->widget("asset"));
    REQUIRE(asset != nullptr);
    REQUIRE(asset->text() == R"({"name":"kit asset"})");
    auto* traversal = dynamic_cast<Label*>(session.bridge()->widget("traversal"));
    REQUIRE(traversal != nullptr);
    REQUIRE(traversal->text() == "404");

    fs::remove_all(temp_dir);
}

TEST_CASE("ScriptedUiSession blocks reviewed kit asset root escape hatches", "[view][scripted-ui][assets]") {
    const auto temp_dir = make_temp_dir("pulp-scripted-kit-asset-escape");
    const auto script_path = temp_dir / "ui" / "main.js";
    const auto asset_root = temp_dir / "pulp-kits" / "kit" / "assets";
    const auto outside_path = temp_dir / "outside.json";

    fs::create_directories(script_path.parent_path());
    fs::create_directories(asset_root / "meta");
    write_text(asset_root / "meta" / "info.json", R"({"name":"kit asset"})");
    write_text(outside_path, R"({"name":"outside"})");

    std::error_code ec;
    fs::create_symlink(outside_path, asset_root / "meta" / "outside-link.json", ec);

    const auto file_url = std::string("file://") + outside_path.string();
    write_text(script_path,
               "var fileAsset = __loadAssetSync__('" + file_url + "');\n"
               "var absoluteAsset = __loadAssetSync__('" + outside_path.string() + "');\n"
               "var symlinkAsset = __loadAssetSync__('meta/outside-link.json');\n"
               "createLabel('file', fileAsset.ok ? 'leaked' : String(fileAsset.status), '');\n"
               "createLabel('absolute', absoluteAsset.ok ? 'leaked' : String(absoluteAsset.status), '');\n"
               "createLabel('symlink', symlinkAsset.ok ? 'leaked' : String(symlinkAsset.status), '');\n");

    View root;
    root.set_bounds({0, 0, 320, 240});
    root.set_theme(Theme::dark());

    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script_path,
        .asset_roots = {asset_root},
        .enable_hot_reload = false,
        .enable_theme_reload = false,
    });

    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(error.empty());

    auto* file = dynamic_cast<Label*>(session.bridge()->widget("file"));
    REQUIRE(file != nullptr);
    REQUIRE(file->text() == "403");

    auto* absolute = dynamic_cast<Label*>(session.bridge()->widget("absolute"));
    REQUIRE(absolute != nullptr);
    REQUIRE(absolute->text() == "404");

    auto* symlink = dynamic_cast<Label*>(session.bridge()->widget("symlink"));
    REQUIRE(symlink != nullptr);
    REQUIRE(symlink->text() == "404");

    fs::remove_all(temp_dir);
}

TEST_CASE("ScriptedUiSession drops stale theme overrides after script reload", "[view][scripted-ui][theme][hotreload]") {
    const auto temp_dir = make_temp_dir("pulp-scripted-theme-reset");
    const auto script_path = temp_dir / "main.js";
    const auto theme_path = temp_dir / "theme.json";

    write_text(script_path, "createLabel('status', 'theme', '');");
    write_text(theme_path, R"({
        "colors": {
            "bg.primary": "#ff0000"
        }
    })");

    View root;
    root.set_bounds({0, 0, 320, 240});
    root.set_theme(Theme::dark());
    const auto default_bg = root.theme().color("bg.primary");
    REQUIRE(default_bg.has_value());

    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script_path,
        .enable_hot_reload = true,
        .enable_theme_reload = true,
    });

    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(error.empty());

    auto overridden_bg = root.theme().color("bg.primary");
    REQUIRE(overridden_bg.has_value());
    REQUIRE(overridden_bg->r8() == 0xFF);
    REQUIRE(overridden_bg->g8() == 0x00);
    REQUIRE(overridden_bg->b8() == 0x00);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_text(script_path, "createLabel('status', 'reloaded', '');");
    REQUIRE(wait_for_reload([&] {
        std::string poll_error;
        return session.poll(&poll_error);
    }));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    fs::remove(theme_path);
    REQUIRE(wait_for_reload([&] {
        std::string poll_error;
        return session.poll(&poll_error);
    }));

    auto reverted_bg = root.theme().color("bg.primary");
    REQUIRE(reverted_bg.has_value());
    REQUIRE(reverted_bg->r == default_bg->r);
    REQUIRE(reverted_bg->g == default_bg->g);
    REQUIRE(reverted_bg->b == default_bg->b);

    fs::remove_all(temp_dir);
}

TEST_CASE("ScriptedUiSession keeps repaint callback across reload", "[view][scripted-ui][hotreload]") {
    const auto temp_dir = make_temp_dir("pulp-scripted-repaint");
    const auto script_path = temp_dir / "main.js";

    write_text(script_path, "createLabel('status', 'v1', '');");

    View root;
    root.set_bounds({0, 0, 320, 240});
    root.set_theme(Theme::dark());

    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script_path,
        .enable_hot_reload = true,
        .enable_theme_reload = true,
    });

    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(error.empty());

    int repaint_count = 0;
    session.set_repaint_callback([&] { ++repaint_count; });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_text(script_path, R"(
        createLabel('status', 'v2', '');
        execAsync('printf repaint', 'status-callback');
    )");

    REQUIRE(wait_for_reload([&] {
        std::string poll_error;
        session.poll(&poll_error);
        return repaint_count > 0;
    }));
    REQUIRE(repaint_count > 0);

    fs::remove_all(temp_dir);
}

// ── Phase iOS-D.3b Slice 1: ScriptedUiSession::attach_gpu_surface ──────────
// planning/2026-05-29-ios-d3b-threejs-webgpu-program.md § Slice 1
//
// Pins the late-attach plumbing that lets format adapters hand a host's
// live GpuSurface to the JS-side widget bridge AFTER the bridge has been
// constructed. Without this, navigator.gpu / canvas.getContext('webgpu')
// stay on mocks and 3D scenes (Three.js, raw WebGPU) render black.

TEST_CASE("ScriptedUiSession::attach_gpu_surface forwards to bridge",
          "[scripted_ui][gpu-surface-plumbing][issue-ios-d3b-slice1]") {
    auto tmp_dir = make_temp_dir("scripted-ui-gpu-surface");
    auto script_path = tmp_dir / "ui.js";
    {
        std::ofstream out(script_path);
        // Minimal valid script that exercises bridge construction. createPanel
        // returns an id; assigning it is enough to load without touching any
        // host-only paths (no canvas, no GPU draws).
        out << "var p = createPanel({});";
    }

    View root;
    StateStore store;
    ScriptedUiSession session(root, store,
                              ScriptedUiOptions{.script_path = script_path,
                                                .enable_hot_reload = false,
                                                .enable_theme_reload = false});

    std::string err;
    REQUIRE(session.load(&err));
    REQUIRE(session.bridge() != nullptr);

    // Before attach: bridge has no surface (matches the ctor default).
    REQUIRE(session.bridge()->gpu_surface() == nullptr);
    REQUIRE(session.gpu_surface() == nullptr);

#if PULP_TEST_HAS_GPU_SURFACE
    // Attach a test (non-null) surface. The contract being pinned here is "the
    // session forwards the pointer to its bridge AND remembers it for the next
    // hot-reload-triggered rebuild". A live Dawn surface is not required for
    // this contract — the live counterpart
    // belongs in slice 3's [jsc][navigator-gpu][live] case.
    TestGpuSurface mock_surface(test_gpu_info());
    session.attach_gpu_surface(&mock_surface);
    REQUIRE(session.gpu_surface() == &mock_surface);
    REQUIRE(session.bridge()->gpu_surface() == &mock_surface);

    // Idempotent re-attach.
    session.attach_gpu_surface(&mock_surface);
    REQUIRE(session.bridge()->gpu_surface() == &mock_surface);
#else
    SUCCEED("render::GpuSurface test double unavailable in GPU-off builds");
#endif

    // Detach.
    session.attach_gpu_surface(nullptr);
    REQUIRE(session.gpu_surface() == nullptr);
    REQUIRE(session.bridge()->gpu_surface() == nullptr);

    fs::remove_all(tmp_dir);
}

TEST_CASE("ScriptedUiSession explicit reload() rebuilds without a watcher, preserving state + last-good",
          "[view][scripted-ui][reload]") {
    const auto temp_dir = make_temp_dir("pulp-scripted-ui");
    const auto script_path = temp_dir / "main.js";
    write_text(script_path, "createKnob('gain', 10, 10, 48, 48);\ncreateLabel('status', 'v1', '');\n");

    View root;
    root.set_bounds({0, 0, 320, 240});
    root.set_theme(Theme::dark());
    StateStore store;
    // No hot-reload watcher: reload() is the on-demand path.
    ScriptedUiSession session(root, store, {.script_path = script_path,
                                            .enable_hot_reload = false,
                                            .enable_theme_reload = false});
    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(error.empty());

    auto* knob = dynamic_cast<Knob*>(session.bridge()->widget("gain"));
    REQUIRE(knob != nullptr);
    knob->set_value(0.66f);

    // Edit + explicit reload (no poll/watcher). State preserved, new widget present.
    write_text(script_path,
               "createKnob('gain', 10, 10, 48, 48);\n"
               "createLabel('status', 'v2', '');\n"
               "createLabel('added', 'yes', '');\n");
    REQUIRE(session.reload(&error));
    REQUIRE(error.empty());

    auto* knob2 = dynamic_cast<Knob*>(session.bridge()->widget("gain"));
    REQUIRE(knob2 != nullptr);
    REQUIRE_THAT(knob2->value(), WithinAbs(0.66f, 0.001f));   // state preserved
    auto* added = dynamic_cast<Label*>(session.bridge()->widget("added"));
    REQUIRE(added != nullptr);                                 // new code applied
    REQUIRE(added->text() == "yes");

    // Last-good: a broken reload keeps the current UI and reports the error.
    write_text(script_path, "this is not valid javascript {{{");
    std::string bad_error;
    REQUIRE_FALSE(session.reload(&bad_error));
    REQUIRE_FALSE(bad_error.empty());
    REQUIRE(session.bridge()->widget("added") != nullptr);     // old UI intact

    // reload_from(): swap to a different bundle's script.
    const auto other = temp_dir / "other.js";
    write_text(other, "createLabel('only-here', 'B', '');\n");
    REQUIRE(session.reload_from(other, &error));
    REQUIRE(session.script_path() == other);
    REQUIRE(session.bridge()->widget("only-here") != nullptr);

    fs::remove_all(temp_dir);
}

TEST_CASE("ScriptedUiSession records JS-axis reload metrics (item 1.2)",
          "[view][scripted-ui][metrics]") {
    const auto temp_dir = make_temp_dir("pulp-scripted-metrics");
    const auto script_path = temp_dir / "main.js";
    write_text(script_path, "createKnob('gain', 10, 10, 48, 48);\n");

    View root;
    root.set_bounds({0, 0, 320, 240});
    root.set_theme(Theme::dark());
    StateStore store;
    ScriptedUiSession session(root, store, {.script_path = script_path,
                                            .enable_hot_reload = false,
                                            .enable_theme_reload = false});
    std::string error;
    REQUIRE(session.load(&error));

    // A reload (preserve_state) exercises probe → snapshot → rebuild → restore.
    write_text(script_path,
               "createKnob('gain', 10, 10, 48, 48);\ncreateLabel('added', 'v2', '');\n");
    REQUIRE(session.reload(&error));

    const auto& m = session.last_reload_metrics();
    CHECK(m.probe_ms >= 0.0);
    CHECK(m.snapshot_ms >= 0.0);
    CHECK(m.rebuild_ms >= 0.0);
    CHECK(m.restore_ms >= 0.0);
    CHECK(m.total_ms >= m.probe_ms);
    CHECK(m.total_ms >= m.rebuild_ms);
    CHECK(session.last_reload_ms() == m.total_ms);
    const double sum = m.probe_ms + m.snapshot_ms + m.rebuild_ms + m.restore_ms;
    CHECK(m.total_ms + 1.0 >= sum);   // total ≈ sum of contiguous phases

    fs::remove_all(temp_dir);
}

TEST_CASE("ScriptedUiSession reload rolls back cleanly on a bad theme (no post-commit failure)",
          "[view][scripted-ui][reload][rollback]") {
    const auto temp_dir = make_temp_dir("pulp-scripted-rollback");
    const auto script_path = temp_dir / "main.js";
    const auto theme_path = temp_dir / "theme.json";
    write_text(script_path, "createLabel('before', 'v1', '');\n");
    write_text(theme_path, R"({ "colors": { "bg.primary": "#112233" } })");

    View root;
    root.set_bounds({0, 0, 320, 240});
    root.set_theme(Theme::dark());
    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script_path,
        .enable_hot_reload = false,
        .enable_theme_reload = true,
    });
    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(session.bridge()->widget("before") != nullptr);

    // New code (would add 'after') + a MALFORMED theme file. The theme is
    // pre-resolved before the commit, so the reload must FAIL with the OLD UI
    // fully intact — NOT commit the new bridge and then error (item 1.5).
    write_text(script_path, "createLabel('after', 'v2', '');\n");
    write_text(theme_path, "{ this is not valid json ]]]");

    std::string reload_error;
    REQUIRE_FALSE(session.reload(&reload_error));      // reload failed…
    REQUIRE_FALSE(reload_error.empty());
    REQUIRE(session.bridge()->widget("before") != nullptr);  // …old UI still live
    REQUIRE(session.bridge()->widget("after") == nullptr);   // …new NOT committed

    // A subsequent good reload (fix the theme) still works — no wedged state.
    write_text(theme_path, R"({ "colors": { "bg.primary": "#445566" } })");
    REQUIRE(session.reload(&error));
    REQUIRE(session.bridge()->widget("after") != nullptr);

    fs::remove_all(temp_dir);
}

TEST_CASE("ScriptedUiSession preserves ComboBox selection across reload (item 1.4)",
          "[view][scripted-ui][hotreload]") {
    const auto temp_dir = make_temp_dir("pulp-scripted-combo");
    const auto script_path = temp_dir / "main.js";
    // Combo with 3 items; the script does NOT set a selection, so a fresh build
    // defaults to index 0. Preservation must come from the reload snapshot.
    const char* script_v1 =
        "createCombo('mode', '');\nsetItems('mode', ['a','b','c']);\n";
    write_text(script_path, script_v1);

    View root;
    root.set_bounds({0, 0, 320, 240});
    root.set_theme(Theme::dark());
    StateStore store;
    ScriptedUiSession session(root, store, {.script_path = script_path,
                                            .enable_hot_reload = false,
                                            .enable_theme_reload = false});
    std::string error;
    REQUIRE(session.load(&error));

    auto* combo = dynamic_cast<ComboBox*>(session.bridge()->widget("mode"));
    REQUIRE(combo != nullptr);
    REQUIRE(combo->selected() == 0);          // default
    combo->set_selected(2);                    // user picks the third item

    // Reload (same combo + items, plus a new widget to force a real rebuild).
    write_text(script_path, std::string(script_v1) + "createLabel('added', 'v2', '');\n");
    REQUIRE(session.reload(&error));

    auto* combo2 = dynamic_cast<ComboBox*>(session.bridge()->widget("mode"));
    REQUIRE(combo2 != nullptr);
    REQUIRE(combo2->selected() == 2);          // selection preserved across reload
    REQUIRE(session.bridge()->widget("added") != nullptr);  // reload really happened

    fs::remove_all(temp_dir);
}

// item 1.2 baseline: publish scripted-UI (JS-axis) reload p50/p95 for a realistic
// panel so regressions in the dev-loop latency are visible. Hidden [.benchmark].
TEST_CASE("ScriptedUiSession reload latency baseline", "[view][scripted-ui][.benchmark]") {
    const auto temp_dir = make_temp_dir("pulp-scripted-baseline");
    const auto script_path = temp_dir / "panel.js";
    std::string base;
    for (int i = 0; i < 24; ++i)
        base += "createKnob('k" + std::to_string(i) + "', " +
                std::to_string(10 + (i % 6) * 52) + ", " +
                std::to_string(10 + (i / 6) * 60) + ", 48, 48);\n";
    write_text(script_path, base);

    View root;
    root.set_bounds({0, 0, 640, 480});
    root.set_theme(Theme::dark());
    StateStore store;
    ScriptedUiSession session(root, store, {.script_path = script_path,
                                            .enable_hot_reload = false,
                                            .enable_theme_reload = false});
    std::string error;
    REQUIRE(session.load(&error));

    constexpr int kIters = 200;
    std::vector<double> totals;
    totals.reserve(kIters);
    for (int i = 0; i < kIters; ++i) {
        write_text(script_path, base + "createLabel('rev', 'v" + std::to_string(i) + "', '');\n");
        REQUIRE(session.reload(&error));
        totals.push_back(session.last_reload_ms());
    }
    std::sort(totals.begin(), totals.end());
    const double p50 = totals[totals.size() / 2];
    const double p95 = totals[static_cast<size_t>(totals.size() * 0.95)];
    WARN("item 1.2 baseline — scripted-UI reload (24-knob panel): p50=" << p50
         << "ms p95=" << p95 << "ms worst=" << totals.back() << "ms (60fps frame=16.7ms)");
    CHECK(p50 >= 0.0);
    fs::remove_all(temp_dir);
}

// ── UX SwapUnit adapter (live-swap item 1.8b/2.5b) ────────────────────────────
TEST_CASE("ScriptedUiSwapUnit applies + rolls back a UI swap via apply_live_swap",
          "[view][scripted-ui][swap-unit][1.8]") {
    const auto dir = make_temp_dir("pulp-ux-swapunit");
    const auto ui_a = dir / "a.js";
    const auto ui_b = dir / "b.js";
    write_text(ui_a, "createLabel('v', 'A', '');");
    write_text(ui_b, "createLabel('v', 'B', '');");

    View root;
    root.set_bounds({0, 0, 320, 240});
    root.set_theme(Theme::dark());
    StateStore store;
    ScriptedUiSession session(root, store, {.script_path = ui_a});
    std::string err;
    REQUIRE(session.load(&err));
    REQUIRE(session.script_path() == ui_a);

    using pulp::format::reload::apply_live_swap;
    using pulp::format::reload::ScriptedUiSwapUnit;
    using pulp::format::reload::SwapStage;
    using pulp::format::reload::SwapUnit;

    SECTION("UX-only transaction commits the swap") {
        ScriptedUiSwapUnit ux(session, ui_b);
        std::vector<SwapUnit*> units{&ux};
        REQUIRE(apply_live_swap(units).ok);
        REQUIRE(session.script_path() == ui_b);
    }

    SECTION("a later stage failing rolls the UX back to the previous script") {
        ScriptedUiSwapUnit ux(session, ui_b);
        // UX first, then a stand-in DSP stage that rejects → UX must roll back.
        std::vector<SwapStage> stages{ux.to_stage(),
                                      SwapStage{"dsp", [] { return false; }, [] {}}};
        auto r = apply_live_swap(stages);
        REQUIRE_FALSE(r.ok);
        REQUIRE(r.failed_stage == "dsp");
        REQUIRE(session.script_path() == ui_a);   // rolled back to the pre-swap UI
    }
}
