#pragma once

#include <cstddef>

// View-free on purpose: the generated registration translation unit that
// pulp_add_plugin(UI_SCRIPT) compiles into every format target includes only
// this header, and the fixture that proves the embedding builds it without the
// rest of the SDK.

namespace pulp::format {

/// A scripted editor compiled into the plug-in image: the UI script bytes and,
/// when the build found one, the theme override beside it. Registered from a
/// generated translation unit's static initializer, so the bytes are present in
/// whichever format binary the plug-in ships as and the editor never reads the
/// developer's source tree at run time.
struct EmbeddedEditorUi {
    /// File name only (`ui.js`), for diagnostics and the script's own
    /// `hostKind()`-style introspection. Never a directory.
    const char* script_name;
    const unsigned char* script;
    std::size_t script_size;
    /// Theme override JSON, or nullptr/0 when the build embedded none.
    const unsigned char* theme;
    std::size_t theme_size;
    /// Higher wins when more than one registration is linked. pulp_add_plugin's
    /// UI_SCRIPT registers at 0 and pulp_use_kit_ui at 1, matching the order in
    /// which the two helpers apply the definitions they replace.
    int priority;
};

/// Install `ui` as the image's embedded editor, keeping an already registered
/// entry of higher priority. nullptr clears the registration, for harnesses
/// that prove the AutoUi fallback in the same binary.
void register_embedded_editor_ui(const EmbeddedEditorUi* ui) noexcept;

/// The registered embedded editor, or nullptr when the image carries none.
const EmbeddedEditorUi* embedded_editor_ui() noexcept;

/// Static-initializer hook for the generated registration unit.
struct EmbeddedEditorUiRegistration {
    explicit EmbeddedEditorUiRegistration(const EmbeddedEditorUi* ui) noexcept {
        register_embedded_editor_ui(ui);
    }
};

} // namespace pulp::format
