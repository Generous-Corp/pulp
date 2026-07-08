#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <typeindex>
#include <vector>

#include <pulp/view/view.hpp>
#include <pulp/view/view_pool.hpp>
#include <pulp/view/virtual_list.hpp>

namespace pulp::view {

/// Recycling virtualized 2D grid for large, scrolling, rich cell views.
///
/// VirtualGrid keeps a bounded pool of cell View objects sized to the visible
/// viewport (rows x columns) plus overscan rows. As the scroll offset changes,
/// existing cells are re-bound to new data indices in row-major order instead
/// of creating one View per item.
///
/// This deliberately duplicates the RowSlot/alive/generation recycling
/// machinery of VirtualList rather than sharing it. The shared per-class view
/// recycling pool (planning item 2.3, "ViewPool") is the intended de-duplication
/// step; until it lands, VirtualGrid carries its own copy of the hard-won
/// reentrancy discipline (alive token, window generation, pool-resize guard,
/// destroying flag) so a binder/releaser/factory that mutates or destroys the
/// grid mid-callback cannot corrupt the pool. Do not abstract this out early.
class VirtualGrid : public View {
public:
    enum class SelectionMode { none, single, multi };

    using CellFactory = std::function<std::unique_ptr<View>(std::size_t slot)>;
    using CellBinder = std::function<void(View& cell, std::size_t index)>;
    using CellReleaser = std::function<void(View& cell)>;

    VirtualGrid();
    ~VirtualGrid() override;

    void set_item_count(std::size_t n);
    std::size_t item_count() const { return item_count_; }

    void set_cell_size(float width, float height);
    float cell_width() const { return cell_w_; }
    float cell_height() const { return cell_h_; }

    /// Fixed number of columns. Pass 0 (the default) to derive the column count
    /// from `local_bounds().width / cell_width()`.
    void set_column_count(int columns);
    int column_count() const { return column_count_; }
    /// Effective column count actually used this frame (>= 1).
    std::size_t columns() const;

    void set_overscan(int rows);
    int overscan() const { return overscan_rows_; }

    void set_cell_factory(CellFactory factory);
    void set_cell_binder(CellBinder binder);
    void set_cell_releaser(CellReleaser releaser);
    void refresh_cells();

    void set_selection_mode(SelectionMode mode);
    SelectionMode selection_mode() const { return selection_mode_; }
    const std::vector<std::size_t>& selection() const { return selection_; }
    bool is_selected(std::size_t index) const;
    void clear_selection();
    void select_cell(std::size_t index);
    void toggle_cell_selection(std::size_t index);

    std::optional<std::size_t> focused_index() const { return focused_index_; }
    void set_focused_index(std::size_t index);

    void scroll_to_item(std::size_t index);
    void set_scroll_y(float y);
    void scroll_by(float dy);
    float scroll_y() const { return scroll_y_; }
    float content_height() const;
    std::size_t total_rows() const;

    void on_cell_activated(std::function<void(std::size_t)> cb) {
        on_cell_activated_ = std::move(cb);
    }
    void on_selection_changed(std::function<void(const std::vector<std::size_t>&)> cb) {
        on_selection_changed_ = std::move(cb);
    }

    std::size_t realized_cell_count() const { return cell_slots_.size(); }
    View* realized_cell_at_slot(std::size_t slot);
    const View* realized_cell_at_slot(std::size_t slot) const;
    std::optional<std::size_t> bound_index_for_slot(std::size_t slot) const;
    std::size_t first_realized_index() const { return first_realized_index_; }
    std::vector<std::size_t> realized_indices() const;

    const VirtualListDirtyTracker& dirty_tracker() const { return dirty_tracker_; }
    void clear_dirty();

    void on_resized() override;
    View* hit_test(Point local_point) override;
    void layout_children() override;
    bool owns_child_layout() const override { return true; }
    void paint(canvas::Canvas& canvas) override;
    void on_mouse_event(const MouseEvent& event) override;
    void on_mouse_drag(Point pos) override;
    bool on_key_event(const KeyEvent& event) override;
    bool wants_wheel_scroll() const override;

private:
    enum class UpdateResult { unchanged, changed, interrupted };
    struct SelectionUpdate {
        bool changed = false;
        bool interrupted = false;
    };

    struct CellSlot {
        View* cell = nullptr;
        std::optional<std::size_t> index;
    };

    std::size_t desired_pool_size() const;
    bool clear_pool();
    bool ensure_pool_size(std::size_t desired);
    bool release_cell(CellSlot& slot);
    bool update_window(bool force_rebind);
    bool bind_slot(CellSlot& slot, std::size_t index, bool force_rebind,
                   std::uint64_t update_generation);
    void position_slot(CellSlot& slot);
    void update_cell_accessibility(CellSlot& slot);
    float max_scroll_y() const;
    bool scroll_to_item_internal(std::size_t index);
    UpdateResult set_scroll_y_internal(float y, bool request);
    bool apply_pending_scroll_to_item();
    bool apply_pending_selected_cell();
    void mark_scroll_dirty(float old_y, float new_y);
    float grid_width() const;

    std::optional<std::size_t> cell_at(Point local) const;
    void choose_cell(std::size_t index, bool extend, bool toggle, bool claim_focus = false);
    SelectionUpdate set_selection_sanitized(std::vector<std::size_t> next, bool notify = true);
    void move_focus_to(std::size_t index, bool extend);
    std::optional<std::size_t> keyboard_anchor_index() const;
    std::size_t visible_rows() const;
    std::size_t page_cell_delta() const;

    bool scrollbar_visible() const;
    float scrollbar_width() const;
    float scrollbar_thumb_length() const;
    float scrollbar_thumb_y() const;
    bool point_in_scrollbar(Point local) const;
    bool point_in_scrollbar_thumb(Point local) const;

    std::size_t item_count_ = 0;
    float cell_w_ = 80.0f;
    float cell_h_ = 60.0f;
    int column_count_ = 0;  // 0 => derive from width
    int overscan_rows_ = 2;
    float scroll_y_ = 0.0f;
    std::optional<std::size_t> pending_scroll_to_item_;
    std::optional<std::size_t> pending_selected_cell_;

    CellFactory cell_factory_;
    CellBinder cell_binder_;
    CellReleaser cell_releaser_;
    std::vector<CellSlot> cell_slots_;
    std::size_t first_realized_index_ = 0;

    // Per-class recycling pool for released cells (planning 2.3). Mirrors
    // VirtualList's row_pool_: opt-in cells (View::supports_reuse()) are parked
    // on release and re-acquired before the user factory. Behavior-neutral for
    // the default View cells the built-in grid uses.
    ViewPool cell_pool_;
    std::optional<std::type_index> cell_type_;

    SelectionMode selection_mode_ = SelectionMode::single;
    std::vector<std::size_t> selection_;
    std::optional<std::size_t> focused_index_;
    std::optional<std::size_t> selection_anchor_;
    std::function<void(std::size_t)> on_cell_activated_;
    std::function<void(const std::vector<std::size_t>&)> on_selection_changed_;

    VirtualListDirtyTracker dirty_tracker_;
    bool dragging_scrollbar_ = false;
    float scrollbar_drag_offset_ = 0.0f;
    bool pool_resize_in_progress_ = false;
    bool destroying_ = false;
    std::uint64_t window_generation_ = 0;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace pulp::view
