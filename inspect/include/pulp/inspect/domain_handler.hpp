// domain_handler.hpp — Dispatches inspector protocol requests to data sources
#pragma once

#include <pulp/inspect/protocol.hpp>

namespace pulp::view { class View; }
namespace pulp::render { class RenderPassManager; }

namespace pulp::inspect {

class InspectorOverlay;
class StateInspector;
class ConsoleCapture;
class AudioInspector;
class MotionInspector;

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
    void set_render_pass_manager(render::RenderPassManager* rpm) { rpm_ = rpm; }

    /// Handle a protocol request. Returns a response message.
    InspectorMessage handle(const InspectorMessage& request);

private:
    view::View* root_ = nullptr;
    InspectorOverlay* overlay_ = nullptr;
    StateInspector* state_ = nullptr;
    ConsoleCapture* console_ = nullptr;
    AudioInspector* audio_ = nullptr;
    MotionInspector* motion_ = nullptr;
    render::RenderPassManager* rpm_ = nullptr;

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
};

} // namespace pulp::inspect
