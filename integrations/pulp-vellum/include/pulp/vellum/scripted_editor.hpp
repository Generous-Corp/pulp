#pragma once

namespace pulp::format {
class EditorFactory;
}

namespace pulp::vellum {

/// Factory for a plug-in's declared UI_SCRIPT and its embedded assets.
format::EditorFactory* scripted_editor_factory();

} // namespace pulp::vellum
