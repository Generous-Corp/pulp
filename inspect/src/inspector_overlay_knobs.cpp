// Semantic-knob controls + send-to-agent text field for the inspector overlay.
//
// The tweaks panel's top region renders a small set of expressive controls: a
// knob is one control whose value drives a bundle of parameter writes at once (a
// "minimalism" slider, a theme flip, a layout-variant switch — see
// pulp::design::design_knobs). Choosing a knob value emits (knob_id, value)
// through knob_apply_sink_; a free-text field emits a request through
// agent_request_sink_. The overlay never touches files — the host owns the
// artifact/manifest and applies the change — so this layer is pure UI + hit
// resolution and is testable by driving the public input entry points headless.

#include "inspector_overlay_internal.hpp"

#include <pulp/inspect/inspector_overlay.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace pulp::inspect {

namespace {

// The value string a click on `pos` emits — the same string the design core's
// select_position() resolves back to this position. A Slider matches by numeric
// anchor, so it emits `at`; an Enum/Toggle matches by label.
std::string position_value(const pulp::design::KnobSpec& knob,
                           const pulp::design::KnobPosition& pos) {
    if (knob.kind == pulp::design::KnobKind::Slider) {
        std::ostringstream oss;
        // Whole anchors print without a trailing ".0" so the label reads clean;
        // fractional anchors keep two places. Either form parses as the number
        // select_position() compares against.
        if (std::abs(pos.at - std::round(pos.at)) < 1e-9)
            oss << static_cast<long long>(std::llround(pos.at));
        else
            oss << pos.at;
        return oss.str();
    }
    return pos.label;
}

}  // namespace

std::string InspectorOverlay::knob_selection(std::string_view knob_id) const {
    auto it = knob_selection_.find(std::string(knob_id));
    return it == knob_selection_.end() ? std::string{} : it->second;
}

