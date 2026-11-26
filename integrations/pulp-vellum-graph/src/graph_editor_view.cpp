#include <pulp/vellum_graph/graph_editor_view.hpp>

#include <pulp/host/signal_graph.hpp>
#include <vellum/canvas/canvas.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::vellum_graph {

namespace {

using ::vellum::canvas::Color;

constexpr Color kAudioColor = {0.30f, 0.55f, 0.95f, 1.0f};
constexpr Color kMidiColor = {0.30f, 0.85f, 0.45f, 1.0f};
constexpr Color kAutomationColor = {0.95f, 0.85f, 0.20f, 1.0f};
constexpr Color kFeedbackColor = {0.95f, 0.40f, 0.40f, 1.0f};

constexpr Color kNodeFill = {0.18f, 0.18f, 0.20f, 1.0f};
constexpr Color kNodeStroke = {0.40f, 0.40f, 0.45f, 1.0f};
constexpr Color kNodeStrokeSelected = {0.95f, 0.85f, 0.20f, 1.0f};
constexpr Color kNodeText = {0.92f, 0.92f, 0.92f, 1.0f};
constexpr Color kGhostColor = {0.7f, 0.7f, 0.7f, 0.6f};

constexpr float kEdgeWidth = 2.0f;
constexpr float kNodeOutlineWidth = 1.5f;

Color edge_color(GraphEditorView::EdgeKind kind) {
    switch (kind) {
    case GraphEditorView::EdgeKind::Midi:
        return kMidiColor;
    case GraphEditorView::EdgeKind::Automation:
        return kAutomationColor;
    case GraphEditorView::EdgeKind::Feedback:
        return kFeedbackColor;
    case GraphEditorView::EdgeKind::Audio:
        break;
    }
    return kAudioColor;
}

GraphEditorView::EdgeKind kind_of(const host::Connection& conn) {
    if (conn.feedback)
        return GraphEditorView::EdgeKind::Feedback;
    if (conn.automation)
        return GraphEditorView::EdgeKind::Automation;
    if (conn.midi)
        return GraphEditorView::EdgeKind::Midi;
    return GraphEditorView::EdgeKind::Audio;
}

float dist2(float ax, float ay, float bx, float by) {
    const float dx = ax - bx;
    const float dy = ay - by;
    return dx * dx + dy * dy;
}

// One cubic Bezier from `from` to `to`, leaving and entering horizontally.
void stroke_edge(::vellum::canvas::Canvas& canvas, ::vellum::view::Point from,
                 ::vellum::view::Point to) {
    const float dx = std::max(40.0f, std::abs(to.x - from.x) * 0.5f);
    canvas.set_line_width(kEdgeWidth);
    canvas.begin_path();
    canvas.move_to(from.x, from.y);
    canvas.cubic_to(from.x + dx, from.y, to.x - dx, to.y, to.x, to.y);
    canvas.stroke_current_path();
}

} // namespace

GraphEditorView::GraphEditorView(host::SignalGraph& graph) : graph_(graph) {
    auto_layout();
}

void GraphEditorView::set_node_position(host::NodeId id, float x, float y) {
    positions_[id] = {x, y};
    request_repaint();
}

void GraphEditorView::auto_layout() {
    // A grid of four columns with its top-left corner at (40, 40).
    int index = 0;
    for (const auto& node : graph_.nodes()) {
        if (positions_.find(node.id) == positions_.end()) {
            const int col = index % 4;
            const int row = index / 4;
            positions_[node.id] = {40.0f + static_cast<float>(col) * (kNodeWidth + 60.0f),
                                   40.0f + static_cast<float>(row) * (kNodeHeight + 60.0f)};
        }
        ++index;
    }
    request_repaint();
}

::vellum::view::Point GraphEditorView::node_position(host::NodeId id) const {
    const auto it = positions_.find(id);
    return it != positions_.end() ? it->second : Pos{0.0f, 0.0f};
}

::vellum::view::Point GraphEditorView::input_port_position(host::NodeId id, int port) const {
    const Pos origin = node_position(id);
    return {origin.x, origin.y + 24.0f + static_cast<float>(port) * kPortSpacing};
}

::vellum::view::Point GraphEditorView::output_port_position(host::NodeId id, int port) const {
    const Pos origin = node_position(id);
    return {origin.x + kNodeWidth, origin.y + 24.0f + static_cast<float>(port) * kPortSpacing};
}

bool GraphEditorView::hit_node(float x, float y, host::NodeId& out_id) const {
    for (const auto& node : graph_.nodes()) {
        const Pos origin = node_position(node.id);
        if (x >= origin.x && x <= origin.x + kNodeWidth && y >= origin.y &&
            y <= origin.y + kNodeHeight) {
            out_id = node.id;
            return true;
        }
    }
    return false;
}

bool GraphEditorView::hit_input_port(float x, float y, host::NodeId& out_id, int& out_port) const {
    const float reach2 = (kPortRadius + 2.0f) * (kPortRadius + 2.0f);
    for (const auto& node : graph_.nodes()) {
        for (int port = 0; port < node.num_input_ports; ++port) {
            const Pos at = input_port_position(node.id, port);
            if (dist2(x, y, at.x, at.y) <= reach2) {
                out_id = node.id;
                out_port = port;
                return true;
            }
        }
    }
    return false;
}

