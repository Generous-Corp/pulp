// The headless binary's editor hook: no editor.

#include "gain_meter_processor.hpp"

namespace pulp::slice {

format::EditorFactory* slice_editor_factory() {
    return nullptr;
}

int slice_editor_live_instances() {
    return 0;
}

} // namespace pulp::slice
