#include <pulp/view/virtual_grid.hpp>

#include <pulp/view/ui_components.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

// NOTE (intentional duplication): VirtualGrid mirrors VirtualList's slot-
// windowing and reentrancy discipline (alive token, window generation, pool-
// resize guard, destroying flag) verbatim-shaped rather than sharing a base.
// Both now share the per-class recycling ViewPool (planning 2.3) — released
// cells/rows that opt in via View::supports_reuse() are parked and re-acquired
// instead of destroyed — but the windowing classes themselves stay independent
// so a hard-won VirtualList reentrancy fix and its VirtualGrid twin can be
// reviewed side-by-side, not hidden behind an early abstraction. See
// virtual_grid.hpp for the full rationale.

namespace pulp::view {

namespace {
constexpr float kMinCellSize = 1.0f;
constexpr float kScrollbarWidth = 6.0f;
constexpr float kScrollbarHitWidth = 12.0f;
constexpr float kScrollbarPad = 2.0f;
constexpr float kMinThumbLength = 20.0f;

std::string cell_value(std::size_t index, std::size_t count) {
    return "cell " + std::to_string(index + 1) + " of " + std::to_string(count);
}

bool is_interactive_target(View& view) {
    return view.on_click || view.on_pointer_event || view.on_drag ||
           view.on_context_menu || view.focusable() || view.wants_wheel_value() ||
           view.wants_mouse_input();
}

bool is_descendant_of(const View& ancestor, const View* view) {
    while (view != nullptr) {
        if (view == &ancestor) return true;
        view = view->parent();
    }
    return false;
}

void close_active_combo_popup_in_subtree(View& root) {
    auto* active = ComboBox::active_popup_;
    if (active != nullptr && is_descendant_of(root, active)) ComboBox::close_active_popup();
}
} // namespace

VirtualGrid::VirtualGrid() {
    set_focusable(true);
    set_access_role(AccessRole::group);
    set_overflow(Overflow::hidden);
}

VirtualGrid::~VirtualGrid() {
    destroying_ = true;
    (void)clear_pool();
    alive_->store(false, std::memory_order_release);
}

std::size_t VirtualGrid::columns() const {
    if (column_count_ > 0) return static_cast<std::size_t>(column_count_);
    const float w = local_bounds().width;
    if (cell_w_ <= 0.0f || w <= 0.0f) return 1;
    return std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(w / cell_w_)));
}

std::size_t VirtualGrid::total_rows() const {
    if (item_count_ == 0) return 0;
    const auto cols = columns();
    return (item_count_ + cols - 1) / cols;
}

void VirtualGrid::set_item_count(std::size_t n) {
    if (item_count_ == n) return;
    item_count_ = n;
    const auto selection_update = set_selection_sanitized(selection_, false);
    if (selection_update.interrupted) return;
    if (focused_index_ && *focused_index_ >= n) focused_index_.reset();
    if (selection_anchor_ && *selection_anchor_ >= n) selection_anchor_.reset();
    if (set_scroll_y_internal(scroll_y_, false) == UpdateResult::interrupted) return;
    dirty_tracker_.invalidate_all();
    if (!update_window(true)) return;
    if (!apply_pending_scroll_to_item()) return;
    if (!apply_pending_selected_cell()) return;
    request_repaint();
    if (selection_update.changed && on_selection_changed_) {
        auto selection = selection_;
        auto callback = on_selection_changed_;
        callback(selection);
    }
}

void VirtualGrid::set_cell_size(float width, float height) {
    width = std::max(kMinCellSize, width);
    height = std::max(kMinCellSize, height);
    if (cell_w_ == width && cell_h_ == height) return;
    cell_w_ = width;
    cell_h_ = height;
    if (set_scroll_y_internal(scroll_y_, false) == UpdateResult::interrupted) return;
    dirty_tracker_.invalidate_all();
    if (!update_window(true)) return;
    if (!apply_pending_scroll_to_item()) return;
    request_repaint();
}

void VirtualGrid::set_column_count(int columns) {
    columns = std::max(0, columns);
    if (column_count_ == columns) return;
    column_count_ = columns;
    if (set_scroll_y_internal(scroll_y_, false) == UpdateResult::interrupted) return;
    dirty_tracker_.invalidate_all();
    if (!update_window(true)) return;
    if (!apply_pending_scroll_to_item()) return;
    request_repaint();
}

