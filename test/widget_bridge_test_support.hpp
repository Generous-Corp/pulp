// Shared private support for the split WidgetBridge test translation units.
#pragma once

#include <cstdlib>
#include <string_view>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/asset_manager.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/css_gradient.hpp>
#include <pulp/view/drag_drop.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/gesture.hpp>
#include <pulp/view/native_view_host.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/virtual_list.hpp>
#include <pulp/view/virtual_grid.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#if __has_include(<pulp/render/gpu_surface.hpp>)
#include <pulp/render/gpu_surface.hpp>
#define PULP_TEST_HAS_GPU_SURFACE 1
#else
#define PULP_TEST_HAS_GPU_SURFACE 0
#endif
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>
#include <thread>
#include <utility>
#include <cstdio>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

#if defined(_WIN32)
static int pulp_test_dup_fd(int fd) { return _dup(fd); }
static int pulp_test_dup2_fd(int old_fd, int new_fd) { return _dup2(old_fd, new_fd); }
static int pulp_test_close_fd(int fd) { return _close(fd); }
static int pulp_test_fileno(FILE* file) { return _fileno(file); }
#else
static int pulp_test_dup_fd(int fd) { return dup(fd); }
static int pulp_test_dup2_fd(int old_fd, int new_fd) { return dup2(old_fd, new_fd); }
static int pulp_test_close_fd(int fd) { return close(fd); }
static int pulp_test_fileno(FILE* file) { return fileno(file); }
#endif

struct PulpTestFdGuard {
    explicit PulpTestFdGuard(int fd_in) : fd(fd_in) {}
    ~PulpTestFdGuard() {
        if (fd >= 0) pulp_test_close_fd(fd);
    }

    PulpTestFdGuard(const PulpTestFdGuard&) = delete;
    PulpTestFdGuard& operator=(const PulpTestFdGuard&) = delete;

    int fd = -1;
};

struct PulpTestFileGuard {
    explicit PulpTestFileGuard(FILE* file_in) : file(file_in) {}
    ~PulpTestFileGuard() {
        if (file != nullptr) std::fclose(file);
    }

    PulpTestFileGuard(const PulpTestFileGuard&) = delete;
    PulpTestFileGuard& operator=(const PulpTestFileGuard&) = delete;

    FILE* file = nullptr;
};

static std::string js_single_quoted(std::string value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c; break;
        }
    }
    return out;
}

static std::string trim_crlf(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
    return value;
}

static bool wait_for_async_result(WidgetBridge& bridge, const std::function<bool()>& done) {
#if defined(_WIN32)
    constexpr int attempts = 300;
#else
    constexpr int attempts = 50;
#endif
    for (int i = 0; i < attempts; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        bridge.poll_async_results();
        if (done()) return true;
    }
    return done();
}

static MouseEvent bridge_gesture_pointer_event(Point p,
                                               MousePhase phase,
                                               double* timestamp,
                                               double advance = 0.05,
                                               int pointer_id = 0) {
    *timestamp += advance;
    MouseEvent event;
    event.position = p;
    event.window_position = p;
    event.button = MouseButton::left;
    event.is_down = phase != MousePhase::release;
    event.phase = phase;
    event.pointer_id = pointer_id;
    return event;
}

static MouseEvent bridge_gesture_touch_event(Point p,
                                             MousePhase phase,
                                             int pointer_id,
                                             double* timestamp,
                                             double advance = 0.05) {
    auto event = bridge_gesture_pointer_event(p, phase, timestamp, advance, pointer_id);
    event.pointer_type = PointerType::touch;
    return event;
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

static pulp::render::GpuSurface::AdapterInfo test_gpu_info(bool native_bridge) {
    pulp::render::GpuSurface::AdapterInfo info;
    info.available = true;
    info.native_bridge = native_bridge;
    info.backend = native_bridge ? "Dawn/Metal" : "Dawn/WebGPU";
    info.backend_type = native_bridge ? "Metal" : "Mock";
    info.name = native_bridge ? "Pulp Test Native Adapter" : "Pulp Test Mock Adapter";
    info.vendor = "Pulp";
    info.architecture = native_bridge ? "arm64" : "unavailable";
    info.description = info.name;
    info.preferred_canvas_format = native_bridge ? "rgba8unorm" : "bgra8unorm";
    return info;
}
#endif
