// inspector_window.hpp — Tabbed inspector window with collapsable property panels
#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/tree_view.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/inspect/console_capture.hpp>
#include <pulp/inspect/state_inspector.hpp>
#include <pulp/render/render_pass.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulp::inspect {

using namespace pulp::view;
using namespace pulp::canvas;

// ── CollapsableSection ─────────────────────────────────────────────────────
/// A disclosure-triangle section: click header to expand/collapse content.

class CollapsableSection : public View {
public:
    CollapsableSection(std::string title, bool initially_expanded = true);

    bool is_expanded() const { return expanded_; }
    void set_expanded(bool e);

    /// The content container — add children here.
    View* content() const { return content_; }

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_event(const MouseEvent& event) override;

private:
    std::string title_;
    bool expanded_;
    View* header_ = nullptr;   // owned as child
    View* content_ = nullptr;  // owned as child

    static constexpr float kHeaderHeight = 24.0f;
};

// ── ConsoleEntry (visual row) ──────────────────────────────────────────────

/// Displays a single console log entry with colored level badge and timestamp.
class ConsoleEntryView : public View {
public:
    void set_entry(const ConsoleCapture::Entry& entry);

    void paint(canvas::Canvas& canvas) override;

    float intrinsic_height() const override { return 22.0f; }

private:
    std::string level_;
    std::string message_;
    std::string timestamp_;
};

// ── InspectorWindow ────────────────────────────────────────────────────────
/// Main tabbed inspector panel with four tabs:
///   Elements | Console | Performance | State

class InspectorWindow : public View {
public:
    InspectorWindow();
    ~InspectorWindow();

    /// Convenience: construct and immediately set the inspected root.
    explicit InspectorWindow(View& root);

    // ── Data sources ────────────────────────────────────────────────
    /// Set the root view to inspect (Elements tab).
    void set_inspected_root(View* root);

    /// Set the console capture for the Console tab.
    void set_console_capture(ConsoleCapture* capture) { console_capture_ = capture; }

    /// Set the render pass manager for the Performance tab.
    void set_render_pass_manager(render::RenderPassManager* rpm) { rpm_ = rpm; }

    /// Set the state inspector for the State tab.
    void set_state_inspector(StateInspector* si) { state_inspector_ = si; }

    /// Refresh all tabs with latest data. Call once per frame.
    void refresh();

    /// Called when a view is selected in the tree.
    ///
    /// WYSIWYG P2e two-way selection (maintainer correction: "we DO want to
    /// be able to tap and select an item in the inspector as it works
    /// today"): by default (selection_readonly_ == false) a tree-row click
    /// fires this callback so the host can drive the SHARED selection —
    /// highlight the picked view in the canvas overlay. The tree row also
    /// highlights and its properties show. The forbidden behavior — a stray
    /// selection box drawn inside the inspector window — is prevented by the
    /// paint-hook root gate, not by suppressing this callback.
    ///
    /// Read-only mode (set_selection_readonly(true)) is a supported opt-out:
    /// a tree click then only shows properties and does NOT fire this
    /// callback / drive the shared selection.
    std::function<void(View* view)> on_view_selected;

    /// Select a view from external code (e.g., Cmd+click in plugin
    /// window). Fires on_view_selected — this is the WRITE path. Avoid
    /// from a sync mirror; use reflect_selection() instead so a host-side
    /// sync does not re-enter on_view_selected.
    void select_view(View* view);

    /// Reflect the canvas-driven selection into this window WITHOUT firing
    /// on_view_selected (no feedback loop into the canvas). P2e: this now
    /// HIGHLIGHTS the matching tree row AND shows the node's properties —
    /// two-way selection means a canvas pick highlights the corresponding row
    /// in the inspector tree. It is the canvas → window mirror the host calls
    /// from sync_selection; the no-feedback property (does not fire
    /// on_view_selected) is what keeps the round-trip from recursing.
    void reflect_selection(View* view);

    /// When true, clicking a tree row only shows that node's properties
    /// (read-only display) and never changes the shared selection (does
    /// not fire on_view_selected). Default false (two-way pick — the P2e
    /// behavior the maintainer wants).
    void set_selection_readonly(bool readonly) {
        selection_readonly_ = readonly;
    }
    bool selection_readonly() const { return selection_readonly_; }

    // ── P3 — Figma-style tool strip (Select / Text) ─────────────────
    //
    // (planning/2026-05-21-wysiwyg-direct-manipulation-extension.md
    // § "Future idea — Figma-style tool palette + inline text editing".)
    // A compact horizontal strip in the window header showing the two
    // tools (pointer = Select, "T" = Text) with the active one
    // highlighted. The strip is wired BOTH WAYS to the overlay's tool
    // mode via the host:
    //   * Click a tool button → on_tool_picked(0=Select, 1=Text) fires;
    //     the host calls InspectorOverlay::set_tool().
    //   * Keyboard V/T flips the overlay tool → the host calls
    //     set_active_tool() so the strip's highlight reflects it.
    /// Index of the active tool the strip highlights (0 = Select,
    /// 1 = Text). The host mirrors the overlay's tool here each tick.
    void set_active_tool(int tool_index);
    int active_tool() const { return active_tool_; }

