#pragma once

// Windows: include system headers before any namespace block to prevent
// CHOC/QuickJS intrinsic declarations from polluting our namespace.
// Without this, _InterlockedIncrement etc. get declared in pulp::view::
// and conflict with winbase.h's expectations.
#if defined(_WIN32)
#include <windows.h>
#endif

#include <pulp/view/script_engine.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/state/store.hpp>
#include <chrono>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>

namespace pulp::render {
class GpuSurface;
}

#ifdef PULP_BENCHMARK
namespace pulp::render::bench {
struct PerfCounters;
}
#endif

namespace pulp::view {

// Widget value snapshot for hot reload preservation
struct WidgetReloadSnapshot {
    std::unordered_map<std::string, float> scalar_values;
    struct XYValue { float x = 0; float y = 0; };
    std::unordered_map<std::string, XYValue> xy_values;
    // Legacy compat
    std::unordered_map<std::string, float> values;
};

// Bridges JS scripts to the Pulp widget system.
// Registers native functions that JS code calls to create, configure,
// layout, style, and interact with widgets.
class WidgetBridge {
public:
    WidgetBridge(ScriptEngine& engine, View& root, state::StateStore& store,
                 render::GpuSurface* gpu_surface = nullptr);
    ~WidgetBridge();

    // Load and execute a UI script
    void load_script(const std::string& code);

    // Get a widget by its JS-assigned ID
    View* widget(const std::string& id);

    // Sync all widget values from the parameter store
    void sync_from_store();

    // Hot reload support: clear all JS-created widgets
    void clear();

    // Forward a key event to JS (called by host for global shortcuts)
    void forward_key_event(int key_code, uint16_t modifiers, bool is_down);

    // Snapshot widget values for preservation across hot reload
    void snapshot_values(WidgetReloadSnapshot& out) const;
    void snapshot_values(std::unordered_map<std::string, float>& out) const;

    // Restore widget values after hot reload rebuild
    void restore_values(const WidgetReloadSnapshot& snapshot);
    void restore_values(const std::unordered_map<std::string, float>& snapshot);

    // Deliver any pending async shell results back onto the JS thread.
    void poll_async_results();

    // Flush requestAnimationFrame-style callbacks and pending microtasks from
    // the host frame loop.
    void service_frame_callbacks();

    // Override the repaint invalidator used after JS-driven UI / style
    // changes and to drain `requestAnimationFrame` callbacks.
    //
    // By default the bridge's constructor wires this to call
    // `root.request_repaint()`, which walks up to the host's `repaint()`.
    // Override only when the bridge is part of a larger composite UI whose
    // top-level invalidator is not the same as the root view passed in
    // (e.g. the standalone window's editor uses the WindowHost directly).
    //
    // Without a wired callback, JS `requestAnimationFrame` callbacks queue
    // but never schedule a second paint — see pulp #899.
    void set_repaint_callback(std::function<void()> cb);

    // Override the AI CLI command used by the design tool chat.
    void set_ai_cli_command(std::string cmd);

    // Inspect the native texture backing a canvas element when the native GPU
    // bridge is active. Used by low-level native WebGPU/Skia validation.
    CanvasWidget::NativeGpuTextureFrame describe_native_canvas_frame(const std::string& canvas_id) const;

    // Inspect a native texture allocated through the GPU bridge by texture id.
    // Used by low-level native WebGPU/Skia validation when the source is not a
    // canvas-backed presentation texture.
    CanvasWidget::NativeGpuTextureFrame describe_native_texture_frame(const std::string& texture_id) const;

#ifdef PULP_BENCHMARK
    // Install (or clear) the zero-copy benchmark perf-counter sink. The
    // counters are accumulated inline around WriteBuffer calls on the
    // JS-driven GPU resource setup path (widget_bridge.cpp ~line 3984).
    // Tooling-only; compiled out unless PULP_BENCHMARK is defined.
    void set_bench_counters(render::bench::PerfCounters* counters);
#endif

private:
    struct NativeGpuBridgeState;

    ScriptEngine& engine_;
    View& root_;
    state::StateStore& store_;
    render::GpuSurface* gpu_surface_ = nullptr;
    std::unique_ptr<NativeGpuBridgeState> native_gpu_bridge_state_;

    // Track widgets by ID for JS access
    std::unordered_map<std::string, View*> widgets_;

    // Registered keyboard shortcuts from JS
    struct ShortcutBinding {
        KeyCode key;
        uint16_t modifiers;
        std::string callback;
    };
    std::vector<ShortcutBinding> shortcuts_;

    // Model-agnostic AI CLI command (default: Claude)
    std::string ai_cli_command_ = "claude --print --model {model}";

    struct AsyncExecResult {
        std::string callback_id;
        std::string output;
    };
    std::shared_ptr<std::atomic<bool>> callback_alive_ = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::mutex> async_exec_mutex_ = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<AsyncExecResult>> async_exec_results_ =
        std::make_shared<std::vector<AsyncExecResult>>();
    std::vector<int> pending_frame_ids_;
    bool frame_preamble_loaded_ = false;

    // pulp #915 — native-side timer queue for setTimeout / setInterval.
    // Callbacks themselves live in JS (`__timerCallbacks__`); native
    // tracks (id, deadline, repeat, interval) so service_frame_callbacks()
    // can fire expired timers regardless of which JS-side scheduler the
    // consumer wants to feature-detect against.
    struct PendingTimer {
        int id;
        std::chrono::steady_clock::time_point deadline;
        std::chrono::milliseconds interval;
        bool repeating;
    };
    std::vector<PendingTimer> pending_timers_;

    std::function<void()> repaint_callback_;

#ifdef PULP_BENCHMARK
    render::bench::PerfCounters* bench_counters_ = nullptr;
#endif

    // Resolve parent: returns view for parentId, or &root_ if empty
    View* resolve_parent(const std::string& parent_id);

    // Install on_change/on_toggle callbacks that dispatch to JS
    void wire_callbacks(const std::string& id, View* w);

    // Called by the JS `__requestFrame__` / async-result chain whenever
    // pending work needs the surface to repaint. Routes through
    // `repaint_callback_`, which is wired in the constructor to
    // `root.request_repaint()` by default and may be replaced via
    // `set_repaint_callback`.
    void request_repaint();

    void register_api();
};

} // namespace pulp::view
