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
#include <pulp/view/reload_capabilities.hpp>
#include <pulp/state/store.hpp>

namespace pulp::view { struct ClaudeBundle; }
#include <chrono>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
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
    // Custom widget-declared state (item 1.4b): opaque per-widget blobs from
    // View::save_reload_state, keyed by script id, restored via restore_reload_state.
    std::unordered_map<std::string, std::string> custom_state;
    // Legacy compat
    std::unordered_map<std::string, float> values;
};

// Bridges JS scripts to the Pulp widget system.
// Registers native functions that JS code calls to create, configure,
// layout, style, and interact with widgets.
class WidgetBridge {
public:
    // `granted_capabilities` scopes the EFFECTFUL bridge-API groups (exec,
    // clipboard, filesystem, storage, ai, runtime_import). Ungranted groups are
    // never registered — the JS symbol is absent. The default all() leaves every
    // group present (the full-featured posture for trusted/local UIs); a host that
    // runs untrusted UI constructs with only the declared set (empty = most
    // restrictive). Pure-UI groups ignore this. See reload_capabilities.hpp.
    WidgetBridge(ScriptEngine& engine, View& root, state::StateStore& store,
                 render::GpuSurface* gpu_surface = nullptr,
                 CapabilitySet granted_capabilities = CapabilitySet::all());
    ~WidgetBridge();

    // Attach (or replace) the GPU surface AFTER construction. Mirrors the
    // 4th constructor argument but for the common case where the WidgetBridge
    // is built before the host's GpuSurface exists (e.g. AUv3 editor lifecycle:
    // the format adapter opens ViewBridge / ScriptedUiSession first, then
    // builds the PluginViewHost — only THEN is `host->gpu_surface()` non-null).
    //
    // Without this attach call, the JS-side navigator.gpu / canvas.getContext
    // ('webgpu') bridge falls through to mocks and Three.js renders black.
    //
    // Lifetime: the GpuSurface must outlive this WidgetBridge. Pass `nullptr`
    // to detach (e.g. during host teardown). Idempotent — calling with the
    // same surface twice is a no-op.
    void attach_gpu_surface(render::GpuSurface* gpu_surface);

    // Read-back accessor for diagnostics + tests.
    render::GpuSurface* gpu_surface() const noexcept { return gpu_surface_; }

    // True iff a GpuSurface is attached AND its adapter reports
    // `native_bridge=true` (i.e. JS navigator.gpu / canvas.getContext('webgpu')
    // will route through Pulp's Dawn).
    bool has_native_gpu_bridge() const noexcept;

    // Load and execute a UI script
    void load_script(const std::string& code);

    /// Set reviewed local roots used by JS asset loading for relative URLs.
    /// These are scoped to this bridge instance and are intended for
    /// post-review UI-kit assets copied under `pulp-kits/<kit-id>/`.
    void set_asset_roots(std::vector<std::filesystem::path> roots) {
        asset_roots_.clear();
        asset_roots_.reserve(roots.size());
        for (auto& root : roots) {
            if (root.empty()) continue;
            std::error_code ec;
            auto abs = std::filesystem::absolute(root, ec).lexically_normal();
            if (ec) abs = root.lexically_normal();
            asset_roots_.push_back(std::move(abs));
        }
    }
    const std::vector<std::filesystem::path>& asset_roots() const noexcept { return asset_roots_; }

    /// load_script overload that retains a script identifier. `script_id` is
    /// recorded as `active_script_id_` and threaded into every rAF callback
    /// registered while the script is active so `motion.publishValue(...)` calls
    /// from inside an rAF body emit events tagged with `source_kind="rAF"`,
    /// `source_id="<script_id>:<callback_id>"`. The legacy overload continues
    /// to work unchanged.
    void load_script(const std::string& code, const std::string& script_id);

    /// Set the active script identifier without re-evaluating a script.
    /// Useful when the same engine evaluates fragments from multiple
    /// sources and the caller wants subsequent rAF/timer callbacks to
    /// attribute to a different surface. Pass an empty string to clear.
    void set_active_script_id(const std::string& script_id);

    const std::string& active_script_id() const noexcept { return active_script_id_; }

    // Get a widget by its JS-assigned ID
    View* widget(const std::string& id);

    // Sync all widget values from the parameter store
    void sync_from_store();

    // Hot reload support: clear all JS-created widgets
    void clear();

    // Forward a key event to JS (called by host for global shortcuts).
    // Per-instance entry; usually invoked by `dispatch_global_key` below.
    void forward_key_event(int key_code, uint16_t modifiers, bool is_down);

