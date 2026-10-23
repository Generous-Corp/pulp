#include <pulp/format/editor_ui_embed.hpp>

namespace pulp::format {

namespace {
// Constant-initialized, so a generated registration unit's static initializer
// can run before anything in this translation unit without an ordering hazard.
const EmbeddedEditorUi* g_embedded_editor_ui = nullptr;
} // namespace

void register_embedded_editor_ui(const EmbeddedEditorUi* ui) noexcept {
    if (ui == nullptr) {
        g_embedded_editor_ui = nullptr;
        return;
    }
    if (g_embedded_editor_ui == nullptr || ui->priority >= g_embedded_editor_ui->priority)
        g_embedded_editor_ui = ui;
}

const EmbeddedEditorUi* embedded_editor_ui() noexcept {
    return g_embedded_editor_ui;
}

} // namespace pulp::format
