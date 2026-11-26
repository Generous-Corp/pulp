#pragma once

// GraphEditorView: a node editor for pulp::host::SignalGraph, drawn as a
// Vellum view. Nodes are draggable boxes with input ports on the left and
// output ports on the right. Dragging from one port to a port on the other side
// connects them; Shift makes the edge a feedback edge and Alt makes it a MIDI
// edge. Every connection goes through the SignalGraph, so a live graph is
// re-snapshotted for the audio thread exactly as for any other edit.
//
// The editor links pulp::host and Vellum's view layer and nothing else from
// Pulp's UI stack. Opening a node's plug-in editor window is left to the
// application: set_node_activated_handler() reports a double click on a node
// and the application decides what that opens.

#include <pulp/host/graph_types.hpp>
#include <vellum/view/view.hpp>

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace pulp::host {
class SignalGraph;
struct GraphNode;
struct Connection;
} // namespace pulp::host

namespace pulp::vellum_graph {

class GraphEditorView final : public ::vellum::view::View {
  public:
    enum class EdgeKind { Audio, Midi, Automation, Feedback };

    explicit GraphEditorView(host::SignalGraph& graph);

    /// Place a node. Nodes without a position get a grid slot from auto_layout().
    void set_node_position(host::NodeId id, float x, float y);

    /// Assign a grid slot to every node that has no position yet. The
    /// constructor calls it; call it again after adding nodes.
    void auto_layout();

    /// The node's top-left corner in view coordinates.
    ::vellum::view::Point node_position(host::NodeId id) const;

    /// The node selected by the last click, or 0 when nothing is selected.
    host::NodeId selected_node() const {
        return selected_node_;
    }

    /// True while a connection is being dragged out of a port.
    bool is_connecting() const {
        return dragging_;
    }

    /// Called with the node's id when a node is double-clicked.
    void set_node_activated_handler(std::function<void(host::NodeId)> handler) {
        on_node_activated_ = std::move(handler);
    }

    static constexpr float kNodeWidth = 160.0f;
    static constexpr float kNodeHeight = 80.0f;
    static constexpr float kPortRadius = 6.0f;
    static constexpr float kPortSpacing = 18.0f;

    /// Port centres, in view coordinates, for a node placed at its position.
    ::vellum::view::Point input_port_position(host::NodeId id, int port) const;
    ::vellum::view::Point output_port_position(host::NodeId id, int port) const;

    void paint(::vellum::canvas::Canvas& canvas) override;

    void on_mouse_event(const ::vellum::view::MouseEvent& event) override;
    void on_mouse_down(::vellum::view::Point pos) override;
    void on_mouse_drag(::vellum::view::Point pos) override;
    void on_mouse_up(::vellum::view::Point pos) override;
    bool wants_mouse_input() const override {
        return true;
    }

  private:
    using Pos = ::vellum::view::Point;

    bool hit_node(float x, float y, host::NodeId& out_id) const;
    bool hit_input_port(float x, float y, host::NodeId& out_id, int& out_port) const;
    bool hit_output_port(float x, float y, host::NodeId& out_id, int& out_port) const;

    void paint_node(::vellum::canvas::Canvas& canvas, const host::GraphNode& node) const;
    void paint_connection(::vellum::canvas::Canvas& canvas, const host::Connection& conn) const;
    void paint_ghost(::vellum::canvas::Canvas& canvas) const;

    host::SignalGraph& graph_;
    std::unordered_map<host::NodeId, Pos> positions_;

    bool dragging_ = false;
    host::NodeId drag_src_node_ = 0;
    int drag_src_port_ = -1;
    bool drag_src_is_output_ = true;
    Pos drag_cursor_{0.0f, 0.0f};

    host::NodeId selected_node_ = 0;

    // Modifiers of the latest mouse event; the point-only press, drag and
    // release callbacks do not carry them, and the release picks the edge kind.
    std::uint16_t last_modifiers_ = 0;

    std::function<void(host::NodeId)> on_node_activated_;
};

} // namespace pulp::vellum_graph