    // Static fan-out: forward a key event to every live WidgetBridge.
    //
    // Platform hosts call this from their key-event paths so that any
    // app using @pulp/react / WidgetBridge gets `window.addEventListener
    // ('keydown', ...)` and `registerShortcut(...)` delivery without the
    // app having to install its own `View::on_global_key` lambda.
    // Bridges register themselves in the static `all_bridges_` set in
    // their constructor and unregister in their destructor; the set is
    // mutex-protected because bridge construction can happen off the
    // platform main thread in some embedding contexts.
    static void dispatch_global_key(int key_code, uint16_t modifiers, bool is_down);

    // Static fan-out: fire a document-level event (`mousedown`,
    // `pointerdown`, etc.) into every live WidgetBridge's JS document.
    // Used by platform hosts to fire synthetic outside-click events on
    // Esc so React popovers that use the
    // `document.addEventListener('mousedown', onDoc)` click-outside
    // idiom close automatically. `event_json_literal` is a JS object
    // literal expression INCLUDING braces, e.g. `{clientX:-1,clientY:-1,target:null}`.
    static void dispatch_document_event(const std::string& event_type,
                                        const std::string& event_json_literal);

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

    // ── Declarative native→widget bindings (params / meters) ──────────────
    //
    // A binding is registered ONCE from JS via `bindWidgetToParam(widgetId,
    // paramName, transform?)` or `bindMeter(widgetId, paramName, transform?)`.
    // Thereafter the widget's value is pushed from the atomic param store to
    // the widget every frame in C++ — off the host FrameClock, with ZERO
    // per-frame JS bridge crossing — by service_param_bindings(), which
    // service_frame_callbacks() drives on the host idle cadence.
    //
    // This replaces the requestAnimationFrame loop a live-metering / param-
    // following UI would otherwise run (the one thing that guarantees QuickJS
    // executes every vsync). Precedence: a value widget (knob/fader/slider/
    // toggle/progress) is re-asserted from the store EVERY frame, so a stray
    // explicit setValue is corrected on the next frame; a bound Meter is updated
    // whenever its source changes (Meter::set_level repaints unconditionally, so
    // it is not touched on a static frame). Either way the binding yields while
    // the user is dragging that widget — the gesture wins (View::is_gesture_active()).
    // See bind_* in state_binding_api.cpp.
    void service_param_bindings();

    // Number of live param/meter bindings (diagnostics + tests).
    std::size_t param_binding_count() const noexcept { return param_bindings_.size(); }

    // ── Runtime design import ─────────────────────────────────────
    //
    // Registers the `__pulpRuntimeImport__(html, source)` and
    // `__pulpRuntimeSettle__(rounds)` native functions on the bridge's
    // JS engine. These are the C++ side of `@pulp/react/runtime-import`'s
    // `renderFromDesign()`: they evaluate a design bundle's HTML in
    // the LIVE bridge engine (not a fresh sandbox) and pump the
    // settle loop using this bridge's own service_frame_callbacks.
    //
    // Call ONCE after constructing the bridge, before consumer code
    // calls renderFromDesign(). Subsequent calls are no-ops.
    //
    void install_runtime_import_handlers();

private:
    // Internal helper used by __pulpRuntimeImport__. Forward-declared in
    // the design_import unit so the implementation lives next to the
    // related offline boot code. Installs navigator/HTML*Element/etc.
    // sandbox shims on engine_ (idempotent), evaluates the bundle's
    // asset payloads (React/ReactDOM/Babel) preserving any host React
    // already installed on globalThis, evaluates inline text/javascript
    // and text/babel scripts, dispatches DOMContentLoaded. Skips
    // buildDom + walkDomJson — the runtime path is reconciler-owned.
    void evaluate_claude_bundle_in_live_engine(const ClaudeBundle& bundle);
public:

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
    // but never schedule a second paint.
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
    // JS-driven GPU resource setup path in widget_bridge/gpu_api.cpp.
    // Tooling-only; compiled out unless PULP_BENCHMARK is defined.
    void set_bench_counters(render::bench::PerfCounters* counters);
#endif

private:
    struct NativeGpuBridgeState;

    ScriptEngine& engine_;
    View& root_;
    state::StateStore& store_;
    // Effectful-API-group gate. See the constructor doc + reload_capabilities.hpp.
    CapabilitySet granted_capabilities_;
    render::GpuSurface* gpu_surface_ = nullptr;
    std::unique_ptr<NativeGpuBridgeState> native_gpu_bridge_state_;
    std::vector<std::filesystem::path> asset_roots_;

    // Track widgets by ID for JS access
    std::unordered_map<std::string, View*> widgets_;

    // Idempotency guards for native-event registrations. Each registrar
    // (registerPointer / registerWheel / etc.) wraps the previous
    // on_pointer_event so calling N times stacks N lambdas — every
    // re-render of a React tree that re-runs the registration would
    // multiply the dispatch cost by the render count. These sets gate
    // the registrations so each (widget id, channel) wires the native
    // hook exactly once.
    std::unordered_set<std::string> pointer_registered_;
    std::unordered_set<std::string> wheel_registered_;

