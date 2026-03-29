#pragma once

#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/state/store.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace pulp::view {

// Bridges JS scripts to the Pulp widget system.
// Registers native functions that JS code calls to create, configure,
// layout, style, and interact with widgets.
class WidgetBridge {
public:
    WidgetBridge(ScriptEngine& engine, View& root, state::StateStore& store);

    // Load and execute a UI script
    void load_script(const std::string& code);

    // Get a widget by its JS-assigned ID
    View* widget(const std::string& id);

    // Sync all widget values from the parameter store
    void sync_from_store();

    // Hot reload support: clear all JS-created widgets
    void clear();

    // Forward a key event to JS (called by host for global shortcuts)
    void forward_key_event(int key_code, uint16_t modifiers, bool is_down);

    // Snapshot widget values for preservation across hot reload
    void snapshot_values(std::unordered_map<std::string, float>& out) const;

    // Restore widget values after hot reload rebuild
    void restore_values(const std::unordered_map<std::string, float>& snapshot);

private:
    ScriptEngine& engine_;
    View& root_;
    state::StateStore& store_;

    // Track widgets by ID for JS access
    std::unordered_map<std::string, View*> widgets_;

    // Registered keyboard shortcuts from JS
    struct ShortcutBinding {
        KeyCode key;
        uint16_t modifiers;
        std::string callback;
    };
    std::vector<ShortcutBinding> shortcuts_;

    // Model-agnostic AI CLI command (default: Claude)
    std::string ai_cli_command_ = "claude --print --model claude-sonnet-4-6";

    // Resolve parent: returns view for parentId, or &root_ if empty
    View* resolve_parent(const std::string& parent_id);

    // Install on_change/on_toggle callbacks that dispatch to JS
    void wire_callbacks(const std::string& id, View* w);

    void register_api();
};

} // namespace pulp::view
