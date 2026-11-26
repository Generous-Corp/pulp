#include <pulp/vellum_components/knob_panel.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace pulp::vellum_components::overlay {

namespace {

constexpr float kSegmentHeight = 16.0f;
constexpr float kSegmentPadX = 6.0f;
constexpr float kSegmentGap = 4.0f;
constexpr float kLine = 15.0f;
constexpr float kTopInset = 2.0f;

} // namespace

std::string knob_position_value(const KnobSpec& knob, const KnobPosition& position) {
    if (knob.kind != KnobKind::slider)
        return position.label;
    std::ostringstream out;
    if (std::abs(position.at - std::round(position.at)) < 1e-9)
        out << static_cast<long long>(std::llround(position.at));
    else
        out << position.at;
    return out.str();
}

KnobPanelLayout layout_knob_panel(const KnobSchema& schema,
                                  const std::map<std::string, std::string>& selection,
                                  bool have_agent, float width, float max_height,
                                  const TextMeasure& measure) {
    KnobPanelLayout out;
    if (schema.knobs.empty() && !have_agent)
        return out;

    float cy = kTopInset;
    for (const auto& knob : schema.knobs) {
        // A knob needs its title and at least one row of segments.
        if (cy + kLine + kSegmentHeight > max_height)
            break;
        out.titles.push_back({knob.label.empty() ? knob.id : knob.label, cy});
        cy += kLine;

        const auto chosen = selection.find(knob.id);
        std::string current = chosen == selection.end() ? std::string{} : chosen->second;
        if (current.empty() && !knob.positions.empty())
            current = knob_position_value(knob, knob.positions.front());

        float sx = 0.0f;
        for (const auto& position : knob.positions) {
            const std::string value = knob_position_value(knob, position);
            const std::string label = position.label.empty() ? value : position.label;
            const float seg_w = measure(label) + 2.0f * kSegmentPadX;
            if (sx + seg_w > width && sx > 0.0f) { // wrap to a new row
                sx = 0.0f;
                cy += kSegmentHeight + kSegmentGap;
                if (cy + kSegmentHeight > max_height)
                    break;
            }
            out.segments.push_back(
                {knob.id, value, label, Rect{sx, cy, seg_w, kSegmentHeight}, value == current});
            sx += seg_w + kSegmentGap;
        }
        cy += kSegmentHeight + kSegmentGap + 4.0f;
    }

    if (have_agent && cy + kLine + kSegmentHeight <= max_height) {
        out.has_agent = true;
        out.agent_title_y = cy;
        cy += kLine;
        const float send_w = measure("Send") + 2.0f * kSegmentPadX;
        const float send_x = width - send_w;
        out.agent_field = Rect{0.0f, cy, std::max(0.0f, send_x - kSegmentGap), kSegmentHeight};
        out.agent_send = Rect{send_x, cy, send_w, kSegmentHeight};
        cy += kSegmentHeight + kSegmentGap;
    }
    out.height = std::min(cy, max_height);
    return out;
}

AgentRequestPayload build_agent_request(std::string text, const AgentRequestContext& context,
                                        std::string created_at) {
    AgentRequestPayload payload;
    payload.text = std::move(text);
    payload.design = context.design;
    payload.screen = context.screen;
    payload.editmode_state = context.editmode_state;
    payload.screenshot_path = context.screenshot_path;
    payload.created_at = std::move(created_at);
    return payload;
}

} // namespace pulp::vellum_components::overlay
