#pragma once

// UI package "a": PULP_EDITOR_FACTORY_FUNCTION names editor_factory() below.

namespace pulp::format {
class EditorFactory;
}

namespace pulp_test_ui_a {

::pulp::format::EditorFactory* editor_factory();

} // namespace pulp_test_ui_a