void VirtualGrid::set_overscan(int rows) {
    rows = std::max(0, rows);
    if (overscan_rows_ == rows) return;
    overscan_rows_ = rows;
    dirty_tracker_.invalidate_all();
    if (!update_window(false)) return;
    request_repaint();
}

void VirtualGrid::set_cell_factory(CellFactory factory) {
    if (!clear_pool()) return;
    // Parked cells belong to the OLD factory — drop the recycle pool and forget
    // the tracked cell type so a new factory's cells are never bound onto stale
    // recycled instances.
    cell_pool_.clear();
    cell_type_.reset();
    cell_factory_ = std::move(factory);
    dirty_tracker_.invalidate_all();
    if (!update_window(true)) return;
    request_repaint();
}

void VirtualGrid::set_cell_binder(CellBinder binder) {
    cell_binder_ = std::move(binder);
    if (!update_window(true)) return;
    request_repaint();
}

void VirtualGrid::set_cell_releaser(CellReleaser releaser) {
    cell_releaser_ = std::move(releaser);
}

void VirtualGrid::refresh_cells() {
    if (!update_window(true)) return;
    request_repaint();
}

void VirtualGrid::set_selection_mode(SelectionMode mode) {
    if (selection_mode_ == mode) return;
    selection_mode_ = mode;
    SelectionUpdate selection_update;
    if (selection_mode_ == SelectionMode::none) {
        selection_update = set_selection_sanitized({}, false);
    } else if (selection_mode_ == SelectionMode::single && selection_.size() > 1) {
        selection_update = set_selection_sanitized({selection_.back()}, false);
    }
    if (selection_update.interrupted) return;
    for (auto& slot : cell_slots_) update_cell_accessibility(slot);
    request_repaint();
    if (selection_update.changed && on_selection_changed_) {
        auto selection = selection_;
        auto callback = on_selection_changed_;
        callback(selection);
    }
}

bool VirtualGrid::is_selected(std::size_t index) const {
    return std::binary_search(selection_.begin(), selection_.end(), index);
}

void VirtualGrid::clear_selection() {
    pending_selected_cell_.reset();
    set_selection_sanitized({});
}

void VirtualGrid::select_cell(std::size_t index) {
    if (item_count_ == 0 || index >= item_count_) {
        pending_selected_cell_ = index;
        set_selection_sanitized({});
        return;
    }
    pending_selected_cell_.reset();
    choose_cell(index, false, false);
}

void VirtualGrid::toggle_cell_selection(std::size_t index) {
    choose_cell(index, false, true);
}

void VirtualGrid::set_focused_index(std::size_t index) {
    if (item_count_ == 0) {
        focused_index_.reset();
        return;
    }
    focused_index_ = std::min(index, item_count_ - 1);
    if (!scroll_to_item_internal(*focused_index_)) return;
    for (auto& slot : cell_slots_) update_cell_accessibility(slot);
}

float VirtualGrid::content_height() const {
    return static_cast<float>(total_rows()) * cell_h_;
}

float VirtualGrid::max_scroll_y() const {
    return std::max(0.0f, content_height() - local_bounds().height);
}

void VirtualGrid::scroll_to_item(std::size_t index) {
    (void)scroll_to_item_internal(index);
}

bool VirtualGrid::scroll_to_item_internal(std::size_t index) {
    if (item_count_ == 0 || local_bounds().height <= 0.0f || index >= item_count_) {
        pending_scroll_to_item_ = index;
        return true;
    }
    pending_scroll_to_item_.reset();
    index = std::min(index, item_count_ - 1);
    const auto cols = columns();
    const float top = static_cast<float>(index / cols) * cell_h_;
    const float bottom = top + cell_h_;
    const float view_h = local_bounds().height;

    if (top < scroll_y_) {
        return set_scroll_y_internal(top, true) != UpdateResult::interrupted;
    } else if (bottom > scroll_y_ + view_h) {
        return set_scroll_y_internal(bottom - view_h, true) != UpdateResult::interrupted;
    } else {
        return update_window(false);
    }
}

