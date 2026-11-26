#include <pulp/vellum_components/inspector_overlay.hpp>

#include <pulp/vellum_components/knob_panel.hpp>
#include <pulp/vellum_components/overlay_palette.hpp>

#include <vellum/view/input_events.hpp>

#include <algorithm>
#include <utility>

namespace pulp::vellum_components {

namespace {

bool contains(const overlay::Rect& r, ::vellum::view::Point p) {
    return r.width > 0.0f && r.height > 0.0f && p.x >= r.x && p.x <= r.x + r.width && p.y >= r.y &&
           p.y <= r.y + r.height;
}

overlay::Rect shifted(const overlay::Rect& r, float dx, float dy) {
    return {r.x + dx, r.y + dy, r.width, r.height};
}

} // namespace

std::string InspectorOverlay::knob_selection(std::string_view knob_id) const {
    const auto it = knob_selection_.find(std::string(knob_id));
    return it == knob_selection_.end() ? std::string{} : it->second;
}

void InspectorOverlay::set_agent_request_handoff(
    AgentRequestHandoff handoff, std::function<overlay::AgentRequestContext()> context,
    std::function<std::string()> now) {
    if (!handoff) {
        agent_request_sink_ = {};
        return;
    }
    agent_request_sink_ = [handoff = std::move(handoff), context = std::move(context),
                           now = std::move(now)](const AgentRequestEdit& edit) {
        handoff(overlay::build_agent_request(edit.text,
                                             context ? context() : overlay::AgentRequestContext{},
                                             now ? now() : std::string{}));
    };
}

void InspectorOverlay::submit_agent_request() {
    if (!agent_edit_.text().empty() && agent_request_sink_)
        agent_request_sink_(AgentRequestEdit{agent_edit_.text()});
    agent_edit_.clear();
    agent_field_active_ = false;
}

bool InspectorOverlay::handle_agent_key(const ::vellum::view::KeyEvent& event) {
    using ::vellum::view::KeyCode;
    switch (event.key) {
    case KeyCode::enter:
        submit_agent_request();
        return true;
    case KeyCode::escape:
        // Defocus and keep the draft.
        agent_field_active_ = false;
        return true;
    case KeyCode::backspace:
        agent_edit_.delete_backward();
        return true;
    case KeyCode::left:
        agent_edit_.move_caret(-1, false);
        return true;
    case KeyCode::right:
        agent_edit_.move_caret(1, false);
        return true;
    default:
        // Character input arrives through handle_text_input.
        return false;
    }
}

bool InspectorOverlay::mouse_knob_panel(const ::vellum::view::MouseEvent& event, bool is_press) {
    if (!knob_panel_visible_ || !is_press || !contains(knob_panel_bounds_, event.position))
        return false;
    const auto p = event.position;
    for (const auto& hit : knob_hits_) {
        if (!contains(hit.bounds, p))
            continue;
        knob_selection_[hit.knob_id] = hit.value;
        if (knob_apply_sink_)
            knob_apply_sink_(KnobApplyEdit{hit.knob_id, hit.value});
        return true;
    }
    if (contains(agent_send_bounds_, p)) {
        submit_agent_request();
        return true;
    }
    if (contains(agent_field_bounds_, p)) {
        // The field takes the keyboard, so an open edit ends first.
        commit_text_edit();
        commit_field_edit();
        agent_field_active_ = true;
        return true;
    }
    // The panel's own background: it takes the press so nothing selects behind
    // it, and a press that is not on the field takes the keyboard away from it.
    agent_field_active_ = false;
    return true;
}

void InspectorOverlay::paint_knob_panel(canvas::Canvas& canvas) {
    knob_hits_.clear();
    agent_field_bounds_ = {};
    agent_send_bounds_ = {};
    knob_panel_bounds_ = {};
    if (!knob_panel_visible_)
        return;

    canvas.set_font("monospace", kFontSize);
    const bool have_agent = static_cast<bool>(agent_request_sink_);
    const float inner_w = kPanelWidth - 2 * kPanelPadding;
    const float max_h =
        std::max(0.0f, root_.bounds().height - 2 * kPanelMargin - 2 * kPanelPadding);
    const auto layout = overlay::layout_knob_panel(
        knob_schema_, knob_selection_, have_agent, inner_w, max_h,
        [&](const std::string& text) { return canvas.measure_text(text); });
    if (layout.titles.empty() && !layout.has_agent)
        return;

    // Docked to the bottom right, clear of the viewers at the top right and the
    // loupe at the bottom left.
    const float h = layout.height + 2 * kPanelPadding;
    const float x = root_.bounds().width - kPanelWidth - kPanelMargin;
    const float y = root_.bounds().height - h - kPanelMargin;
    const float ox = x + kPanelPadding;
    const float oy = y + kPanelPadding;
    knob_panel_bounds_ = {x, y, kPanelWidth, h};

    canvas.save();
    canvas.set_fill_color(overlay::kPanelBackground);
    canvas.fill_rounded_rect(x, y, kPanelWidth, h, 4.0f);

    constexpr float kSegmentText = 12.0f;
    for (const auto& title : layout.titles) {
        canvas.set_fill_color(overlay::kPanelDim);
        canvas.fill_text(title.text, ox, oy + title.y + 11);
    }
    for (const auto& seg : layout.segments) {
        const auto b = shifted(seg.bounds, ox, oy);
        canvas.set_fill_color(seg.selected ? overlay::kSelectedFill : overlay::kPanelHighlight);
        canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, 3.0f);
        canvas.set_stroke_color(seg.selected ? overlay::kSelectedStroke : overlay::kPanelDim);
        canvas.stroke_rounded_rect(b.x, b.y, b.width, b.height, 3.0f);
        canvas.set_fill_color(seg.selected ? overlay::kPanelText : overlay::kPanelDim);
        canvas.fill_text(seg.label, b.x + 6.0f, b.y + kSegmentText);
        knob_hits_.push_back({seg.knob_id, seg.value, b});
    }

