#pragma once

// A Vellum editor root whose lifecycle hooks are defined out of line, in a
// library the plug-in's audio code links. Like a real editor, the code behind
// these symbols lives with the plug-in rather than in the generated editor
// package, so a target that holds only the package's source cannot link.

#include <vellum/view/view.hpp>

namespace pulp::vellum {
struct EditorEnv;
}

namespace pulp::test_fixtures {

class Auv3EditorView final : public ::vellum::view::View {
  public:
    explicit Auv3EditorView(pulp::vellum::EditorEnv& env);

    void editor_opened();
    void editor_idle();
    void editor_closing();
    void layout_children() override;
};

} // namespace pulp::test_fixtures