void VirtualGrid::set_scroll_y(float y) {
    (void)set_scroll_y_internal(y, true);
}

void VirtualGrid::scroll_by(float dy) {
    set_scroll_y(scroll_y_ + dy);
}

View* VirtualGrid::realized_cell_at_slot(std::size_t slot) {
    return slot < cell_slots_.size() ? cell_slots_[slot].cell : nullptr;
}

const View* VirtualGrid::realized_cell_at_slot(std::size_t slot) const {
    return slot < cell_slots_.size() ? cell_slots_[slot].cell : nullptr;
}

std::optional<std::size_t> VirtualGrid::bound_index_for_slot(std::size_t slot) const {
    if (slot >= cell_slots_.size()) return std::nullopt;
    return cell_slots_[slot].index;
}

std::vector<std::size_t> VirtualGrid::realized_indices() const {
    std::vector<std::size_t> out;
    out.reserve(cell_slots_.size());
    for (const auto& slot : cell_slots_) {
        if (slot.index) out.push_back(*slot.index);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void VirtualGrid::clear_dirty() {
    dirty_tracker_.clear();
}

void VirtualGrid::on_resized() {
    dirty_tracker_.set_viewport(local_bounds().width, local_bounds().height);
    if (set_scroll_y_internal(scroll_y_, false) == UpdateResult::interrupted) return;
    dirty_tracker_.invalidate_all();
    // Auto-derived column counts change with width, so force a rebind: existing
    // slots may map to different item indices after the column count shifts.
    if (!update_window(true)) return;
    if (!apply_pending_scroll_to_item()) return;
}

void VirtualGrid::layout_children() {
    if (!update_window(false)) return;
    for (auto& slot : cell_slots_) {
        if (slot.cell && slot.index) slot.cell->layout_children();
    }
    clear_layout_dirty();
}

void VirtualGrid::paint(canvas::Canvas& canvas) {
    if (!scrollbar_visible()) return;
    const auto b = local_bounds();
    const float width = scrollbar_width();
    const float thumb_h = scrollbar_thumb_length();
    const float thumb_y = scrollbar_thumb_y();

    canvas.set_fill_color(
        resolve_color("control.track", canvas::Color::rgba8(255, 255, 255, 28)));
    canvas.fill_rounded_rect(b.width - width - kScrollbarPad, kScrollbarPad,
                             width, std::max(0.0f, b.height - 2.0f * kScrollbarPad),
                             width * 0.5f);
    canvas.set_fill_color(
        resolve_color("control.thumb", canvas::Color::rgba8(255, 255, 255, 96)));
    canvas.fill_rounded_rect(b.width - width - kScrollbarPad, thumb_y,
                             width, thumb_h, width * 0.5f);
}

void VirtualGrid::on_mouse_event(const MouseEvent& event) {
    if (event.is_wheel) {
        scroll_by(event.scroll_delta_y);
        return;
    }

    if (!event.is_down) {
        dragging_scrollbar_ = false;
        return;
    }

    if (event.button != MouseButton::left) return;

    if (point_in_scrollbar(event.position)) {
        if (point_in_scrollbar_thumb(event.position)) {
            dragging_scrollbar_ = true;
            scrollbar_drag_offset_ = event.position.y - scrollbar_thumb_y();
        } else {
            const float thumb_y = scrollbar_thumb_y();
            scroll_by(event.position.y < thumb_y ? -local_bounds().height
                                                 : local_bounds().height);
        }
        return;
    }

    auto index = cell_at(event.position);
    if (!index) return;
    auto alive = alive_;
    choose_cell(*index, event.isShiftDown(),
                event.isMainModifier() || event.isCtrlDown(), true);
    if (!alive->load(std::memory_order_acquire)) return;
    if (event.click_count >= 2 && on_cell_activated_) {
        auto activate = on_cell_activated_;
        activate(*index);
    }
}

void VirtualGrid::on_mouse_drag(Point pos) {
    if (!dragging_scrollbar_ || !scrollbar_visible()) return;
    const auto b = local_bounds();
    const float thumb_h = scrollbar_thumb_length();
    const float track_h = std::max(1.0f, b.height - 2.0f * kScrollbarPad - thumb_h);
    const float local_y = pos.y - scrollbar_drag_offset_ - kScrollbarPad;
    const float t = std::clamp(local_y / track_h, 0.0f, 1.0f);
    set_scroll_y(t * max_scroll_y());
}

bool VirtualGrid::on_key_event(const KeyEvent& event) {
    if (!event.is_down || item_count_ == 0) return false;

    const auto cols = columns();
    const auto anchor = keyboard_anchor_index();
    const bool has_anchor = anchor.has_value();
    const std::size_t current = anchor.value_or(0);
    std::size_t next = current;

    switch (event.key) {
        case KeyCode::left:
            next = current > 0 ? current - 1 : 0;
            break;
        case KeyCode::right:
            next = has_anchor ? std::min(item_count_ - 1, current + 1) : 0;
            break;
        case KeyCode::up:
            next = (has_anchor && current >= cols) ? current - cols : current;
            if (!has_anchor) next = 0;
            break;
        case KeyCode::down:
            next = has_anchor ? std::min(item_count_ - 1, current + cols) : 0;
            break;
        case KeyCode::page_up: {
            const auto delta = page_cell_delta();
            next = current > delta ? current - delta : 0;
            break;
        }
        case KeyCode::page_down:
            next = std::min(item_count_ - 1, current + page_cell_delta());
            break;
        case KeyCode::home:
            next = 0;
            break;
        case KeyCode::end_:
            next = item_count_ - 1;
            break;
        case KeyCode::enter:
            if (focused_index_ && on_cell_activated_) {
                const auto index = *focused_index_;
                auto activate = on_cell_activated_;
                activate(index);
                return true;
            }
            return false;
        default:
            return false;
    }

    move_focus_to(next, event.isShiftDown());
    return true;
}

View* VirtualGrid::hit_test(Point local_point) {
    auto* hit = View::hit_test(local_point);
    if (hit == nullptr || hit == this) return hit;

    View* interactive = nullptr;
    for (auto* v = hit; v != nullptr && v != this; v = v->parent()) {
        if (is_interactive_target(*v)) {
            interactive = v;
            break;
        }
    }
    if (interactive != nullptr) return interactive;
    return pointer_events() == PointerEvents::box_none ? nullptr : this;
}

bool VirtualGrid::wants_wheel_scroll() const {
    return scrollbar_visible();
}

std::size_t VirtualGrid::visible_rows() const {
    const auto b = local_bounds();
    if (cell_h_ <= 0.0f || b.height <= 0.0f) return 0;
    return std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(b.height / cell_h_)));
}