    /// Fired when the user clicks a tool button in the strip. The
    /// argument is the tool index (0 = Select, 1 = Text). The host wires
    /// this to InspectorOverlay::set_tool().
    std::function<void(int tool_index)> on_tool_picked;

private:
    // ── Tab construction ────────────────────────────────────────────
    std::unique_ptr<View> build_elements_tab();
    std::unique_ptr<View> build_console_tab();
    std::unique_ptr<View> build_performance_tab();
    std::unique_ptr<View> build_state_tab();

    // ── Refresh helpers ─────────────────────────────────────────────
    void refresh_elements();
    void refresh_console();
    void refresh_performance();
    void refresh_state();

    void populate_tree_from_view(TreeNode& parent, View* view);
    void show_properties_for(View* view);

    // ── Data sources ────────────────────────────────────────────────
    View* inspected_root_ = nullptr;
    ConsoleCapture* console_capture_ = nullptr;
    render::RenderPassManager* rpm_ = nullptr;
    StateInspector* state_inspector_ = nullptr;

    // ── Widgets (non-owning, owned as children) ─────────────────────
    TabPanel* tabs_ = nullptr;

    // ── P3 — tool strip ─────────────────────────────────────────────
    // The strip View (custom paint + click handling) lives as the first
    // child of the window, above the tab panel. active_tool_ is the index
    // it highlights (0 = Select, 1 = Text). The strip reads active_tool_
    // each paint and calls on_tool_picked on a button click.
    class ToolStrip;
    ToolStrip* tool_strip_ = nullptr;
    int active_tool_ = 0;

    // Elements tab
    TreeView* tree_view_ = nullptr;
    ScrollView* tree_scroll_ = nullptr;  // viewport wrapping tree_view_
    CollapsableSection* identity_section_ = nullptr;
    CollapsableSection* layout_section_ = nullptr;
    CollapsableSection* visual_section_ = nullptr;
    CollapsableSection* widget_section_ = nullptr;
    CollapsableSection* theme_section_ = nullptr;
    View* elements_props_container_ = nullptr;

    // Console tab
    ScrollView* console_scroll_ = nullptr;
    View* console_list_ = nullptr;

    // Performance tab
    Label* fps_label_ = nullptr;
    Label* frame_time_label_ = nullptr;
    Label* gpu_render_label_ = nullptr;
    Label* budget_label_ = nullptr;
    View* pass_list_ = nullptr;
    Label* view_count_label_ = nullptr;

    // State tab
    ScrollView* state_scroll_ = nullptr;
    View* state_list_ = nullptr;

    // Currently selected view (Elements tab)
    View* selected_view_ = nullptr;

    // WYSIWYG P2e: when true, tree-row clicks only display properties
    // read-only and never drive the shared selection (opt-out). Default
    // false — two-way selection, the maintainer's wanted behavior. See
    // set_selection_readonly().
    bool selection_readonly_ = false;

    // P2e — reflect_selection() (the canvas → window mirror) now HIGHLIGHTS
    // the matching tree row + shows props, the same as a tree-row pick. The
    // P2d "reflect-state-only" suppression (which left the row un-highlighted)
    // was reverted: two-way selection is wanted, and the only thing forbidden
    // is the stray box inside the inspector window, fixed in the paint hook.

    // P2d (A) — tree-structure signature, to suppress the empty-content
    // flicker. refresh_elements() runs on every idle tick (~30 Hz); rebuilding
    // the whole TreeView each tick clears then repopulates it, which flashes
    // empty. We hash the inspected tree's structure (type + id per node) and
    // only rebuild when it actually changed; otherwise we just refresh the
    // property labels for the current selection. Zero means "never built".
    std::size_t tree_signature_ = 0;
    // Compute the structural signature of the inspected tree.
    std::size_t compute_tree_signature(const View* view) const;

    // Property labels (Elements tab, Identity section)
    Label* prop_type_label_ = nullptr;
    Label* prop_id_label_ = nullptr;

    // Layout section labels
    Label* prop_bounds_label_ = nullptr;
    Label* prop_direction_label_ = nullptr;
    Label* prop_gap_label_ = nullptr;
    Label* prop_padding_label_ = nullptr;
    Label* prop_margin_label_ = nullptr;
    Label* prop_grow_shrink_label_ = nullptr;

    // Visual section labels
    Label* prop_opacity_label_ = nullptr;
    Label* prop_bgcolor_label_ = nullptr;
    Label* prop_border_label_ = nullptr;
    Label* prop_corner_label_ = nullptr;
    Label* prop_visible_label_ = nullptr;
    Label* prop_enabled_label_ = nullptr;

    // Widget section labels
    Label* prop_widget_value_label_ = nullptr;
};

} // namespace pulp::inspect
