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
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>

namespace pulp::render {
class GpuSurface;
}

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

    // Request a repaint after JS-driven UI or style changes.
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
    bool dom_ops_loaded_ = false;
    bool frame_preamble_loaded_ = false;
    std::function<void()> repaint_callback_;

    // Resolve parent: returns view for parentId, or &root_ if empty
    View* resolve_parent(const std::string& parent_id);

    // Install on_change/on_toggle callbacks that dispatch to JS
    void wire_callbacks(const std::string& id, View* w);

    void request_repaint();

    void register_api();
};

} // namespace pulp::view