std::size_t VirtualGrid::desired_pool_size() const {
    const auto b = local_bounds();
    if (item_count_ == 0 || cell_h_ <= 0.0f || b.height <= 0.0f) return 0;
    const auto cols = columns();
    const auto pool_rows =
        visible_rows() + 1U + static_cast<std::size_t>(std::max(0, overscan_rows_)) * 2U;
    return std::min(item_count_, pool_rows * cols);
}

bool VirtualGrid::clear_pool() {
    if (pool_resize_in_progress_) return false;
    auto alive = alive_;
    pool_resize_in_progress_ = true;
    auto finish = [&](bool result) {
        if (!alive->load(std::memory_order_acquire)) return false;
        pool_resize_in_progress_ = false;
        return result;
    };

    while (!cell_slots_.empty()) {
        if (!release_cell(cell_slots_.back())) return finish(false);
        cell_slots_.pop_back();
    }
    first_realized_index_ = 0;
    return finish(true);
}

bool VirtualGrid::ensure_pool_size(std::size_t desired) {
    if (pool_resize_in_progress_) return false;
    auto alive = alive_;
    pool_resize_in_progress_ = true;
    auto finish = [&](bool result) {
        if (!alive->load(std::memory_order_acquire)) return false;
        pool_resize_in_progress_ = false;
        return result;
    };

    while (cell_slots_.size() > desired) {
        if (!release_cell(cell_slots_.back())) return finish(false);
        cell_slots_.pop_back();
    }

    while (cell_slots_.size() < desired) {
        const std::size_t slot = cell_slots_.size();
        const auto generation = window_generation_;
        // Prefer a recycled cell of the known cell type before building a fresh
        // one. The pool pop is side-effect-free (no user callback), so it needs
        // no alive/generation re-check; falls through to the factory when the
        // pool is empty or the cell type never opts in.
        std::unique_ptr<View> cell;
        if (cell_type_) cell = cell_pool_.acquire(*cell_type_);
        if (!cell) {
            auto factory = cell_factory_;
            cell = factory ? factory(slot) : std::make_unique<View>();
            if (!alive->load(std::memory_order_acquire)) return false;
            if (!destroying_ && generation != window_generation_) return finish(false);
            if (!cell) cell = std::make_unique<View>();
            cell_type_ = std::type_index(typeid(*cell));
        }
        cell->set_overflow(Overflow::hidden);
        cell->set_position(View::Position::absolute);
        cell->set_left(0.0f);
        cell->set_top(0.0f);
        auto* ptr = cell.get();
        add_child(std::move(cell));
        cell_slots_.push_back({ptr, std::nullopt});
    }
    return finish(true);
}

