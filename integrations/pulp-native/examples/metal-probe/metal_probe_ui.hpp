#pragma once

// The metal-probe UI package. PULP_EDITOR_FACTORY_FUNCTION names
// editor_factory() below.

namespace pulp::format {
class EditorFactory;
}

namespace pulp_native_metal_probe {

::pulp::format::EditorFactory* editor_factory();

} // namespace pulp_native_metal_probe
