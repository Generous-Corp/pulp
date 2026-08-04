// domain_handler.hpp — Dispatches inspector protocol requests to data sources
#pragma once

#include <pulp/inspect/agent_context.hpp>
#include <pulp/inspect/capture_source.hpp>
#include <pulp/inspect/editor_url.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/inspect/publication_binding.hpp>
#include <pulp/inspect/runtime_evaluator.hpp>
#include <pulp/inspect/test_input.hpp>

#include <memory>
#include <string>
#include <utility>

namespace pulp::view { class View; }
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
class DomainHandler : public InspectorDomainPublicationBindings {
public:
    DomainHandler() = default;

    // ── Data sources (all optional) ─────────────────────────────────
    void set_root_view(view::View* root) { root_ = root; }
    void set_agent_context_source(InspectorAgentContextSource* source) {
        agent_context_ = source;
    }
    void set_capture_source(InspectorCaptureSource* source) {
        capture_ = source;
    }
    /// Attach the overlay. Also seeds the overlay's source-jump config
    /// with the handler's current config so the `J` hotkey matches
    /// `Inspector.jumpToSource`. Out-of-line for the same reason as
    /// set_config().
    void set_overlay(InspectorOverlay* overlay);
    void set_state_inspector(StateInspector* state) { state_ = state; }
    void set_console_capture(ConsoleCapture* console) { console_ = console; }
    /// Inject the separately linked arbitrary-execution component. Without it,
    /// Runtime status remains available but evaluation reports no engine.
    void set_runtime_evaluator(RuntimeEvaluator* evaluator) {
        runtime_evaluator_ = evaluator;
    }
    /// Dispatch one Runtime request with a scoped borrowed evaluator. The
    /// previous binding is restored on every exit, including exceptions.
    InspectorMessage handle_runtime_with_evaluator(
        const InspectorMessage& request, RuntimeEvaluator* evaluator);

    /// Opt in to `Runtime.evaluate` / `Runtime.interrupt`. OFF by default:
    /// evaluate is arbitrary code execution in the plugin's JS context, and the
    /// authenticated inspector transport does not make arbitrary evaluation
    /// safe to expose merely because a debug console was wired. A host enables
    /// this only for a trusted development session. `Runtime.getCapabilities`
    /// reflects the flag via `canEvaluate`; read-only surfaces (logs, DOM,
    /// state) are unaffected.
    void set_runtime_eval_enabled(bool enabled) {
        runtime_eval_enabled_ = enabled;
        runtime_eval_denial_.clear();
    }
    /// Fail-closed runtime-eval state with an exact host-owned diagnostic.
    /// This does not alter the attached JS realm or mask any of its globals.
    void set_runtime_eval_denied(std::string reason) {
        runtime_eval_enabled_ = false;
        runtime_eval_denial_ = std::move(reason);
    }
    void set_audio_inspector(AudioInspector* audio) { audio_ = audio; }
    void set_motion_inspector(MotionInspector* motion) { motion_ = motion; }
    void set_motion_scrubber(MotionScrubber* scrubber) { motion_scrubber_ = scrubber; }
    /// Wire and retain the Perfetto tracing bridge used by both `Trace.*`
    /// dispatch and the server's publication-scoped ownership lease.
    void set_trace_inspector(std::shared_ptr<TraceInspector> trace);
    /// Source-compatible dispatch-only wiring. A server exposing
    /// `trace.session.control` rejects startup until shared ownership is supplied.
    void set_trace_inspector(TraceInspector* trace) {
        trace_binding_.reset();
        trace_ = trace;
    }
    std::vector<InspectorPublicationBindingRegistration>
    publication_bindings() const override;
    void set_render_pass_manager(render::RenderPassManager* rpm) { rpm_ = rpm; }
    void set_tweak_store(TweakStore* store) { tweak_store_ = store; }
    void set_test_input_source(InspectorTestInputSource* source) {
        test_input_.set_source(source);
    }
    InspectorTestInputSource* test_input_source() const {
        return test_input_.source();
    }
    void release_test_input(TestInputReleaseReason reason) noexcept {
        test_input_.release(reason);
    }

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
    InspectorAgentContextSource* agent_context_ = nullptr;
    InspectorCaptureSource* capture_ = nullptr;
    InspectorOverlay* overlay_ = nullptr;
    StateInspector* state_ = nullptr;
    ConsoleCapture* console_ = nullptr;
    RuntimeEvaluator* runtime_evaluator_ = nullptr;
    bool runtime_eval_enabled_ = false;
    std::string runtime_eval_denial_;
    AudioInspector* audio_ = nullptr;
    MotionInspector* motion_ = nullptr;
    MotionScrubber* motion_scrubber_ = nullptr;
    TraceInspector* trace_ = nullptr;
    std::shared_ptr<TraceInspector> trace_binding_;
    render::RenderPassManager* rpm_ = nullptr;
    render::DirtyTracker* dirty_ = nullptr;
    TweakStore* tweak_store_ = nullptr;
    TestInputDomain test_input_;
    InspectorConfig config_{};

    // Domain handlers
    InspectorMessage handle_inspector(const InspectorMessage& req);
    InspectorMessage handle_dom(const InspectorMessage& req);
    InspectorMessage handle_css(const InspectorMessage& req);
    InspectorMessage handle_performance(const InspectorMessage& req);
    InspectorMessage handle_state(const InspectorMessage& req);
    InspectorMessage handle_test(const InspectorMessage& req);
    InspectorMessage handle_console(const InspectorMessage& req);
    InspectorMessage handle_runtime(const InspectorMessage& req);
    InspectorMessage handle_audio(const InspectorMessage& req);
    InspectorMessage handle_capture(const InspectorMessage& req);
    InspectorMessage handle_motion(const InspectorMessage& req);
    InspectorMessage handle_trace(const InspectorMessage& req);
    InspectorMessage handle_live_constant(const InspectorMessage& req);
};

} // namespace pulp::inspect