bool GraphEditorView::hit_output_port(float x, float y, host::NodeId& out_id, int& out_port) const {
    const float reach2 = (kPortRadius + 2.0f) * (kPortRadius + 2.0f);
    for (const auto& node : graph_.nodes()) {
        for (int port = 0; port < node.num_output_ports; ++port) {
            const Pos at = output_port_position(node.id, port);
            if (dist2(x, y, at.x, at.y) <= reach2) {
                out_id = node.id;
                out_port = port;
                return true;
            }
        }
    }
    return false;
}

void GraphEditorView::paint_node(::vellum::canvas::Canvas& canvas,
                                 const host::GraphNode& node) const {
    const Pos origin = node_position(node.id);
    canvas.set_fill_color(kNodeFill);
    canvas.fill_rect(origin.x, origin.y, kNodeWidth, kNodeHeight);
    canvas.set_stroke_color(node.id == selected_node_ ? kNodeStrokeSelected : kNodeStroke);
    canvas.set_line_width(kNodeOutlineWidth);
    canvas.stroke_rect(origin.x, origin.y, kNodeWidth, kNodeHeight);

    canvas.set_fill_color(kNodeText);
    canvas.set_font("Inter", 12.0f);
    canvas.fill_text(node.name.empty() ? "(unnamed)" : node.name, origin.x + 8.0f,
                     origin.y + 14.0f);

    canvas.set_fill_color(kAudioColor);
    for (int port = 0; port < node.num_input_ports; ++port) {
        const Pos at = input_port_position(node.id, port);
        canvas.fill_circle(at.x, at.y, kPortRadius);
    }
    for (int port = 0; port < node.num_output_ports; ++port) {
        const Pos at = output_port_position(node.id, port);
        canvas.fill_circle(at.x, at.y, kPortRadius);
    }
}

void GraphEditorView::paint_connection(::vellum::canvas::Canvas& canvas,
                                       const host::Connection& conn) const {
    Pos from = output_port_position(conn.source_node, static_cast<int>(conn.source_port));
    Pos to;
    if (conn.automation) {
        // An automation edge ends on the destination's title bar.
        to = node_position(conn.dest_node);
        to.y += 14.0f;
    } else {
        to = input_port_position(conn.dest_node, static_cast<int>(conn.dest_port));
    }
    canvas.set_stroke_color(edge_color(kind_of(conn)));
    stroke_edge(canvas, from, to);
}

void GraphEditorView::paint_ghost(::vellum::canvas::Canvas& canvas) const {
    if (!dragging_)
        return;
    const Pos origin = drag_src_is_output_ ? output_port_position(drag_src_node_, drag_src_port_)
                                           : input_port_position(drag_src_node_, drag_src_port_);
    canvas.set_stroke_color(kGhostColor);
    stroke_edge(canvas, origin, drag_cursor_);
}

void GraphEditorView::paint(::vellum::canvas::Canvas& canvas) {
    // Connections first so nodes paint over their ends.
    for (const auto& conn : graph_.connections())
        paint_connection(canvas, conn);
    paint_ghost(canvas);
    for (const auto& node : graph_.nodes())
        paint_node(canvas, node);
}

void GraphEditorView::on_mouse_event(const ::vellum::view::MouseEvent& event) {
    last_modifiers_ = event.modifiers;
    if (event.click_count >= 2 && on_node_activated_ &&
        (event.phase == ::vellum::view::MousePhase::press ||
         event.phase == ::vellum::view::MousePhase::automatic)) {
        host::NodeId id = 0;
        if (hit_node(event.position.x, event.position.y, id))
            on_node_activated_(id);
    }
    View::on_mouse_event(event);
}

void GraphEditorView::on_mouse_down(::vellum::view::Point pos) {
    host::NodeId id = 0;
    int port = -1;
    if (hit_output_port(pos.x, pos.y, id, port)) {
        dragging_ = true;
        drag_src_is_output_ = true;
    } else if (hit_input_port(pos.x, pos.y, id, port)) {
        dragging_ = true;
        drag_src_is_output_ = false;
    } else {
        selected_node_ = hit_node(pos.x, pos.y, id) ? id : 0;
        request_repaint();
        return;
    }
    drag_src_node_ = id;
    drag_src_port_ = port;
    drag_cursor_ = pos;
    request_repaint();
}

void GraphEditorView::on_mouse_drag(::vellum::view::Point pos) {
    if (!dragging_)
        return;
    drag_cursor_ = pos;
    request_repaint();
}

void GraphEditorView::on_mouse_up(::vellum::view::Point pos) {
    if (!dragging_)
        return;
    const bool shift = (last_modifiers_ & ::vellum::view::kModShift) != 0;
    const bool alt = (last_modifiers_ & ::vellum::view::kModAlt) != 0;

    // The drag began on one side; the release must land on the other.
    host::NodeId other = 0;
    int other_port = -1;
    const bool landed = drag_src_is_output_ ? hit_input_port(pos.x, pos.y, other, other_port)
                                            : hit_output_port(pos.x, pos.y, other, other_port);
    if (landed) {
        const host::NodeId source = drag_src_is_output_ ? drag_src_node_ : other;
        const int source_port = drag_src_is_output_ ? drag_src_port_ : other_port;
        const host::NodeId dest = drag_src_is_output_ ? other : drag_src_node_;
        const int dest_port = drag_src_is_output_ ? other_port : drag_src_port_;
        if (shift)
            graph_.connect_feedback(source, source_port, dest, dest_port);
        else if (alt)
            graph_.connect_midi(source, dest);
        else
            graph_.connect(source, source_port, dest, dest_port);
    }
    dragging_ = false;
    drag_src_port_ = -1;
    request_repaint();
}

} // namespace pulp::vellum_graph