bool VirtualGrid::release_cell(CellSlot& slot) {
    if (!slot.cell) return true;
    auto* cell = slot.cell;
    auto removed = remove_child(cell);
    slot.cell = nullptr;
    slot.index.reset();
    auto releaser = cell_releaser_;
    if (removed && releaser) {
        auto alive = alive_;
        const auto generation = window_generation_;
        releaser(*removed);
        if (!alive->load(std::memory_order_acquire)) return false;
        if (!destroying_ && generation != window_generation_) return false;
    }
    // Park the detached cell for recycling. release() keeps it only when the
    // cell type opts in (View::supports_reuse()); otherwise destroys it here, so
    // the default-View cells behave exactly as before.
    if (removed) cell_pool_.release(std::move(removed));
    return true;
}

bool VirtualGrid::update_window(bool force_rebind) {
    if (pool_resize_in_progress_) return true;
    const auto update_generation = ++window_generation_;
    const auto cols = columns();
    const std::size_t desired = desired_pool_size();
    if (!ensure_pool_size(desired)) return false;
    if (desired == 0) {
        first_realized_index_ = 0;
        return true;
    }

    const std::size_t visible_first_row =
        static_cast<std::size_t>(std::max(0.0f, std::floor(scroll_y_ / cell_h_)));
    const std::size_t before = static_cast<std::size_t>(std::max(0, overscan_rows_));
    const std::size_t pool_rows = (desired + cols - 1) / cols;
    const std::size_t total = total_rows();
    const std::size_t max_first_row = total > pool_rows ? total - pool_rows : 0;
    const std::size_t first_row =
        std::min(max_first_row, visible_first_row > before ? visible_first_row - before : 0);
    first_realized_index_ = first_row * cols;
    const std::size_t last = std::min(item_count_, first_realized_index_ + desired);

    for (std::size_t slot_index = 0; slot_index < cell_slots_.size(); ++slot_index) {
        const auto index = first_realized_index_ + slot_index;
        if (index >= last) break;
        auto& slot = cell_slots_[slot_index];
        if (!bind_slot(slot, index, force_rebind, update_generation)) return false;
        position_slot(slot);
    }
    return true;
}

bool VirtualGrid::bind_slot(CellSlot& slot, std::size_t index, bool force_rebind,
                            std::uint64_t update_generation) {
    if (!slot.cell) return true;
    const bool changed = !slot.index || *slot.index != index;
    slot.index = index;
    if (changed || force_rebind) {
        slot.cell->set_access_label("");
        slot.cell->set_access_value("");
    }
    auto binder = cell_binder_;
    if ((changed || force_rebind) && binder) {
        auto* cell = slot.cell;
        auto alive = alive_;
        binder(*cell, index);
        if (!alive->load(std::memory_order_acquire)) return false;
        if (update_generation != window_generation_) return false;
        if (slot.cell != cell || !slot.index || *slot.index != index) return false;
    }
    update_cell_accessibility(slot);
    return true;
}

void VirtualGrid::position_slot(CellSlot& slot) {
    if (!slot.cell || !slot.index) return;
    const auto cols = columns();
    const std::size_t col = *slot.index % cols;
    const std::size_t row = *slot.index / cols;
    const float x = static_cast<float>(col) * cell_w_;
    const float y = static_cast<float>(row) * cell_h_ - scroll_y_;
    auto& flex = slot.cell->flex();
    flex.preferred_width = cell_w_;
    flex.preferred_height = cell_h_;
    slot.cell->set_left(x);
    slot.cell->set_top(y);
    slot.cell->set_bounds({x, y, cell_w_, cell_h_});
}

