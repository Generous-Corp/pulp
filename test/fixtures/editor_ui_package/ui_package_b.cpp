#include "ui_package_b.hpp"

#include "tagged_editor_factory.hpp"

namespace pulp_test_ui_b {

::pulp::format::EditorFactory* editor_factory() {
    static ::pulp::test_fixtures::TaggedEditorFactory factory("b");
    return &factory;
}

} // namespace pulp_test_ui_b
