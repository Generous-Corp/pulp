#pragma once

/// @file tree_view.hpp
/// Hierarchical tree display widget for file browsers, plugin lists, etc.

#include <pulp/view/view.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/canvas/canvas.hpp>
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace pulp::view {

/// A node in the tree hierarchy.
struct TreeNode {
    std::string label;
    std::string icon;          ///< Optional icon name
    bool expanded = false;     ///< Whether children are visible
    bool selectable = true;
    void* user_data = nullptr; ///< Arbitrary payload

    std::vector<std::unique_ptr<TreeNode>> children;

    /// Add a child node and return a reference to it.
    TreeNode& add_child(std::string child_label) {
        children.push_back(std::make_unique<TreeNode>());
        children.back()->label = std::move(child_label);
        return *children.back();
    }

    /// Check if this node has children.
    bool has_children() const { return !children.empty(); }

    /// Toggle expanded state.
    void toggle() { expanded = !expanded; }
};

/// Hierarchical tree view widget.
///
/// Displays a collapsible tree of items with indent levels, expand/collapse
/// triangles, and selection. Useful for preset browsers, file trees, etc.
///
/// @code
/// TreeView tree;
/// auto& root = tree.root();
/// auto& synths = root.add_child("Synths");
/// synths.add_child("Bass");
/// synths.add_child("Lead");
/// auto& fx = root.add_child("Effects");
/// fx.add_child("Reverb");
///
/// tree.on_select = [&](TreeNode& node) { load_preset(node.label); };
/// @endcode
class TreeView : public View {
public:
    TreeView() { set_focusable(true); }

    /// The invisible root node (its children are the top-level items).
    TreeNode& root() { return root_; }
    const TreeNode& root() const { return root_; }

    /// Called when a node is selected (clicked).
    std::function<void(TreeNode& node)> on_select;

    /// Called when a node is double-clicked (activated).
    std::function<void(TreeNode& node)> on_activate;

    /// Called when a node is expanded or collapsed.
    std::function<void(TreeNode& node, bool expanded)> on_toggle;

    /// Row height in pixels.
    float row_height() const { return row_height_; }
    void set_row_height(float h) { row_height_ = h; }

    /// Indent per level in pixels.
    float indent() const { return indent_; }
    void set_indent(float px) { indent_ = px; }

    /// Get the currently selected node (nullptr if none).
    TreeNode* selected_node() const { return selected_; }

    /// Programmatically select a node (without firing on_select).
    void set_selected_node(TreeNode* node) { selected_ = node; }

    /// Find a node whose user_data matches the given pointer.
    /// Returns nullptr if not found.
    TreeNode* find_node_by_user_data(void* data) {
        return find_by_user_data(root_, data);
    }

    /// Number of rows currently painted (every node under an expanded
    /// ancestor; collapsed subtrees are excluded). The invisible root is
    /// not counted.
    int visible_node_count() const { return count_visible(root_, -1); }

    /// Intrinsic content height in pixels: visible rows * row height.
    /// A ScrollView wrapping this TreeView should size the tree to this
    /// height so expanded nodes past the viewport remain scrollable
    /// rather than clipped.
    float content_height() const {
        return static_cast<float>(visible_node_count()) * row_height_;
    }

    void paint(canvas::Canvas& canvas) override {
        auto b = local_bounds();

        canvas.save();
        canvas.clip_rect(b.x, b.y, b.width, b.height);

        float y = b.y - scroll_offset_;
        paint_node(canvas, root_, -1, b.x, y, b.width);

        canvas.restore();
    }

    void on_mouse_event(const MouseEvent& event) override {
        if (!event.is_down) return;

        float y = event.position.y + scroll_offset_;
        float current_y = 0;
        int hit_depth = 0;
        TreeNode* hit = find_node_at_y(root_, -1, current_y, y, &hit_depth);

        if (hit) {
            if (event.click_count == 2) {
                if (on_activate) on_activate(*hit);
            } else {
                // Check if click is on the expand triangle area
                float indent_x = static_cast<float>(hit_depth) * indent_;
                float triangle_end = indent_x + indent_;
                if (hit->has_children() && event.position.x < triangle_end) {
                    hit->toggle();
                    if (on_toggle) on_toggle(*hit, hit->expanded);
                } else if (hit->selectable) {
                    selected_ = hit;
                    if (on_select) on_select(*hit);
                }
            }
        }
    }

    bool on_key_event(const KeyEvent& event) override {
        if (!event.is_down) return false;

        if (event.key == KeyCode::right && selected_ && selected_->has_children()) {
            selected_->expanded = true;
            if (on_toggle) on_toggle(*selected_, true);
            return true;
        }
        if (event.key == KeyCode::left && selected_) {
            if (selected_->expanded && selected_->has_children()) {
                selected_->expanded = false;
                if (on_toggle) on_toggle(*selected_, false);
            }
            return true;
        }
        return false;
    }

private:
    TreeNode root_;
    TreeNode* selected_ = nullptr;
    float row_height_ = 22.0f;
    float indent_ = 18.0f;
    float scroll_offset_ = 0.0f;

    /// Space reserved for the disclosure triangle column. Wider than the
    /// glyph so the label always starts AFTER the triangle with a visible
    /// gap (the triangle is drawn at indent_x + 2 in a ~9px font, ending
    /// near indent_x + 9; the label begins at indent_x + kTriangleWidth).
    static constexpr float kTriangleWidth = 18.0f;
    /// Explicit small font size for the disclosure glyph so it never
    /// inherits a stale/oversized font from a prior canvas call.
    static constexpr float kTriangleFontSize = 9.0f;

    void paint_node(canvas::Canvas& canvas, TreeNode& node, int depth,
                    float x, float& y, float width) {
        if (depth >= 0) { // Don't paint the invisible root
            float indent_x = x + static_cast<float>(depth) * indent_;

            // Selection highlight
            if (&node == selected_) {
                canvas.set_fill_color(resolve_color("tree_selected_bg",
                    canvas::Color::hex(0x0f3460)));
                canvas.fill_rect(x, y, width, row_height_);
            }

            // Expand triangle. Draw with an explicit SMALL font so it
            // doesn't inherit the stale/large font from a previous paint
            // call (which made the glyph render oversized and overlap the
            // label). The triangle sits in its own column, fully BEFORE
            // the label text with a clear gap (see kTriangleWidth / the
            // label_x offset below).
            if (node.has_children()) {
                canvas.set_font("system", kTriangleFontSize);
                canvas.set_fill_color(resolve_color("text_muted",
                    canvas::Color::hex(0x808090)));
                float tx = indent_x + 2;
                // Vertically center the small glyph on the row. fill_text's
                // y is the baseline, so offset up from the row middle by
                // roughly a third of the glyph height.
                float ty = y + row_height_ / 2 + kTriangleFontSize * 0.35f;
                if (node.expanded) {
                    // Down-pointing triangle
                    canvas.fill_text("\xe2\x96\xbc", tx, ty); // ▼
                } else {
                    canvas.fill_text("\xe2\x96\xb6", tx, ty); // ▶
                }
            }

            // Label — offset past the triangle column so the arrow always
            // sits cleanly before the text with a visible gap.
            float label_x = indent_x + kTriangleWidth;
            canvas.set_font("system", 13);
            canvas.set_fill_color(resolve_color("text",
                canvas::Color::hex(0xe0e0e0)));
            canvas.fill_text(node.label, label_x, y + row_height_ * 0.7f);

            y += row_height_;
        }

        if (depth < 0 || node.expanded) {
            for (auto& child : node.children) {
                paint_node(canvas, *child, depth + 1, x, y, width);
            }
        }
    }

    int count_visible(const TreeNode& node, int depth) const {
        int n = (depth >= 0) ? 1 : 0; // don't count the invisible root
        if (depth < 0 || node.expanded) {
            for (const auto& child : node.children) {
                n += count_visible(*child, depth + 1);
            }
        }
        return n;
    }

    TreeNode* find_by_user_data(TreeNode& node, void* data) {
        if (node.user_data == data && data != nullptr) return &node;
        for (auto& child : node.children) {
            if (auto* found = find_by_user_data(*child, data)) return found;
        }
        return nullptr;
    }

    TreeNode* find_node_at_y(TreeNode& node, int depth, float& current_y, float target_y,
                             int* out_depth = nullptr) {
        if (depth >= 0) {
            if (target_y >= current_y && target_y < current_y + row_height_) {
                if (out_depth) *out_depth = depth;
                return &node;
            }
            current_y += row_height_;
        }

        if (depth < 0 || node.expanded) {
            for (auto& child : node.children) {
                auto* result = find_node_at_y(*child, depth + 1, current_y, target_y, out_depth);
                if (result) return result;
            }
        }
        return nullptr;
    }
};

} // namespace pulp::view
