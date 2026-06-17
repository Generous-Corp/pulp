// widget_bridge/runtime_import_api_stub.cpp — no-op replacement for
// WidgetBridge::install_runtime_import_handlers() when the design-import
// authoring subsystem is gated out (PULP_ENABLE_DESIGN_IMPORT=OFF).
//
// The real implementation (runtime_import_api.cpp) registers the
// __pulpRuntimeImport__ / __pulpRuntimeSettle__ JS handlers and pulls in the
// design-import parsers. In a stripped/shipped build those parsers are not
// compiled, so this stub provides the symbol (callers — examples/tools — still
// link) while doing nothing: a plugin built without design-import simply has no
// runtime design-import surface. Exactly one of {real, stub} is compiled per
// config (CMake if/else), so there is no ODR conflict.

#include <pulp/view/widget_bridge.hpp>

namespace pulp::view {

void WidgetBridge::install_runtime_import_handlers() {
    // Intentionally empty: runtime design import is unavailable when
    // PULP_ENABLE_DESIGN_IMPORT is OFF. runtime_import_installed_ stays false.
}

}  // namespace pulp::view