    // Registered keyboard shortcuts from JS
    struct ShortcutBinding {
        KeyCode key;
        uint16_t modifiers;
        std::string callback;
    };
    std::vector<ShortcutBinding> shortcuts_;

    /// Application-wide @keyframes registry. `defineKeyframes(name, stops)`
    /// populates this; `setAnimation` looks up by name when resolving
    /// animation-name. Per-View playback state lives on
    /// View::active_animations_.
    CssKeyframesRegistry css_keyframes_registry_;
public:
    CssKeyframesRegistry& css_keyframes_registry() { return css_keyframes_registry_; }
    const CssKeyframesRegistry& css_keyframes_registry() const { return css_keyframes_registry_; }
private:

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
    // Identity of the most-recently loaded script (via
    // `load_script(code, script_id)` or `set_active_script_id`). When
    // non-empty, rAF callbacks and `motion.publishValue` bindings use
    // it as the `source_id` prefix on emitted motion events.
    std::string active_script_id_;
    // install_runtime_import_handlers() is idempotent. Tracked per-bridge
    // (not as a static thread_local on `this`, because heap reuse across tests
    // gave duplicate addresses → spurious skip).
    bool runtime_import_installed_ = false;

    // Native-side timer queue for setTimeout / setInterval. Callbacks
    // themselves live in JS (`__timerCallbacks__`); native tracks (id,
    // deadline, repeat, interval) so service_frame_callbacks() can fire expired
    // timers regardless of which JS-side scheduler the consumer wants to
    // feature-detect against.
    struct PendingTimer {
        int id;
        std::chrono::steady_clock::time_point deadline;
        std::chrono::milliseconds interval;
        bool repeating;
    };
    std::vector<PendingTimer> pending_timers_;

    // Declarative native→widget binding (see service_param_bindings()).
    // Transform applied to the source before it reaches the widget, in order:
    // optional dB→linear map → scale → offset → clamp. The default is the
    // identity on the store's already-normalized [0,1] value, which is the
    // common case (a knob/fader/meter tracking a normalized param 1:1).
    struct BindingTransform {
        bool db = false;        ///< read the RAW param value and map [db_min,db_max]→[0,1]
        float db_min = -60.0f;
        float db_max = 0.0f;
        float scale = 1.0f;
        float offset = 0.0f;
        bool clamp = true;      ///< clamp the result to [clamp_min, clamp_max]
        float clamp_min = 0.0f;
        float clamp_max = 1.0f;
        float apply(float v) const;
    };
    struct ParamBinding {
        enum class Target { value, meter };
        std::string widget_id;
        state::ParamID param_id = 0;   ///< source param (resolved once at bind time)
        Target target = Target::value;
        BindingTransform transform;
        float last_applied = std::numeric_limits<float>::quiet_NaN();  ///< skip repaint when unchanged
    };
    std::vector<ParamBinding> param_bindings_;
    // Register (or replace) a binding for `widget_id`. Resolves `param_name` to
    // its id; returns false (no binding added) if the store has no such param.
    // `transform` is the optional JS transform object (may be null → identity).
    bool add_param_binding(const std::string& widget_id,
                           const std::string& param_name,
                           ParamBinding::Target target,
                           const choc::value::Value* transform);
    // Parse the optional JS transform object (`{db,dbMin,dbMax,scale,offset,
    // min,max,clamp}`) into a BindingTransform. Null / non-object → identity.
    static BindingTransform parse_transform(const choc::value::Value* v);
    // Resolve a param NAME to its id via the store. Returns false when the
    // store has no param with that name (the binding is then not registered).
    bool resolve_param_id(const std::string& name, state::ParamID& out) const;
    // Drop bindings whose widget id no longer resolves in widgets_ (after a
    // removeWidget subtree teardown) so a later widget reusing that id is not
    // silently re-bound.
    void prune_dangling_bindings();
    // Forget every native bridge reference to the removed subtree. This is
    // stronger than erasing widgets_: native event-registration guards are also
    // keyed by id and must be cleared before recycled ids can be rewired.
    void forget_widget_subtree(View* node);
    // Push one binding's transformed source value onto the resolved widget.
    // Writes the widget only when the transformed value changed since the last
    // frame; returns true on a change so the caller schedules one repaint.
    bool apply_param_binding(ParamBinding& binding, View* w);

    std::function<void()> repaint_callback_;

#ifdef PULP_BENCHMARK
    render::bench::PerfCounters* bench_counters_ = nullptr;
#endif

    // Resolve parent: returns view for parentId, or &root_ if empty
    View* resolve_parent(const std::string& parent_id);

    // Install on_change/on_toggle callbacks that dispatch to JS
    void wire_callbacks(const std::string& id, View* w);

