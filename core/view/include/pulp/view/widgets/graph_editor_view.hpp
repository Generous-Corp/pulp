#pragma once

// GraphEditorView — canvas-based node editor on top of pulp::canvas.
//
// Renders a SignalGraph as a draggable, connectable node graph. Drag-to-
// connect creates audio / MIDI / automation / feedback edges (modifier keys
// distinguish the variants). Live-patchable: every connection mutation
// invalidates the SignalGraph's CompiledGraph snapshot, so the audio thread
// transitions through one block of silence and back without tearing.
//
// Phase 2 of planning/signal-graph-followups-plan.md.

#include <pulp/host/signal_graph.hpp>
#include <pulp/view/view.hpp>
#include <pulp/canvas/canvas.hpp>
#include <unordered_map>
#include <string>

namespace pulp::view::widgets {

class GraphEditorView : public View {
public:
    explicit GraphEditorView(host::SignalGraph& graph) : graph_(graph) {
        auto_layout();
    }

    // Position a node manually (by default add_*_node positions are auto-
    // assigned in a grid). Persist positions in your own StateTree if you
    // want them to survive save/load.
    void set_node_position(host::NodeId id, float x, float y) {
        positions_[id] = {x, y};
    }

    // Recompute auto-layout. Called from the constructor; call again after
    // adding/removing nodes if you don't want to set_node_position manually.
    void auto_layout();

    void paint(canvas::Canvas& c) override;

    // Use the dispatched legacy mouse callbacks (down/up/drag) so press
    // and release each get their own entry point — on_mouse_event would
    // require disambiguating press/release/drag from MouseEvent::is_down,
    // which platforms set inconsistently during in-flight drags.
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;
    void on_mouse_up(Point pos) override;

    // Modifier-key state for the next on_mouse_up: the legacy down/up
    // callbacks pass only Point, so we capture modifiers via on_mouse_event
    // (which fires before the dispatch to legacy handlers) and consume
    // them in on_mouse_up to pick the connect variant.
    void on_mouse_event(const MouseEvent& ev) override;

    enum class EdgeKind { Audio, Midi, Automation, Feedback };

private:
    struct Pos { float x, y; };

    static constexpr float kNodeWidth  = 160.0f;
    static constexpr float kNodeHeight = 80.0f;
    static constexpr float kPortRadius = 6.0f;
    static constexpr float kPortSpacing = 18.0f;
    static constexpr float kCornerRadius = 8.0f;

    // ── Geometry helpers ────────────────────────────────────────────
    Pos node_origin_(host::NodeId id) const;
    Pos input_port_pos_(host::NodeId id, int port) const;
    Pos output_port_pos_(host::NodeId id, int port) const;
    bool hit_node_(float mx, float my, host::NodeId& out_id) const;
    bool hit_input_port_(float mx, float my, host::NodeId& out_id, int& out_port) const;
    bool hit_output_port_(float mx, float my, host::NodeId& out_id, int& out_port) const;

    void paint_node_(canvas::Canvas& c, const host::GraphNode& n);
    void paint_connection_(canvas::Canvas& c, const host::Connection& conn);
    void paint_ghost_(canvas::Canvas& c);

    // ── State ───────────────────────────────────────────────────────
    host::SignalGraph& graph_;
    std::unordered_map<host::NodeId, Pos> positions_;

    // Drag-to-connect state.
    bool dragging_ = false;
    host::NodeId drag_src_node_ = 0;
    int drag_src_port_ = -1;
    bool drag_src_is_output_ = true;
    Pos drag_cursor_{0, 0};

    // Selection (for delete via backspace; not yet wired into key events).
    host::NodeId selected_node_ = 0;

    // Last-seen modifier state from on_mouse_event, consumed in on_mouse_up
    // to pick the connect/connect_midi/connect_feedback variant.
    uint16_t last_modifiers_ = 0;
};

} // namespace pulp::view::widgets
