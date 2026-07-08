#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <typeindex>
#include <vector>

#include <pulp/view/view.hpp>
#include <pulp/view/view_pool.hpp>

namespace pulp::view {

class VirtualListDirtyTracker {
public:
    struct Rect {
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;

        bool intersects(const Rect& other) const {
            return x < other.x + other.w && x + w > other.x &&
                   y < other.y + other.h && y + h > other.y;
        }

        Rect merged(const Rect& other) const {
            const float nx = std::min(x, other.x);
            const float ny = std::min(y, other.y);
            return {
                nx,
                ny,
                std::max(x + w, other.x + other.w) - nx,
                std::max(y + h, other.y + other.h) - ny
            };
        }

        float area() const { return w * h; }
    };

    void invalidate(const Rect& rect) {
        if (rect.w <= 0.0f || rect.h <= 0.0f) return;
        dirty_rects_.push_back(rect);
        coalesce_if_needed();
    }

    void invalidate(float x, float y, float w, float h) {
        invalidate({x, y, w, h});
    }

    void invalidate_all() {
        full_repaint_ = true;
        dirty_rects_.clear();
    }

    bool is_dirty() const { return full_repaint_ || !dirty_rects_.empty(); }
    bool needs_full_repaint() const { return full_repaint_; }
    const std::vector<Rect>& dirty_rects() const { return dirty_rects_; }

    void clear() {
        dirty_rects_.clear();
        full_repaint_ = false;
        ++frame_count_;
    }

    Rect bounds() const {
        if (dirty_rects_.empty()) return {};
        Rect bounds = dirty_rects_[0];
        for (std::size_t i = 1; i < dirty_rects_.size(); ++i) {
            bounds = bounds.merged(dirty_rects_[i]);
        }
        return bounds;
    }

    void set_viewport(float width, float height, float full_repaint_threshold = 0.6f) {
        viewport_w_ = width;
        viewport_h_ = height;
        full_repaint_threshold_ = full_repaint_threshold;
    }

    std::uint64_t frame_count() const { return frame_count_; }

private:
    void coalesce_if_needed() {
        if (dirty_rects_.size() > max_rects_) coalesce();
        if (viewport_w_ <= 0.0f || viewport_h_ <= 0.0f) return;

        float total_area = 0.0f;
        for (const auto& rect : dirty_rects_) total_area += rect.area();
        if (total_area > viewport_w_ * viewport_h_ * full_repaint_threshold_) {
            invalidate_all();
        }
    }

    void coalesce() {
        bool merged = true;
        while (merged) {
            merged = false;
            for (std::size_t i = 0; i < dirty_rects_.size(); ++i) {
                for (std::size_t j = i + 1; j < dirty_rects_.size(); ++j) {
                    auto combined = dirty_rects_[i].merged(dirty_rects_[j]);
                    const float sum_area = dirty_rects_[i].area() + dirty_rects_[j].area();
                    if (combined.area() < sum_area * 1.5f ||
                        dirty_rects_[i].intersects(dirty_rects_[j])) {
                        dirty_rects_[i] = combined;
                        dirty_rects_.erase(dirty_rects_.begin() + static_cast<long>(j));
                        merged = true;
                        break;
                    }
                }
                if (merged) break;
            }
        }
    }

    std::vector<Rect> dirty_rects_;
    bool full_repaint_ = true;
    float viewport_w_ = 0.0f;
    float viewport_h_ = 0.0f;
    float full_repaint_threshold_ = 0.6f;
    std::uint64_t frame_count_ = 0;
    static constexpr std::size_t max_rects_ = 16;
};

/// Recycling virtualized list for large, scrolling, rich row views.
///
/// VirtualList keeps a bounded pool of row View objects sized to the visible
/// viewport plus overscan. As the scroll offset changes, existing rows are
/// re-bound to new data indices instead of creating one View per item.
class VirtualList : public View {
public:
    enum class SelectionMode { none, single, multi };

    using RowFactory = std::function<std::unique_ptr<View>(std::size_t slot)>;
    using RowBinder = std::function<void(View& row, std::size_t index)>;
    using RowReleaser = std::function<void(View& row)>;
    using TypeToSearchHandler =
        std::function<std::optional<std::size_t>(std::string_view query,
                                                 std::size_t from_index)>;

    VirtualList();
    ~VirtualList() override;

    void set_row_count(std::size_t n);
    std::size_t row_count() const { return row_count_; }

    void set_row_height(float px);
    float row_height() const { return row_height_; }

    void set_overscan(int rows);
    int overscan() const { return overscan_rows_; }

    void set_row_factory(RowFactory factory);
    void set_row_binder(RowBinder binder);
    void set_row_releaser(RowReleaser releaser);
    void refresh_rows();

