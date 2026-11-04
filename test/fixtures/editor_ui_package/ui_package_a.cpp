#include "ui_package_a.hpp"

#include "tagged_editor_factory.hpp"

namespace pulp_test_ui_a {

::pulp::format::EditorFactory* editor_factory() {
    static ::pulp::test_fixtures::TaggedEditorFactory factory("a");
    return &factory;
}

} // namespace pulp_test_ui_a
