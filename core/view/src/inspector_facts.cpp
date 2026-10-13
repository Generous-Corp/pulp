#include <pulp/view/inspector_facts.hpp>
#include <pulp/view/widgets.hpp>

namespace pulp::view {

// The key function: InspectorFacts' typeinfo and vtable live in view-core, so
// view-core's diagnostics and the Pulp inspector can cross-cast to it without
// linking any implementing widget.
InspectorFacts::~InspectorFacts() = default;

std::string view_type_name(const View& view) {
    if (dynamic_cast<const Knob*>(&view))
        return "Knob";
    if (dynamic_cast<const Fader*>(&view))
        return "Fader";
    if (dynamic_cast<const Toggle*>(&view))
        return "Toggle";
    if (dynamic_cast<const Label*>(&view))
        return "Label";
    if (dynamic_cast<const XYPad*>(&view))
        return "XYPad";
    if (auto* described = dynamic_cast<const InspectorFacts*>(&view))
        return std::string(described->inspector_type_name());
    return "View";
}

} // namespace pulp::view
