#pragma once

// UI package "b": PULP_EDITOR_FACTORY_FUNCTION names editor_factory() below.

namespace pulp::format {
class EditorFactory;
}

namespace pulp_test_ui_b {

::pulp::format::EditorFactory* editor_factory();

} // namespace pulp_test_ui_b
