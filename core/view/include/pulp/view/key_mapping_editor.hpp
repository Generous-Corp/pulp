#pragma once

/// @file key_mapping_editor.hpp
/// KeyMappingEditor — Pulp-native UI for rebinding command shortcuts.
///
/// Pulp-native naming per planning/2026-05-24-macos-plugin-authoring-plan.md
/// section 6.4. The editor is a `View` that lists every command registered
/// with a `CommandRegistry`, shows each command's current chord, and lets
/// the user pick a row + press a new chord to rebind. Changes are committed
/// to the registry's `ShortcutMap` and persisted through
/// `pulp::state::ApplicationProperties::user_settings()` (or any caller-
/// supplied `PropertiesFile`).
///
/// The widget is deliberately small — it relies on the parent shell to
/// scroll it. Rows use `Theme` tokens (`background.surface`,
/// `text.primary`, `border.subtle`, `accent.primary`) where available and
/// fall back to a neutral palette otherwise.

#include <pulp/view/command_registry.hpp>
#include <pulp/view/view.hpp>

#include <functional>
#include <string>

namespace pulp::state { class PropertiesFile; }

namespace pulp::view {

class KeyMappingEditor : public View {
public:
    /// Construct an editor bound to a `CommandRegistry`. The registry must
    /// outlive the editor.
    explicit KeyMappingEditor(CommandRegistry& registry);

    /// Optional: a `PropertiesFile` to persist rebindings to. Each commit
    /// calls `ShortcutMap::save_to(props)` and (if `auto_save_path` is
    /// non-empty) `props.save(auto_save_path)`. Without a file the editor
    /// just mutates the in-memory ShortcutMap.
    void set_persistence(pulp::state::PropertiesFile* props,
                         std::string auto_save_path = {});

    /// Optional: prefix to use inside the PropertiesFile (defaults to
    /// "shortcuts"). Lets multiple editors share one settings file
    /// without colliding.
    void set_persistence_prefix(std::string prefix);

    // ── Selection / capture state ───────────────────────────────────

    /// Select a row (by command id) and begin chord capture. Subsequent
    /// `on_key_event` calls will commit the next non-modifier-only key
    /// as the new binding for `id`.
    void begin_capture(CommandID id);

    /// Cancel any in-progress chord capture.
    void cancel_capture();

    /// True when waiting for the user to press a chord.
    bool is_capturing() const { return capture_id_ != kInvalidCommandID; }

    /// The command id currently being captured (kInvalidCommandID when
    /// not capturing).
    CommandID capturing_command() const { return capture_id_; }

    /// Force-rebind a command's chord, bypassing capture. Persists if
    /// persistence is configured. Pass `KeyCode::unknown` to unbind.
    void rebind(CommandID id, KeyCode key, std::uint16_t modifiers);

    // ── Optional callbacks ──────────────────────────────────────────

    /// Fires after a successful commit (capture or `rebind`).
    std::function<void(CommandID, KeyCode, std::uint16_t)> on_rebound;

    // ── View overrides ──────────────────────────────────────────────

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    bool on_key_event(const KeyEvent& event) override;
    bool accepts_text_input() const override { return is_capturing(); }

    /// Row height in points. Public for parent-shell layout math.
    float row_height() const { return row_height_; }
    void set_row_height(float h) { row_height_ = h > 4.0f ? h : 4.0f; }

    /// Total intrinsic height = row_height * (1 + registered command count).
    float intrinsic_height() const override;

private:
    void commit(CommandID id, KeyCode key, std::uint16_t modifiers);
    int row_at(float y) const;

    CommandRegistry& registry_;
    pulp::state::PropertiesFile* props_ = nullptr;
    std::string auto_save_path_;
    std::string persistence_prefix_ = "shortcuts";

    CommandID capture_id_ = kInvalidCommandID;
    float row_height_ = 28.0f;
};

/// Render a (key + modifiers) chord as a user-facing string, e.g.
/// "Cmd+Shift+O" or "F5". Exposed so the editor and other UIs share one
/// formatter. Modifier order is canonical (Ctrl, Alt, Shift, Cmd/Meta).
std::string format_chord(KeyCode key, std::uint16_t modifiers);

} // namespace pulp::view
