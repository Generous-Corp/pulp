// domain_handler.hpp — Dispatches inspector protocol requests to data sources
#pragma once

#include <pulp/inspect/editor_url.hpp>
#include <pulp/inspect/protocol.hpp>

namespace pulp::view { class View; }
namespace pulp::render { class RenderPassManager; class DirtyTracker; }

namespace pulp::inspect {

class InspectorOverlay;
class StateInspector;
class ConsoleCapture;
class AudioInspector;
class MotionInspector;
class MotionScrubber;
class TweakStore;

/// Handles inspector protocol requests by delegating to the appropriate
/// inspector component. All data sources are optional — missing sources
/// return error responses for their domain's methods.
class DomainHandler {
public:
    DomainHandler() = default;

    // ── Data sources (all optional) ─────────────────────────────────
    void set_root_view(view::View* root) { root_ = root; }
    void set_overlay(InspectorOverlay* overlay) { overlay_ = overlay; }
    void set_state_inspector(StateInspector* state) { state_ = state; }
    void set_console_capture(ConsoleCapture* console) { console_ = console; }
    void set_audio_inspector(AudioInspector* audio) { audio_ = audio; }
    void set_motion_inspector(MotionInspector* motion) { motion_ = motion; }
    void set_motion_scrubber(MotionScrubber* scrubber) { motion_scrubber_ = scrubber; }
    void set_render_pass_manager(render::RenderPassManager* rpm) { rpm_ = rpm; }
    void set_tweak_store(TweakStore* store) { tweak_store_ = store; }

    /// Tier A Slice 6: wire the per-frame dirty tracker so the inspector's
    /// Performance tab can toggle `DirtyTracker::set_debug_overlay()` at
    /// runtime. The host installs the tracker once during plugin / app
    /// init; if unset, the toggle silently no-ops.
    void set_dirty_tracker(render::DirtyTracker* dirty) { dirty_ = dirty; }

    // ── Inspector-wide config ───────────────────────────────────────
    /// Replace the runtime config (Phase 5.3: editor_url_template).
    /// Mutating accessors below (e.g. Inspector.setEditorUrlTemplate)
    /// update this in place.
    void set_config(InspectorConfig config) { config_ = std::move(config); }
    const InspectorConfig& config() const { return config_; }
    InspectorConfig& mutable_config() { return config_; }

    /// Handle a protocol request. Returns a response message.
    InspectorMessage handle(const InspectorMessage& request);

private:
    view::View* root_ = nullptr;
    InspectorOverlay* overlay_ = nullptr;
    StateInspector* state_ = nullptr;
    ConsoleCapture* console_ = nullptr;
    AudioInspector* audio_ = nullptr;
    MotionInspector* motion_ = nullptr;
    MotionScrubber* motion_scrubber_ = nullptr;
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
    InspectorMessage handle_live_constant(const InspectorMessage& req);
};

} // namespace pulp::inspect
