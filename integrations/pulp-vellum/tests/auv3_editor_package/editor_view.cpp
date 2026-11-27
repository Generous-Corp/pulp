#include "editor_view.hpp"

namespace pulp::test_fixtures {

Auv3EditorView::Auv3EditorView(pulp::vellum::EditorEnv&) {}

void Auv3EditorView::editor_opened() {}
void Auv3EditorView::editor_idle() {}
void Auv3EditorView::editor_closing() {}
void Auv3EditorView::layout_children() {
    ::vellum::view::View::layout_children();
}

} // namespace pulp::test_fixtures