    if (layout.has_agent) {
        canvas.set_fill_color(overlay::kPanelDim);
        canvas.fill_text("Ask agent", ox, oy + layout.agent_title_y + 11);

        const auto field = shifted(layout.agent_field, ox, oy);
        agent_field_bounds_ = field;
        canvas.set_fill_color(agent_field_active_ ? overlay::kFieldFocusFill
                                                  : overlay::kPanelHighlight);
        canvas.fill_rounded_rect(field.x, field.y, field.width, field.height, 3.0f);
        canvas.set_stroke_color(agent_field_active_ ? overlay::kSelectedStroke
                                                    : overlay::kPanelDim);
        canvas.stroke_rounded_rect(field.x, field.y, field.width, field.height, 3.0f);
        const std::string& draft = agent_edit_.text();
        if (draft.empty() && !agent_field_active_) {
            canvas.set_fill_color(overlay::kPanelDim);
            canvas.fill_text("Describe a change\xe2\x80\xa6", field.x + 6.0f,
                             field.y + kSegmentText);
        } else {
            canvas.set_fill_color(overlay::kPanelText);
            canvas.fill_text(draft, field.x + 6.0f, field.y + kSegmentText);
            if (agent_field_active_) {
                const float caret_x =
                    field.x + 6.0f + canvas.measure_text(draft.substr(0, agent_edit_.caret()));
                canvas.set_stroke_color(overlay::kFieldCaret);
                canvas.stroke_line(caret_x, field.y + 2.0f, caret_x, field.y + field.height - 2.0f);
            }
        }

        const auto send = shifted(layout.agent_send, ox, oy);
        agent_send_bounds_ = send;
        const bool sendable = !draft.empty();
        canvas.set_fill_color(sendable ? overlay::kHighlightFill : overlay::kPanelHighlight);
        canvas.fill_rounded_rect(send.x, send.y, send.width, send.height, 3.0f);
        canvas.set_stroke_color(sendable ? overlay::kHighlightStroke : overlay::kPanelDim);
        canvas.stroke_rounded_rect(send.x, send.y, send.width, send.height, 3.0f);
        canvas.set_fill_color(sendable ? overlay::kPanelText : overlay::kPanelDim);
        canvas.fill_text("Send", send.x + 6.0f, send.y + kSegmentText);
    }
    canvas.restore();
}

} // namespace pulp::vellum_components
