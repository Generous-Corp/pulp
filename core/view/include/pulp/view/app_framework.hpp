#pragma once

/// @file app_framework.hpp
/// Application framework components: MenuBar, Toolbar, keyboard shortcuts,
/// application settings persistence.

#include <pulp/view/input_events.hpp>
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <optional>

namespace pulp::view {

// ── Command System ───────────────────────────────────────────────────────

/// A unique command identifier.
using CommandID = uint32_t;

/// A keyboard shortcut (key + modifiers).
struct KeyShortcut {
    KeyCode key = KeyCode::unknown;
    uint16_t modifiers = 0;

    bool matches(const KeyEvent& event) const {
        return event.key == key && event.modifiers == modifiers && event.is_down;
    }

    /// Create from string like "Cmd+S", "Ctrl+Shift+Z", "F5".
    static KeyShortcut from_string(const std::string& s);

    /// Convert to display string.
    std::string to_string() const;
};

/// A named command with optional shortcut and callback.
struct Command {
    CommandID id = 0;
    std::string name;
    std::string category;      ///< Menu category ("File", "Edit", etc.)
    KeyShortcut shortcut;
    std::function<void()> action;
    std::function<bool()> is_enabled;  ///< Returns false to grey out
    bool is_separator = false; ///< True for menu separator items
};

// ── KeyMapping ───────────────────────────────────────────────────────────

/// Manages keyboard shortcuts with configurable bindings.
///
/// @code
/// KeyMapping keys;
/// keys.add_command({1, "Save", "File", KeyShortcut{KeyCode::s, kModCmd},
///                   [&] { save(); }});
/// keys.add_command({2, "Undo", "Edit", KeyShortcut{KeyCode::z, kModCmd},
///                   [&] { undo(); }});
///
/// // In key event handler:
/// if (keys.handle_key(event)) return true;
/// @endcode
class KeyMapping {
public:
    void add_command(Command cmd);
    void remove_command(CommandID id);

    /// Try to handle a key event by matching against registered shortcuts.
    /// @return True if a matching command was found and executed.
    bool handle_key(const KeyEvent& event);

    /// Get all registered commands.
    const std::vector<Command>& commands() const { return commands_; }

    /// Get commands in a specific category.
    std::vector<const Command*> commands_in_category(const std::string& category) const;

    /// Rebind a command's shortcut.
    void rebind(CommandID id, KeyShortcut shortcut);

    /// Save keybindings to a JSON file.
    bool save_bindings(const std::filesystem::path& path) const;

    /// Load keybindings from a JSON file (overrides defaults).
    bool load_bindings(const std::filesystem::path& path);

private:
    std::vector<Command> commands_;
};

// ── MenuBar ──────────────────────────────────────────────────────────────

/// Cross-platform menu bar descriptor.
///
/// MenuBar owns menu titles, items, actions, shortcuts, enabled state, and
/// submenu metadata. Hosts that need a platform menu should consume `menus()`
/// and install their own backend.
///
/// @code
/// MenuBar menu;
/// menu.set_key_mapping(keys);  // auto-populates from commands
///
/// // Or build manually:
/// menu.add_menu("File", {
///     {"New", [&]{ new_project(); }, KeyShortcut{KeyCode::n, kModCmd}},
///     {"Open...", [&]{ open(); }, KeyShortcut{KeyCode::o, kModCmd}},
///     {}, // separator
///     {"Quit", [&]{ quit(); }, KeyShortcut{KeyCode::q, kModCmd}},
/// });
/// @endcode
class MenuBar {
public:
    struct MenuItem {
        std::string title;
        std::function<void()> action;
        KeyShortcut shortcut;
        bool enabled = true;
        bool is_separator = false;
        std::vector<MenuItem> submenu; ///< Non-empty for submenu items
    };

    struct Menu {
        std::string title;
        std::vector<MenuItem> items;
    };

    /// Add a menu with items.
    void add_menu(std::string title, std::vector<MenuItem> items);

    /// Build menus automatically from a KeyMapping's command categories.
    void set_key_mapping(const KeyMapping& keys);

    /// Get all menus for custom rendering (Linux, embedded).
    const std::vector<Menu>& menus() const { return menus_; }

private:
    std::vector<Menu> menus_;
};

// ── NativeToolbar ────────────────────────────────────────────────────────

/// Native-toolbar descriptor — a lightweight config struct for host-level
/// toolbars. Separate from
/// pulp::view::Toolbar (core/view/include/pulp/view/toolbar.hpp), which is
/// a full View subclass with custom paint/hit-testing.
///
/// Native toolbar installation is host/platform work outside this descriptor.
class NativeToolbar {
public:
    struct Item {
        std::string id;
        std::string label;
        std::string icon_name;     ///< SF Symbol name (macOS) or icon path
        std::function<void()> action;
        bool is_separator = false;
    };

    void add_item(Item item);
    void add_separator();

    const std::vector<Item>& items() const { return items_; }

private:
    std::vector<Item> items_;
};

// ── AppSettings ──────────────────────────────────────────────────────────

/// Persistent application settings (window size, device selection, preferences).
///
/// Settings are stored as JSON in platform-standard locations:
/// - macOS: ~/Library/Application Support/<app_name>/settings.json
/// - Windows: %APPDATA%/<app_name>/settings.json
/// - Linux: ~/.config/<app_name>/settings.json
class AppSettings {
public:
    explicit AppSettings(const std::string& app_name);

    /// Get a string setting.
    std::optional<std::string> get_string(const std::string& key) const;
    /// Get an int setting.
    std::optional<int> get_int(const std::string& key) const;
    /// Get a float setting.
    std::optional<float> get_float(const std::string& key) const;
    /// Get a bool setting.
    std::optional<bool> get_bool(const std::string& key) const;

    /// Set values (not persisted until save() is called).
    void set_string(const std::string& key, const std::string& value);
    void set_int(const std::string& key, int value);
    void set_float(const std::string& key, float value);
    void set_bool(const std::string& key, bool value);

    /// Persist to disk.
    bool save();
    /// Load from disk.
    bool load();

    /// Convenience: save/restore window geometry.
    void save_window_state(int x, int y, int width, int height);
    struct WindowState { int x, y, width, height; };
    std::optional<WindowState> load_window_state() const;

    /// Settings file path.
    std::filesystem::path settings_path() const { return settings_path_; }

private:
    std::filesystem::path settings_path_;
    std::unordered_map<std::string, std::string> values_;
};

} // namespace pulp::view
