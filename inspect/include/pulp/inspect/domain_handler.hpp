// domain_handler.hpp — Dispatches inspector protocol requests to data sources
#pragma once

#include <pulp/inspect/editor_url.hpp>
#include <pulp/inspect/protocol.hpp>

namespace pulp::view { class View; class ScriptInspectorBridge; }
namespace pulp::render { class RenderPassManager; class DirtyTracker; }

namespace pulp::inspect {

class InspectorOverlay;
class StateInspector;
class ConsoleCapture;
class AudioInspector;
class MotionInspector;
class MotionScrubber;
class TraceInspector;
class TweakStore;

/// Handles inspector protocol requests by delegating to the appropriate
/// inspector component. All data sources are optional — missing sources
/// return error responses for their domain's methods.
class DomainHandler {
public:
    DomainHandler() = default;

    // ── Data sources (all optional) ─────────────────────────────────
    void set_root_view(view::View* root) { root_ = root; }
    /// Attach the overlay. Also seeds the overlay's source-jump config
    /// with the handler's current config so the `J` hotkey matches
    /// `Inspector.jumpToSource`. Out-of-line for the same reason as
    /// set_config().
    void set_overlay(InspectorOverlay* overlay);
    void set_state_inspector(StateInspector* state) { state_ = state; }
    void set_console_capture(ConsoleCapture* console) { console_ = console; }
    /// Wire the scripted-UI runtime inspector so `Runtime.getCapabilities`
    /// reports the live engine and `Console`/`Runtime` reflect it. The bridge
    /// marshals evaluation onto the engine thread; without it, those methods
    /// report the engine as unavailable.
    void set_script_inspector(view::ScriptInspectorBridge* bridge) { script_inspector_ = bridge; }

    /// Opt in to `Runtime.evaluate` / `Runtime.interrupt`. OFF by default:
    /// evaluate is arbitrary code execution in the plugin's JS context, and the
    /// inspector transport is unauthenticated, so it must never be reachable
    /// just because a debug console was wired. A host enables this only for a
    /// trusted, dev / loopback session. `Runtime.getCapabilities` reflects the
    /// flag via `canEvaluate`; read-only surfaces (logs, DOM, state) are
    /// unaffected.
    void set_runtime_eval_enabled(bool enabled) { runtime_eval_enabled_ = enabled; }
    void set_audio_inspector(AudioInspector* audio) { audio_ = audio; }
    void set_motion_inspector(MotionInspector* motion) { motion_ = motion; }
    void set_motion_scrubber(MotionScrubber* scrubber) { motion_scrubber_ = scrubber; }
    /// Wire the Perfetto tracing bridge so `Trace.*` (the `pulp trace` CLI)
    /// can drive the process-global session. Optional: unset → Trace methods
    /// return a targeted "no trace inspector attached" error.
    void set_trace_inspector(TraceInspector* trace) { trace_ = trace; }
    void set_render_pass_manager(render::RenderPassManager* rpm) { rpm_ = rpm; }
    void set_tweak_store(TweakStore* store) { tweak_store_ = store; }

    /// Wire the per-frame dirty tracker so the inspector's Performance
    /// tab can toggle `DirtyTracker::set_debug_overlay()` at runtime.
    /// The host installs the tracker once during plugin / app init; if
    /// unset, the toggle silently no-ops.
    void set_dirty_tracker(render::DirtyTracker* dirty) { dirty_ = dirty; }

    // ── Inspector-wide config ───────────────────────────────────────
    /// Replace the runtime config. Mutating accessors below (e.g.
    /// Inspector.setEditorUrlTemplate) update this in place. Also
    /// pushes the config to the attached overlay (if any) so the `J`
    /// source-jump hotkey and the protocol `Inspector.jumpToSource`
    /// share one template. Defined out-of-line so the header doesn't
    /// need the overlay's definition.
    void set_config(InspectorConfig config);
    const InspectorConfig& config() const { return config_; }
    InspectorConfig& mutable_config() { return config_; }

    /// Handle a protocol request. Returns a response message.
    InspectorMessage handle(const InspectorMessage& request);

private:
    view::View* root_ = nullptr;
    InspectorOverlay* overlay_ = nullptr;
    StateInspector* state_ = nullptr;
    ConsoleCapture* console_ = nullptr;
    view::ScriptInspectorBridge* script_inspector_ = nullptr;
    bool runtime_eval_enabled_ = false;
    AudioInspector* audio_ = nullptr;
    MotionInspector* motion_ = nullptr;
    MotionScrubber* motion_scrubber_ = nullptr;
    TraceInspector* trace_ = nullptr;
    render::RenderPassManager* rpm_ = nullptr;
    render::DirtyTracker* dirty_ = nullptr;
    TweakStore* tweak_store_ = nullptr;
    InspectorConfig config_{};

    // Domain handlers
    InspectorMessage handle_inspector(const InspectorMessage& req);
    InspectorMessage handle_dom(const InspectorMessage& req);
    InspectorMessage handle_css(const InspectorMessage& req);
    InspectorMessage handle_performance(const InspectorMessage& req);
    InspectorMessage handle_state(const InspectorMessage& req);
    InspectorMessage handle_console(const InspectorMessage& req);
    InspectorMessage handle_runtime(const InspectorMessage& req);
    InspectorMessage handle_audio(const InspectorMessage& req);
    InspectorMessage handle_capture(const InspectorMessage& req);
    InspectorMessage handle_motion(const InspectorMessage& req);
    InspectorMessage handle_trace(const InspectorMessage& req);
    InspectorMessage handle_live_constant(const InspectorMessage& req);
};

} // namespace pulp::inspect