float InspectorOverlay::paint_knob_controls(Canvas& canvas, float x, float y,
                                            float w, float h) {
    knob_hits_.clear();
    agent_field_bounds_ = Rect{};
    agent_send_bounds_ = Rect{};

    const bool have_knobs = !knob_schema_.knobs.empty();
    const bool have_agent = static_cast<bool>(agent_request_sink_);
    if (!have_knobs && !have_agent) return 0.0f;

    canvas.set_font("monospace", kFontSize);

    constexpr float kSegH = 16.0f;      // segment / field height
    constexpr float kSegPadX = 6.0f;    // text inset inside a segment
    constexpr float kSegGap = 4.0f;     // gap between segments
    constexpr float kLineH = 15.0f;     // label line advance
    const float bottom = y + h;

    float cy = y + 2.0f;

    // ── Knobs ────────────────────────────────────────────────────────
    for (const auto& knob : knob_schema_.knobs) {
        if (cy + kLineH + kSegH > bottom) break;  // out of room — stop cleanly

        // Knob label.
        canvas.set_fill_color(kPanelDim);
        canvas.fill_text(knob.label.empty() ? knob.id : knob.label, x, cy + 11);
        cy += kLineH;

        // Resolve the current selection (default: the first position's value).
        std::string current = knob_selection(knob.id);
        if (current.empty() && !knob.positions.empty())
            current = position_value(knob, knob.positions.front());

        // Segments, wrapping to a new line when they run past the right edge.
        float sx = x;
        for (const auto& pos : knob.positions) {
            const std::string label = pos.label.empty() ? position_value(knob, pos)
                                                         : pos.label;
            const float tw = canvas.measure_text(label);
            const float seg_w = tw + 2.0f * kSegPadX;
            if (sx + seg_w > x + w && sx > x) {  // wrap
                sx = x;
                cy += kSegH + kSegGap;
                if (cy + kSegH > bottom) break;
            }
            const std::string value = position_value(knob, pos);
            const bool selected = (value == current);

            Rect seg{sx, cy, seg_w, kSegH};
            canvas.set_fill_color(selected ? kSelectedFill : kPanelHighlight);
            canvas.fill_rounded_rect(seg.x, seg.y, seg.width, seg.height, 3.0f);
            canvas.set_stroke_color(selected ? kSelectedStroke : kPanelDim);
            canvas.stroke_rounded_rect(seg.x, seg.y, seg.width, seg.height, 3.0f);
            canvas.set_fill_color(selected ? kPanelText : kPanelDim);
            canvas.fill_text(label, sx + kSegPadX, cy + 12);

            knob_hits_.push_back({knob.id, value, seg});
            sx += seg_w + kSegGap;
        }
        cy += kSegH + kSegGap + 4.0f;
    }

    // ── Send-to-agent field ──────────────────────────────────────────
    if (have_agent && cy + kLineH + kSegH <= bottom) {
        canvas.set_fill_color(kPanelDim);
        canvas.fill_text("Ask agent", x, cy + 11);
        cy += kLineH;

        // "Send" button on the right; the field fills the remaining width.
        const std::string send_label = "Send";
        const float send_w = canvas.measure_text(send_label) + 2.0f * kSegPadX;
        const float send_x = x + w - send_w;
        const float field_w = std::max(0.0f, send_x - kSegGap - x);

        agent_field_bounds_ = Rect{x, cy, field_w, kSegH};
        canvas.set_fill_color(agent_field_active_ ? kFieldEditBg : kPanelHighlight);
        canvas.fill_rounded_rect(x, cy, field_w, kSegH, 3.0f);
        canvas.set_stroke_color(agent_field_active_ ? kSelectedStroke : kPanelDim);
        canvas.stroke_rounded_rect(x, cy, field_w, kSegH, 3.0f);

        if (agent_text_buffer_.empty() && !agent_field_active_) {
            canvas.set_fill_color(kPanelDim);
            canvas.fill_text("Describe a change\xe2\x80\xa6", x + kSegPadX, cy + 12);
        } else {
            canvas.set_fill_color(kPanelText);
            canvas.fill_text(agent_text_buffer_, x + kSegPadX, cy + 12);
            if (agent_field_active_) {
                const float caret_x =
                    x + kSegPadX +
                    canvas.measure_text(agent_text_buffer_.substr(0, agent_caret_));
                canvas.set_stroke_color(kFieldEditCaret);
                canvas.stroke_line(caret_x, cy + 2, caret_x, cy + kSegH - 2);
            }
        }

        agent_send_bounds_ = Rect{send_x, cy, send_w, kSegH};
        const bool sendable = !agent_text_buffer_.empty();
        canvas.set_fill_color(sendable ? kHighlightFill : kPanelHighlight);
        canvas.fill_rounded_rect(send_x, cy, send_w, kSegH, 3.0f);
        canvas.set_stroke_color(sendable ? kHighlightStroke : kPanelDim);
        canvas.stroke_rounded_rect(send_x, cy, send_w, kSegH, 3.0f);
        canvas.set_fill_color(sendable ? kPanelText : kPanelDim);
        canvas.fill_text(send_label, send_x + kSegPadX, cy + 12);

        cy += kSegH + kSegGap;
    }

    return std::min(cy - y, h);
}

bool InspectorOverlay::handle_knob_panel_click(Point p) {
    auto in = [&](const Rect& r) {
        return r.width > 0.0f && r.height > 0.0f && p.x >= r.x && p.x <= r.x + r.width &&
               p.y >= r.y && p.y <= r.y + r.height;
    };

    for (const auto& hit : knob_hits_) {
        if (in(hit.bounds)) {
            knob_selection_[hit.knob_id] = hit.value;
            if (knob_apply_sink_) knob_apply_sink_(KnobApplyEdit{hit.knob_id, hit.value});
            return true;
        }
    }

    if (in(agent_send_bounds_)) {
        submit_agent_request();
        return true;
    }
    if (in(agent_field_bounds_)) {
        agent_field_active_ = true;
        return true;
    }

    // A click anywhere else in the panel defocuses the text field.
    agent_field_active_ = false;
    return false;
}

void InspectorOverlay::submit_agent_request() {
    if (!agent_text_buffer_.empty() && agent_request_sink_)
        agent_request_sink_(AgentRequestEdit{agent_text_buffer_});
    agent_text_buffer_.clear();
    agent_caret_ = 0;
    agent_field_active_ = false;
}

}  // namespace pulp::inspect