    void set_selection_mode(SelectionMode mode);
    SelectionMode selection_mode() const { return selection_mode_; }
    const std::vector<std::size_t>& selection() const { return selection_; }
    bool is_selected(std::size_t index) const;
    void clear_selection();
    void select_row(std::size_t index);
    void toggle_row_selection(std::size_t index);

    std::optional<std::size_t> focused_index() const { return focused_index_; }
    void set_focused_index(std::size_t index);

    void scroll_to_row(std::size_t index);
    void set_scroll_y(float y);
    void scroll_by(float dy);
    float scroll_y() const { return scroll_y_; }
    float content_height() const;

    void on_row_activated(std::function<void(std::size_t)> cb) {
        on_row_activated_ = std::move(cb);
    }
    void on_selection_changed(std::function<void(const std::vector<std::size_t>&)> cb) {
        on_selection_changed_ = std::move(cb);
    }
    void set_type_to_search(TypeToSearchHandler handler) {
        type_to_search_ = std::move(handler);
    }
    void clear_type_to_search_buffer() { type_buffer_.clear(); }

    std::size_t realized_row_count() const { return row_slots_.size(); }
    View* realized_row_at_slot(std::size_t slot);
    const View* realized_row_at_slot(std::size_t slot) const;
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
    void on_text_input(const TextInputEvent& event) override;
    bool wants_wheel_scroll() const override;

private:
    enum class UpdateResult { unchanged, changed, interrupted };
    struct SelectionUpdate {
        bool changed = false;
        bool interrupted = false;
    };

    struct RowSlot {
        View* row = nullptr;
        std::optional<std::size_t> index;
    };

    std::size_t desired_pool_size() const;
    bool clear_pool();
    bool ensure_pool_size(std::size_t desired);
    bool release_row(RowSlot& slot);
    bool update_window(bool force_rebind);
    bool bind_slot(RowSlot& slot, std::size_t index, bool force_rebind,
                   std::uint64_t update_generation);
    void position_slot(RowSlot& slot);
    void update_row_accessibility(RowSlot& slot);
    float max_scroll_y() const;
    bool scroll_to_row_internal(std::size_t index);
    UpdateResult set_scroll_y_internal(float y, bool request);
    bool apply_pending_scroll_to_row();
    bool apply_pending_selected_row();
    void mark_scroll_dirty(float old_y, float new_y);
    float row_width() const;

    std::optional<std::size_t> row_at(Point local) const;
    void choose_row(std::size_t index, bool extend, bool toggle, bool claim_focus = false);
    SelectionUpdate set_selection_sanitized(std::vector<std::size_t> next, bool notify = true);
    void move_focus_to(std::size_t index, bool extend);
    std::optional<std::size_t> keyboard_anchor_index() const;
    std::size_t page_row_delta() const;

    bool scrollbar_visible() const;
    float scrollbar_width() const;
    float scrollbar_thumb_length() const;
    float scrollbar_thumb_y() const;
    bool point_in_scrollbar(Point local) const;
    bool point_in_scrollbar_thumb(Point local) const;

    std::size_t row_count_ = 0;
    float row_height_ = 24.0f;
    int overscan_rows_ = 3;
    float scroll_y_ = 0.0f;
    std::optional<std::size_t> pending_scroll_to_row_;
    std::optional<std::size_t> pending_selected_row_;

    RowFactory row_factory_;
    RowBinder row_binder_;
    RowReleaser row_releaser_;
    std::vector<RowSlot> row_slots_;
    std::size_t first_realized_index_ = 0;

    // Per-class recycling pool for released rows (planning 2.3). Rows that opt
    // in via View::supports_reuse() are parked here on release and re-acquired
    // (in place of a fresh factory() call) when the pool grows. Behavior-neutral
    // for the default View rows the built-in list uses: they never opt in, so
    // release() destroys them and acquire() always falls through to the factory.
    // `row_type_` remembers the concrete row type the factory produces so the
    // type-keyed pool can be queried before the type is otherwise known.
    ViewPool row_pool_;
    std::optional<std::type_index> row_type_;

    SelectionMode selection_mode_ = SelectionMode::single;
    std::vector<std::size_t> selection_;
    std::optional<std::size_t> focused_index_;
    std::optional<std::size_t> selection_anchor_;
    std::function<void(std::size_t)> on_row_activated_;
    std::function<void(const std::vector<std::size_t>&)> on_selection_changed_;
    TypeToSearchHandler type_to_search_;
    std::string type_buffer_;

    VirtualListDirtyTracker dirty_tracker_;
    bool dragging_scrollbar_ = false;
    float scrollbar_drag_offset_ = 0.0f;
    bool pool_resize_in_progress_ = false;
    bool destroying_ = false;
    std::uint64_t window_generation_ = 0;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace pulp::view
