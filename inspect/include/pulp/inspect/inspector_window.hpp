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
    /// WYSIWYG decoupling (planning/2026-05-21 § R-decouple, maintainer
    /// "we don't want to select items in the inspector ever"): when
    /// `selection_readonly_` is set (set_selection_readonly(true)), a
    /// click in the tree does NOT fire this callback and does NOT change
    /// the shared selection — it only shows that node's properties for
    /// READ-ONLY display. Selection is then driven exclusively by the
    /// in-canvas overlay. Hosts that want the legacy two-way coupling
    /// leave read-only mode OFF (the default), so existing behavior is
    /// unchanged.
    std::function<void(View* view)> on_view_selected;

    /// Select a view from external code (e.g., Cmd+click in plugin
    /// window). Fires on_view_selected — this is the WRITE path. Avoid
    /// from a sync mirror; use reflect_selection() instead so the canvas
    /// stays the single selection source.
    void select_view(View* view);

    /// Reflect the canvas-driven selection into this window WITHOUT
    /// firing on_view_selected (no feedback loop) and WITHOUT treating it
    /// as a window-originated pick. Shows the node's properties read-only.
    /// This is the one-way mirror the WYSIWYG decoupling uses: the canvas
    /// overlay is the only selection source; the floating window only
    /// reflects.
    void reflect_selection(View* view);

    /// When true, clicking a tree row only shows that node's properties
    /// (read-only display) and never changes the shared selection (does
    /// not fire on_view_selected). Default false (legacy two-way pick).
    void set_selection_readonly(bool readonly) {
        selection_readonly_ = readonly;
    }
    bool selection_readonly() const { return selection_readonly_; }

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

    // Elements tab
    TreeView* tree_view_ = nullptr;
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

    // WYSIWYG decoupling: when true, tree-row clicks only display
    // properties read-only and never drive the shared selection. See
    // set_selection_readonly().
    bool selection_readonly_ = false;

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
