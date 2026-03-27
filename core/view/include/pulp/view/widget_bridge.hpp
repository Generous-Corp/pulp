#pragma once

#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/state/store.hpp>
#include <string>
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

    // Resolve parent: returns view for parentId, or &root_ if empty
    View* resolve_parent(const std::string& parent_id);

    // Install on_change/on_toggle callbacks that dispatch to JS
    void wire_callbacks(const std::string& id, View* w);

    void register_api();
};

} // namespace pulp::view