void VirtualGrid::update_cell_accessibility(CellSlot& slot) {
    if (!slot.cell || !slot.index) return;
    const auto position = cell_value(*slot.index, item_count_);
    slot.cell->set_access_role(AccessRole::group);
    if (slot.cell->access_label().empty()) {
        slot.cell->set_access_label(position);
    }
    slot.cell->set_access_value(position);
    if (focused_index_ && *focused_index_ == *slot.index) {
        slot.cell->set_focus(true);
    } else {
        slot.cell->set_focus(false);
    }
    if (selection_mode_ == SelectionMode::none) {
        slot.cell->set_access_checked("");
    } else {
        slot.cell->set_access_checked(is_selected(*slot.index) ? "true" : "false");
    }
}

VirtualGrid::UpdateResult VirtualGrid::set_scroll_y_internal(float y, bool request) {
    const float next = std::clamp(y, 0.0f, max_scroll_y());
    if (next == scroll_y_) return UpdateResult::unchanged;
    close_active_combo_popup_in_subtree(*this);
    const float old = scroll_y_;
    scroll_y_ = next;
    mark_scroll_dirty(old, next);
    if (!update_window(false)) return UpdateResult::interrupted;
    if (request) request_repaint();
    return UpdateResult::changed;
}

bool VirtualGrid::apply_pending_scroll_to_item() {
    if (!pending_scroll_to_item_) return true;
    const auto index = *pending_scroll_to_item_;
    pending_scroll_to_item_.reset();
    auto alive = alive_;
    return scroll_to_item_internal(index) && alive->load(std::memory_order_acquire);
}

bool VirtualGrid::apply_pending_selected_cell() {
    if (!pending_selected_cell_ || item_count_ == 0) return true;
    const auto index = *pending_selected_cell_;
    if (index >= item_count_) return true;
    pending_selected_cell_.reset();
    auto alive = alive_;
    choose_cell(index, false, false);
    return alive->load(std::memory_order_acquire);
}

void VirtualGrid::mark_scroll_dirty(float old_y, float new_y) {
    const auto b = local_bounds();
    dirty_tracker_.set_viewport(b.width, b.height);
    const float delta = new_y - old_y;
    const float strip = std::min(std::abs(delta), b.height);
    if (strip <= 0.0f || strip >= b.height) {
        dirty_tracker_.invalidate_all();
    } else if (delta > 0.0f) {
        dirty_tracker_.invalidate(0.0f, b.height - strip, b.width, strip);
    } else {
        dirty_tracker_.invalidate(0.0f, 0.0f, b.width, strip);
    }
}

float VirtualGrid::grid_width() const {
    const auto b = local_bounds();
    const float gutter = scrollbar_visible() ? (scrollbar_width() + 2.0f * kScrollbarPad) : 0.0f;
    return std::max(0.0f, b.width - gutter);
}

std::optional<std::size_t> VirtualGrid::cell_at(Point local) const {
    const auto b = local_bounds();
    if (cell_w_ <= 0.0f || cell_h_ <= 0.0f || !b.contains(local)) return std::nullopt;
    if (scrollbar_visible() && local.x >= grid_width()) return std::nullopt;
    if (local.x < 0.0f) return std::nullopt;
    const auto cols = columns();
    const auto col = static_cast<std::size_t>(local.x / cell_w_);
    if (col >= cols) return std::nullopt;
    const float y = local.y + scroll_y_;
    if (y < 0.0f) return std::nullopt;
    const auto row = static_cast<std::size_t>(y / cell_h_);
    const std::size_t index = row * cols + col;
    if (index >= item_count_) return std::nullopt;
    return index;
}