    // Single source-of-truth table that maps a lowercase widget tag
    // (`knob`/`fader`/`toggle`/`combo`/`checkbox`/`spectrum`/`waveform`/
    // `meter`/`xypad`/`listbox`/`icon`, plus the `select`/`progress`/`img`
    // HTML aliases) to a freshly constructed, id-set, callback-wired native
    // widget. Returns nullptr for non-widget tags so callers fall through to
    // their container default. Used by the `__domAppend` React-commit fast path;
    // mirrors the `createX` factory functions and the JS `_ensureNative` /
    // `@pulp/react` host-config maps so the four widget-routing surfaces can't
    // drift again (asserted by the routing-parity test).
    std::unique_ptr<View> make_widget_for_tag(const std::string& tag,
                                              const std::string& id);

    // Called by the JS `__requestFrame__` / async-result chain whenever
    // pending work needs the surface to repaint. Routes through
    // `repaint_callback_`, which is wired in the constructor to
    // `root.request_repaint()` by default and may be replaced via
    // `set_repaint_callback`.
    void request_repaint();

    void register_api();
    void register_accessibility_api();
    void register_dom_api();
    void register_hover_event_api();
    void register_pointer_event_api();
    void register_wheel_event_api();
    void register_context_menu_event_api();
    void register_drop_event_api();
    void register_widget_style_background_color_api(std::function<canvas::Color(const std::string&)> parse_color);
    void register_widget_style_shadow_api(std::function<canvas::Color(const std::string&)> parse_color);
    void register_widget_style_opacity_api();
    void register_widget_style_overflow_api();
    void register_widget_style_background_gradient_api(std::function<canvas::Color(const std::string&)> parse_color);
    void register_widget_style_box_shadow_api(std::function<canvas::Color(const std::string&)> parse_color);
    void register_widget_style_cursor_direction_api();
    void register_widget_style_filter_clip_api(std::function<canvas::Color(const std::string&)> parse_color);
    void register_widget_style_blend_api();
    void register_widget_style_rn_compat_api(std::function<canvas::Color(const std::string&)> parse_color);
    void register_widget_style_state_api(std::function<canvas::Color(const std::string&)> parse_color);
    void register_widget_style_background_repeat_api();
    void register_widget_style_mask_object_api();
    void register_widget_style_background_subproperty_api();
    void register_widget_style_visibility_api();
    void register_widget_style_interaction_api();
    void register_layout_grid_api();
    void register_layout_flex_api();
    void register_layout_query_api();
    void register_layout_box_model_api();
    void register_layout_position_api();
    void register_list_style_api();
    void register_metadata_removal_api();
    void register_metadata_source_api();
    void register_metadata_computed_api();
    void register_platform_services_ai_api();
    void register_platform_services_exec_api();
    void register_platform_services_dialog_api();
    void register_platform_services_clipboard_api();
    void register_state_binding_api();
    void register_storage_key_value_api();
    void register_asset_loading_api();
    void register_font_assets_api();
    void register_svg_api(std::function<canvas::Color(const std::string&)> parse_color);
    void register_shader_widget_api();
    void register_shader_canvas_api();
    void register_theme_api();
    void register_tokens_api(std::function<canvas::Color(const std::string&)> parse_color);
    void register_widget_assets_api();
    void register_widget_schema_api();
    void register_widget_factory_controls_api();
    void register_widget_value_controls_api();
    void register_widget_factory_form_api();
    void register_widget_factory_container_api();
    void register_widget_factory_composite_api();
    void register_widget_value_list_api();
    void register_widget_factory_text_editor_api();
    void register_widget_factory_design_system_api();
    void register_widget_value_label_api();
    void register_widget_value_basic_api();
    void register_widget_typography_api();
    void register_widget_typography_color_api(std::function<canvas::Color(const std::string&)> parse_color);
    void register_widget_typography_decoration_api(std::function<canvas::Color(const std::string&)> parse_color);
    void register_widget_typography_overflow_api();
    void register_widget_typography_extended_api();
    void register_widget_typography_shadow_shorthand_api();
    void register_widget_value_content_api();
    void register_widget_text_runs_api(std::function<canvas::Color(const std::string&)> parse_color);
    void register_widget_border_box_api(std::function<canvas::Color(const std::string&)> parse_color);
    void register_widget_outline_api(std::function<canvas::Color(const std::string&)> parse_color);
    void register_widget_border_radius_api();
    void register_widget_border_side_api(std::function<canvas::Color(const std::string&)> parse_color);
    void register_runtime_api();
    void register_animation_api();
    void register_animation_style_api();
    void register_canvas2d_api(std::function<canvas::Color(const std::string&)> parse_color);
    void register_gpu_api();
};

} // namespace pulp::view
