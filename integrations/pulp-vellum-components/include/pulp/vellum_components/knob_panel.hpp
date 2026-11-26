#pragma once

/// @file knob_panel.hpp
/// The inspector's semantic knob panel and the request to send an agent: what
/// a panel of knob controls lays out to, and the payload a free-text request
/// becomes.
///
/// A knob is one expressive control (a "minimalism" slider, a theme flip, a
/// layout switch) whose value stands for a bundle of parameter writes. These
/// types carry only what the panel shows and emits (an id, a label, the
/// positions and the value each one emits), so the bundle of writes stays with
/// whatever applies it. Everything here is pure: `layout_knob_panel` takes a
/// text measurer instead of a canvas, so a test or another surface can place
/// the controls without painting.

#include <pulp/vellum_components/aliases.hpp>

#include <vellum/view/geometry.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace pulp::vellum_components::overlay {

using ::vellum::view::Rect;

/// How a knob's value picks its position: a slider by number, a toggle or an
/// enum by label.
enum class KnobKind { slider, toggle, enumeration };

/// One selectable position. A slider's `at` is its numeric anchor.
struct KnobPosition {
    std::string label;
    double at = 0.0;
};

struct KnobSpec {
    std::string id;
    std::string label; ///< shown above the control; the id when empty
    KnobKind kind = KnobKind::enumeration;
    std::vector<KnobPosition> positions;
};

struct KnobSchema {
    std::vector<KnobSpec> knobs;
};

/// The value choosing `position` emits: a slider's anchor as a number (whole
/// anchors without a fraction, so 1.0 reads "1"), anything else its label.
std::string knob_position_value(const KnobSpec& knob, const KnobPosition& position);

/// One clickable position as laid out.
struct KnobSegment {
    std::string knob_id;
    std::string value; ///< what choosing it emits
    std::string label; ///< what it shows
    Rect bounds;       ///< panel space
    bool selected = false;
};

/// A knob's title line.
struct KnobTitle {
    std::string text;
    float y = 0.0f; ///< top of the line, panel space
};

/// A laid-out panel in its own space, origin at its top-left.
struct KnobPanelLayout {
    std::vector<KnobTitle> titles;
    std::vector<KnobSegment> segments;
    bool has_agent = false;
    float agent_title_y = 0.0f;
    Rect agent_field; ///< empty (zero width) without an agent field
    Rect agent_send;
    float height = 0.0f;
};

using TextMeasure = std::function<float(const std::string&)>;

/// Lay out `schema` in a panel `width` wide and at most `max_height` tall. Each
/// knob is a title line over a row of segments that wraps at the right edge; a
/// segment is selected when its value equals the knob's entry in `selection`
/// (the first position when there is none). With `have_agent` an "Ask agent"
/// field and a Send button follow the knobs. Whatever does not fit in
/// `max_height` is left out, whole knobs first.
KnobPanelLayout layout_knob_panel(const KnobSchema& schema,
                                  const std::map<std::string, std::string>& selection,
                                  bool have_agent, float width, float max_height,
                                  const TextMeasure& measure);

/// What an agent needs to act on a request without guessing which design,
/// screen or state it means.
struct AgentRequestContext {
    std::string design;          ///< design or asset name
    std::string screen;          ///< screen within the design, if any
    std::string editmode_state;  ///< the current edit-mode block, as JSON
    std::string screenshot_path; ///< a captured screenshot, if any
};

/// A free-text request with its context, ready to queue for an agent. The
/// fields and their meaning match the agent request queue's record, so a host
/// maps it one to one.
struct AgentRequestPayload {
    std::string text;
    std::string design;
    std::string screen;
    std::string editmode_state;
    std::string screenshot_path;
    std::string created_at; ///< the host's timestamp; nothing here keeps a clock
};

/// The payload for `text` in `context`, stamped `created_at`.
AgentRequestPayload build_agent_request(std::string text, const AgentRequestContext& context,
                                        std::string created_at);

} // namespace pulp::vellum_components::overlay