void VirtualGrid::choose_cell(std::size_t index, bool extend, bool toggle, bool claim_focus) {
    if (item_count_ == 0 || index >= item_count_) return;
    focused_index_ = index;
    if (claim_focus) {
        on_focus_changed(true);
        claim_input_focus();
    }

    if (selection_mode_ == SelectionMode::none) {
        for (auto& slot : cell_slots_) update_cell_accessibility(slot);
        request_repaint();
        return;
    }

    if (selection_mode_ == SelectionMode::single) {
        selection_anchor_ = index;
        set_selection_sanitized({index});
        return;
    }

    if (extend && selection_anchor_) {
        const auto a = std::min(*selection_anchor_, index);
        const auto b = std::max(*selection_anchor_, index);
        std::vector<std::size_t> next;
        next.reserve(b - a + 1);
        for (std::size_t i = a; i <= b; ++i) next.push_back(i);
        set_selection_sanitized(std::move(next));
    } else if (toggle) {
        auto next = selection_;
        auto it = std::lower_bound(next.begin(), next.end(), index);
        if (it != next.end() && *it == index)
            next.erase(it);
        else
            next.insert(it, index);
        selection_anchor_ = index;
        set_selection_sanitized(std::move(next));
    } else {
        selection_anchor_ = index;
        set_selection_sanitized({index});
    }
}

VirtualGrid::SelectionUpdate VirtualGrid::set_selection_sanitized(std::vector<std::size_t> next,
                                                                  bool notify) {
    next.erase(std::remove_if(next.begin(), next.end(),
                              [this](std::size_t i) { return i >= item_count_; }),
               next.end());
    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());
    if (selection_mode_ == SelectionMode::single && next.size() > 1) {
        next.erase(next.begin(), next.end() - 1);
    }
    if (selection_mode_ == SelectionMode::none) next.clear();
    if (next == selection_) {
        for (auto& slot : cell_slots_) update_cell_accessibility(slot);
        return {};
    }
    selection_ = std::move(next);
    if (!update_window(true)) return {true, true};
    request_repaint();
    if (notify && on_selection_changed_) {
        auto selection = selection_;
        auto callback = on_selection_changed_;
        callback(selection);
    }
    return {true, false};
}

void VirtualGrid::move_focus_to(std::size_t index, bool extend) {
    if (item_count_ == 0) return;
    index = std::min(index, item_count_ - 1);
    focused_index_ = index;
    if (!scroll_to_item_internal(index)) return;
    choose_cell(index, extend, false);
}

std::optional<std::size_t> VirtualGrid::keyboard_anchor_index() const {
    if (focused_index_ && *focused_index_ < item_count_) return *focused_index_;
    if (!selection_.empty() && selection_.back() < item_count_) return selection_.back();
    return std::nullopt;
}

std::size_t VirtualGrid::page_cell_delta() const {
    const auto rows = visible_rows();
    return std::max<std::size_t>(1, rows * columns());
}

bool VirtualGrid::scrollbar_visible() const {
    return content_height() > local_bounds().height;
}

float VirtualGrid::scrollbar_width() const {
    return kScrollbarWidth;
}

float VirtualGrid::scrollbar_thumb_length() const {
    const auto b = local_bounds();
    if (!scrollbar_visible()) return 0.0f;
    const float track_h = std::max(0.0f, b.height - 2.0f * kScrollbarPad);
    const float ratio = b.height / std::max(b.height, content_height());
    return std::min(track_h, std::max(kMinThumbLength, track_h * ratio));
}

float VirtualGrid::scrollbar_thumb_y() const {
    const auto b = local_bounds();
    const float thumb_h = scrollbar_thumb_length();
    const float track_h = std::max(1.0f, b.height - 2.0f * kScrollbarPad - thumb_h);
    const float max_scroll = max_scroll_y();
    const float t = max_scroll > 0.0f ? scroll_y_ / max_scroll : 0.0f;
    return kScrollbarPad + t * track_h;
}

bool VirtualGrid::point_in_scrollbar(Point local) const {
    const auto b = local_bounds();
    return scrollbar_visible() && local.x >= b.width - kScrollbarHitWidth &&
           local.x <= b.width && local.y >= 0.0f && local.y <= b.height;
}

bool VirtualGrid::point_in_scrollbar_thumb(Point local) const {
    if (!point_in_scrollbar(local)) return false;
    const float y = scrollbar_thumb_y();
    return local.y >= y && local.y <= y + scrollbar_thumb_length();
}

} // namespace pulp::view
