#include <pulp/view/widgets/graph_editor_view.hpp>
#include <algorithm>
#include <cmath>

namespace pulp::view::widgets {

namespace {

// Color palette per edge kind.
constexpr canvas::Color kAudioColor      = {0.30f, 0.55f, 0.95f, 1.0f}; // blue
constexpr canvas::Color kMidiColor       = {0.30f, 0.85f, 0.45f, 1.0f}; // green
constexpr canvas::Color kAutomationColor = {0.95f, 0.85f, 0.20f, 1.0f}; // yellow
constexpr canvas::Color kFeedbackColor   = {0.95f, 0.40f, 0.40f, 1.0f}; // red

constexpr canvas::Color kNodeFill        = {0.18f, 0.18f, 0.20f, 1.0f};
constexpr canvas::Color kNodeStroke      = {0.40f, 0.40f, 0.45f, 1.0f};
constexpr canvas::Color kNodeStrokeSel   = {0.95f, 0.85f, 0.20f, 1.0f};
constexpr canvas::Color kNodeText        = {0.92f, 0.92f, 0.92f, 1.0f};

inline canvas::Color edge_color(GraphEditorView::EdgeKind k) {
    switch (k) {
        case GraphEditorView::EdgeKind::Midi:       return kMidiColor;
        case GraphEditorView::EdgeKind::Automation: return kAutomationColor;
        case GraphEditorView::EdgeKind::Feedback:   return kFeedbackColor;
        case GraphEditorView::EdgeKind::Audio:
        default:                                    return kAudioColor;
    }
}

inline GraphEditorView::EdgeKind kind_of(const host::Connection& c) {
    if (c.feedback)   return GraphEditorView::EdgeKind::Feedback;
    if (c.automation) return GraphEditorView::EdgeKind::Automation;
    if (c.midi)       return GraphEditorView::EdgeKind::Midi;
    return GraphEditorView::EdgeKind::Audio;
}

inline float dist2(float ax, float ay, float bx, float by) {
    const float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}

} // namespace

void GraphEditorView::auto_layout() {
    // Simple grid: 4 columns, top-left origin at (40, 40).
    const auto& nodes = graph_.nodes();
    int i = 0;
    for (const auto& n : nodes) {
        if (positions_.find(n.id) != positions_.end()) { ++i; continue; }
        const int col = i % 4;
        const int row = i / 4;
        positions_[n.id] = {
            40.0f + col * (kNodeWidth + 60.0f),
            40.0f + row * (kNodeHeight + 60.0f)
        };
        ++i;
    }
}

GraphEditorView::Pos GraphEditorView::node_origin_(host::NodeId id) const {
    auto it = positions_.find(id);
    return it != positions_.end() ? it->second : Pos{0, 0};
}

GraphEditorView::Pos GraphEditorView::input_port_pos_(host::NodeId id, int port) const {
    auto p = node_origin_(id);
    return {p.x, p.y + 24.0f + port * kPortSpacing};
}

GraphEditorView::Pos GraphEditorView::output_port_pos_(host::NodeId id, int port) const {
    auto p = node_origin_(id);
    return {p.x + kNodeWidth, p.y + 24.0f + port * kPortSpacing};
}

bool GraphEditorView::hit_node_(float mx, float my, host::NodeId& out_id) const {
    for (const auto& n : graph_.nodes()) {
        auto p = node_origin_(n.id);
        if (mx >= p.x && mx <= p.x + kNodeWidth
            && my >= p.y && my <= p.y + kNodeHeight) {
            out_id = n.id;
            return true;
        }
    }
    return false;
}

bool GraphEditorView::hit_input_port_(float mx, float my, host::NodeId& out_id, int& out_port) const {
    const float r2 = (kPortRadius + 2.0f) * (kPortRadius + 2.0f);
    for (const auto& n : graph_.nodes()) {
        for (int p = 0; p < n.num_input_ports; ++p) {
            auto pp = input_port_pos_(n.id, p);
            if (dist2(mx, my, pp.x, pp.y) <= r2) {
                out_id = n.id;
                out_port = p;
                return true;
            }
        }
    }
    return false;
}

bool GraphEditorView::hit_output_port_(float mx, float my, host::NodeId& out_id, int& out_port) const {
    const float r2 = (kPortRadius + 2.0f) * (kPortRadius + 2.0f);
    for (const auto& n : graph_.nodes()) {
        for (int p = 0; p < n.num_output_ports; ++p) {
            auto pp = output_port_pos_(n.id, p);
            if (dist2(mx, my, pp.x, pp.y) <= r2) {
                out_id = n.id;
                out_port = p;
                return true;
            }
        }
    }
    return false;
}

void GraphEditorView::paint_node_(canvas::Canvas& c, const host::GraphNode& n) {
    auto p = node_origin_(n.id);
    c.set_fill_color(kNodeFill);
    c.fill_rect(p.x, p.y, kNodeWidth, kNodeHeight);
    c.set_stroke_color(n.id == selected_node_ ? kNodeStrokeSel : kNodeStroke);
    c.stroke_path(nullptr, 0);  // outline drawn via path API in real impl
    // Title
    c.set_fill_color(kNodeText);
    c.fill_text(n.name.empty() ? "(unnamed)" : n.name, p.x + 8, p.y + 14);
    // Ports
    for (int i = 0; i < n.num_input_ports; ++i) {
        auto pp = input_port_pos_(n.id, i);
        c.set_fill_color(kAudioColor);
        c.fill_circle(pp.x, pp.y, kPortRadius);
    }
    for (int i = 0; i < n.num_output_ports; ++i) {
        auto pp = output_port_pos_(n.id, i);
        c.set_fill_color(kAudioColor);
        c.fill_circle(pp.x, pp.y, kPortRadius);
    }
}

void GraphEditorView::paint_connection_(canvas::Canvas& c, const host::Connection& conn) {
    auto kind = kind_of(conn);
    Pos from, to;
    if (conn.automation) {
        from = output_port_pos_(conn.source_node, (int)conn.source_port);
        to   = node_origin_(conn.dest_node);
        to.y += 14.0f;  // connect to title bar area for automation
    } else {
        from = output_port_pos_(conn.source_node, (int)conn.source_port);
        to   = input_port_pos_(conn.dest_node, (int)conn.dest_port);
    }
    const float dx = std::max(40.0f, std::abs(to.x - from.x) * 0.5f);
    c.set_stroke_color(edge_color(kind));
    c.begin_path();
    c.move_to(from.x, from.y);
    c.cubic_to(from.x + dx, from.y, to.x - dx, to.y, to.x, to.y);
    c.stroke_path(nullptr, 0);  // backend strokes the just-built path
}

void GraphEditorView::paint_ghost_(canvas::Canvas& c) {
    if (!dragging_) return;
    Pos origin = drag_src_is_output_
        ? output_port_pos_(drag_src_node_, drag_src_port_)
        : input_port_pos_(drag_src_node_, drag_src_port_);
    const float dx = std::max(40.0f, std::abs(drag_cursor_.x - origin.x) * 0.5f);
    c.set_stroke_color({0.7f, 0.7f, 0.7f, 0.6f});
    c.begin_path();
    c.move_to(origin.x, origin.y);
    c.cubic_to(origin.x + dx, origin.y,
               drag_cursor_.x - dx, drag_cursor_.y,
               drag_cursor_.x, drag_cursor_.y);
    c.stroke_path(nullptr, 0);
}

void GraphEditorView::paint(canvas::Canvas& c) {
    // Connections first so nodes paint over them.
    for (const auto& conn : graph_.connections()) {
        paint_connection_(c, conn);
    }
    paint_ghost_(c);
    for (const auto& n : graph_.nodes()) {
        paint_node_(c, n);
    }
}

void GraphEditorView::on_mouse_event(const MouseEvent& ev) {
    // Capture modifier state for on_mouse_up; the legacy callbacks only
    // give us a Point. The base class dispatches through this overload
    // before the per-phase callbacks.
    last_modifiers_ = ev.modifiers;
    View::on_mouse_event(ev);
}

void GraphEditorView::on_mouse_down(Point pos) {
    const float mx = pos.x, my = pos.y;
    host::NodeId nid = 0;
    int port = -1;
    if (hit_output_port_(mx, my, nid, port)) {
        dragging_ = true;
        drag_src_node_ = nid;
        drag_src_port_ = port;
        drag_src_is_output_ = true;
        drag_cursor_ = {mx, my};
    } else if (hit_input_port_(mx, my, nid, port)) {
        dragging_ = true;
        drag_src_node_ = nid;
        drag_src_port_ = port;
        drag_src_is_output_ = false;
        drag_cursor_ = {mx, my};
    } else if (hit_node_(mx, my, nid)) {
        selected_node_ = nid;
    } else {
        selected_node_ = 0;
    }
}

void GraphEditorView::on_mouse_drag(Point pos) {
    if (!dragging_) return;
    drag_cursor_ = {pos.x, pos.y};
}

void GraphEditorView::on_mouse_up(Point pos) {
    if (!dragging_) return;
    const float mx = pos.x, my = pos.y;
    const bool shift = (last_modifiers_ & kModShift) != 0;
    const bool alt   = (last_modifiers_ & kModAlt)   != 0;

    host::NodeId other_id = 0;
    int other_port = -1;
    if (drag_src_is_output_) {
        if (hit_input_port_(mx, my, other_id, other_port)) {
            if (shift) {
                graph_.connect_feedback(drag_src_node_, drag_src_port_,
                                        other_id, other_port);
            } else if (alt) {
                graph_.connect_midi(drag_src_node_, other_id);
            } else {
                graph_.connect(drag_src_node_, drag_src_port_,
                               other_id, other_port);
            }
        }
    } else {
        if (hit_output_port_(mx, my, other_id, other_port)) {
            if (shift) {
                graph_.connect_feedback(other_id, other_port,
                                        drag_src_node_, drag_src_port_);
            } else if (alt) {
                graph_.connect_midi(other_id, drag_src_node_);
            } else {
                graph_.connect(other_id, other_port,
                               drag_src_node_, drag_src_port_);
            }
        }
    }
    dragging_ = false;
    drag_src_port_ = -1;
}

} // namespace pulp::view::widgets
