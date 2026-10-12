#include <pulp/view/inspector_facts.hpp>

namespace pulp::view {

// The key function: InspectorFacts' typeinfo and vtable live in view-core, so
// the inspector can cross-cast to it without linking any implementing widget.
InspectorFacts::~InspectorFacts() = default;

} // namespace pulp::view
