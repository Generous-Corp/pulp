#pragma once

/// @file inspector_window.hpp
/// Separate floating inspector window with TreeView and PropertyList.
/// Opens alongside the plugin window via Cmd+I (macOS) / Ctrl+I (others).
/// Uses existing multi-window infrastructure (WindowManager, WindowType::inspector).

#include <pulp/view/view.hpp>
#include <pulp/view/command_registry.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/tree_view.hpp>
#include <pulp/view/property_list.hpp>
#include <pulp/view/split_view.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/window_manager.hpp>
#include <functional>
#include <memory>

namespace pulp::view {

/// Highlight overlay added to the target root to visualize the selected view bounds.
/// Draws a semi-transparent blue rectangle. Not hit-testable.
class InspectorHighlight : public View {
public:
    InspectorHighlight();

    /// Set the rectangle to highlight (in root coordinates).
    void set_highlight_rect(Rect r);

    /// Clear the highlight.
    void clear();

    void paint(canvas::Canvas& canvas) override;

private:
    Rect highlight_rect_{};
    bool has_rect_ = false;
};

/// Floating inspector window that displays the view hierarchy and properties
/// of a target view tree in a separate OS window.
///
/// Privately implements `CommandHandler` so the inspector can register its
/// toggle command with a shell-owned `CommandRegistry` (see
/// `register_command_handler`) without exposing the handler interface as
/// public API.
class InspectorWindow : private CommandHandler {
public:
    using HostFactory = std::function<std::unique_ptr<WindowHost>(
        View& root, const WindowOptions& options)>;

    /// Command id for the "Toggle Layout Inspector" action registered by
    /// `register_command_handler()`. Stable so apps can reference it from
    /// menus or a `KeyMappingEditor`. ASCII 'PLPI'.
    static constexpr CommandID kToggleCommand = 0x504C5049;

    /// Construct an inspector targeting the given root view.
    /// @param target_root  The root of the view tree to inspect.
    /// @param mgr          Optional WindowManager for registration.
    /// @param parent        Optional parent WindowHost (positions relative to it).
    /// @param host_factory Optional host creation override for tests or embedders
    ///                     that route inspector windows through a custom host.
    InspectorWindow(View& target_root, WindowManager* mgr = nullptr,
                    WindowHost* parent = nullptr,
                    HostFactory host_factory = {});
    ~InspectorWindow();

    InspectorWindow(const InspectorWindow&) = delete;
    InspectorWindow& operator=(const InspectorWindow&) = delete;

    /// Show the inspector window.
    void show();

    /// Hide the inspector window.
    void hide();

    /// Toggle visibility.
    void toggle();

    /// Whether the inspector window is currently visible.
    bool is_visible() const;

    /// Refresh the tree and properties from the target root.
    void refresh();

    /// Select a specific view and update the property list.
    void select_view(View* view);

    /// Register the toggle command (`kToggleCommand`, default chord
    /// Cmd+I / Ctrl+I) with a caller-owned `CommandRegistry`. Preferred over
    /// `install_keyboard_shortcut()`: the registry path composes with other
    /// tools instead of clobbering `View::on_global_key`. RAII: the handler
    /// is removed automatically in the destructor, so dispatch after this
    /// window is destroyed simply finds no handler. The registry must
    /// outlive this window (the destructor calls back into it).
    /// Re-registering moves the handler to the new registry. Does NOT touch
    /// `target_root_.on_global_key`; route the root key path once per shell
    /// with `route_global_keys(root, registry)`.
    void register_command_handler(CommandRegistry& registry);

    /// Install Cmd+I / Ctrl+I keyboard shortcut on the target root's
    /// on_global_key.
    ///
    /// DEPRECATED — direct assignment owns the entire global-key hook, so a
    /// second tool installing its shortcut silently disables this one. New
    /// shells should own a `CommandRegistry`, call `route_global_keys()`
    /// once, and use `register_command_handler()` instead. Kept for
    /// backward compatibility with single-tool callers.
    void install_keyboard_shortcut();

private:
    // CommandHandler (private base) — registry dispatch entry points.
    std::vector<CommandID> commands() const override;
    bool perform_command(CommandID id) override;

    View& target_root_;
    WindowManager* manager_ = nullptr;
    WindowHost* parent_host_ = nullptr;
    HostFactory host_factory_;
    CommandRegistry* registry_ = nullptr;

    // Inspector window's own view tree
    std::unique_ptr<View> inspector_root_;
    TreeView* tree_view_ = nullptr;
    PropertyList* property_list_ = nullptr;
    Label* header_label_ = nullptr;

    // The actual OS window
    std::unique_ptr<WindowHost> window_host_;
    WindowId window_id_ = 0;

    // Highlight overlay on the target root
    std::unique_ptr<InspectorHighlight> highlight_;
    InspectorHighlight* highlight_ptr_ = nullptr;

    View* selected_view_ = nullptr;

    void build_ui();
    void on_tree_select(TreeNode& node);
    void update_highlight();
};

} // namespace pulp::view
