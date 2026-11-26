#pragma once

/// @file inspector_window.hpp
/// The component inspector's tabbed window as a Vellum view: a tool strip
/// above an Elements tab that lists the inspected view tree beside collapsable
/// Identity, Layout, Visual, Widget and Theme Colors sections.
///
/// The window is a view, not an OS window: a host places it in a window or
/// panel of its choice and calls `refresh()` once per frame.

#include <pulp/vellum_components/aliases.hpp>

#include <vellum/view/tree_view.hpp>
#include <vellum/view/ui_components.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace pulp::vellum_components {

/// A disclosure-triangle section: clicking the header expands or collapses
/// the content container.
class CollapsableSection : public View {
  public:
    explicit CollapsableSection(std::string title, bool initially_expanded = true);

    bool is_expanded() const {
        return expanded_;
    }
    void set_expanded(bool expanded);

    /// The content container; add rows here.
    View* content() const {
        return content_;
    }

    void on_mouse_event(const ::vellum::view::MouseEvent& event) override;

  private:
    std::string title_;
    bool expanded_;
    View* header_ = nullptr;
    View* content_ = nullptr;

    static constexpr float kHeaderHeight = 24.0f;
};

class InspectorWindow : public View {
  public:
    InspectorWindow();
    ~InspectorWindow() override;

    /// Construct and immediately inspect `root`.
    explicit InspectorWindow(View& root);

    /// Set the root view to inspect, or nullptr for none.
    void set_inspected_root(View* root);

    /// Refresh the active tab from the live tree. Call once per frame; the
    /// tree is rebuilt only when its structure changed.
    void refresh();

    /// Select a tab by title; no-op when the title matches no tab.
    void select_tab(std::string_view title);

    /// Fires when a view is selected, from a tree row or `select_view`.
    ///
    /// Selection is two-way: a tree-row click highlights the row, shows the
    /// properties and fires this callback so the host can mirror it into a
    /// canvas overlay. In read-only mode the click only shows properties.
    std::function<void(View* view)> on_view_selected;

    /// Select a view from external code. Shows its properties and fires
    /// `on_view_selected`; use `reflect_selection` from a mirror that must
    /// not re-enter the callback.
    void select_view(View* view);

    /// Reflect a canvas-driven selection without firing `on_view_selected`:
    /// highlights the matching tree row and shows the properties.
    void reflect_selection(View* view);

    /// When true a tree-row click shows properties but never fires
    /// `on_view_selected`.
    void set_selection_readonly(bool readonly) {
        selection_readonly_ = readonly;
    }
    bool selection_readonly() const {
        return selection_readonly_;
    }

    /// Index of the tool the strip highlights: 0 Select, 1 Text. Any other
    /// value selects 0.
    void set_active_tool(int tool_index);
    int active_tool() const {
        return active_tool_;
    }

    /// Fires when a tool button is clicked, with the tool index.
    std::function<void(int tool_index)> on_tool_picked;

  private:
    std::unique_ptr<View> build_elements_tab();
    void refresh_elements();
    void clear_elements();
    void populate_tree_from_view(::vellum::view::TreeNode& parent, View* view);
    void show_properties_for(View* view);
    std::size_t compute_tree_signature(const View* view) const;

    View* inspected_root_ = nullptr;
    ::vellum::view::TabPanel* tabs_ = nullptr;

    class ToolStrip;
    ToolStrip* tool_strip_ = nullptr;
    int active_tool_ = 0;

    ::vellum::view::TreeView* tree_view_ = nullptr;
    ::vellum::view::ScrollView* tree_scroll_ = nullptr;
    CollapsableSection* identity_section_ = nullptr;
    CollapsableSection* layout_section_ = nullptr;
    CollapsableSection* visual_section_ = nullptr;
    CollapsableSection* widget_section_ = nullptr;
    CollapsableSection* theme_section_ = nullptr;
    View* elements_props_container_ = nullptr;

    View* selected_view_ = nullptr;
    bool selection_readonly_ = false;

    // Structural hash of the inspected tree. refresh() runs every idle tick;
    // rebuilding the TreeView each time would flash it empty, so the tree is
    // rebuilt only when the hash changes. Zero means never built.
    std::size_t tree_signature_ = 0;

    Label* prop_type_label_ = nullptr;
    Label* prop_id_label_ = nullptr;
    Label* prop_bounds_label_ = nullptr;
    Label* prop_direction_label_ = nullptr;
    Label* prop_gap_label_ = nullptr;
    Label* prop_padding_label_ = nullptr;
    Label* prop_margin_label_ = nullptr;
    Label* prop_grow_shrink_label_ = nullptr;
    Label* prop_opacity_label_ = nullptr;
    Label* prop_bgcolor_label_ = nullptr;
    Label* prop_border_label_ = nullptr;
    Label* prop_corner_label_ = nullptr;
    Label* prop_visible_label_ = nullptr;
    Label* prop_enabled_label_ = nullptr;
    Label* prop_widget_value_label_ = nullptr;
};

} // namespace pulp::vellum_components
