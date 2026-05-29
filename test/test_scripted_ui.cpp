#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#if __has_include(<pulp/render/gpu_surface.hpp>)
#include <pulp/render/gpu_surface.hpp>
#define PULP_TEST_HAS_GPU_SURFACE 1
#else
#define PULP_TEST_HAS_GPU_SURFACE 0
#endif
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/widgets.hpp>
#include <chrono>
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
