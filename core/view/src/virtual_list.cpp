#include <pulp/view/virtual_list.hpp>

#include <pulp/view/ui_components.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace pulp::view {

namespace {
constexpr float kMinRowHeight = 1.0f;
constexpr float kScrollbarWidth = 6.0f;
constexpr float kScrollbarHitWidth = 12.0f;
constexpr float kScrollbarPad = 2.0f;
constexpr float kMinThumbLength = 20.0f;

std::string row_value(std::size_t index, std::size_t count) {
    return "row " + std::to_string(index + 1) + " of " + std::to_string(count);
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

VirtualList::VirtualList() {
    set_focusable(true);
    set_access_role(AccessRole::group);
    set_overflow(Overflow::hidden);
}

VirtualList::~VirtualList() {
    destroying_ = true;
    (void)clear_pool();
    alive_->store(false, std::memory_order_release);
}

void VirtualList::set_row_count(std::size_t n) {
    if (row_count_ == n) return;
    row_count_ = n;
    const auto selection_update = set_selection_sanitized(selection_, false);
    if (selection_update.interrupted) return;
    if (focused_index_ && *focused_index_ >= n) focused_index_.reset();
    if (selection_anchor_ && *selection_anchor_ >= n) selection_anchor_.reset();
    if (set_scroll_y_internal(scroll_y_, false) == UpdateResult::interrupted) return;
    dirty_tracker_.invalidate_all();
    if (!update_window(true)) return;
    if (!apply_pending_scroll_to_row()) return;
    if (!apply_pending_selected_row()) return;
    request_repaint();
    if (selection_update.changed && on_selection_changed_) {
        auto selection = selection_;
        auto callback = on_selection_changed_;
        callback(selection);
    }
}

void VirtualList::set_row_height(float px) {
    px = std::max(kMinRowHeight, px);
    if (row_height_ == px) return;
    row_height_ = px;
    if (set_scroll_y_internal(scroll_y_, false) == UpdateResult::interrupted) return;
    dirty_tracker_.invalidate_all();
    if (!update_window(false)) return;
    if (!apply_pending_scroll_to_row()) return;
    request_repaint();
}

void VirtualList::set_overscan(int rows) {
    rows = std::max(0, rows);
    if (overscan_rows_ == rows) return;
    overscan_rows_ = rows;
    dirty_tracker_.invalidate_all();
    if (!update_window(false)) return;
    request_repaint();
}

void VirtualList::set_row_factory(RowFactory factory) {
    if (!clear_pool()) return;
    row_factory_ = std::move(factory);
    dirty_tracker_.invalidate_all();
    if (!update_window(true)) return;
    request_repaint();
}

void VirtualList::set_row_binder(RowBinder binder) {
    row_binder_ = std::move(binder);
    if (!update_window(true)) return;
    request_repaint();
}

void VirtualList::set_row_releaser(RowReleaser releaser) {
    row_releaser_ = std::move(releaser);
}

void VirtualList::refresh_rows() {
    if (!update_window(true)) return;
    request_repaint();
}

void VirtualList::set_selection_mode(SelectionMode mode) {
    if (selection_mode_ == mode) return;
    selection_mode_ = mode;
    SelectionUpdate selection_update;
    if (selection_mode_ == SelectionMode::none) {
        selection_update = set_selection_sanitized({}, false);
    } else if (selection_mode_ == SelectionMode::single && selection_.size() > 1) {
        selection_update = set_selection_sanitized({selection_.back()}, false);
    }
    if (selection_update.interrupted) return;
    for (auto& slot : row_slots_) update_row_accessibility(slot);
    request_repaint();
    if (selection_update.changed && on_selection_changed_) {
        auto selection = selection_;
        auto callback = on_selection_changed_;
        callback(selection);
    }
}

bool VirtualList::is_selected(std::size_t index) const {
    return std::binary_search(selection_.begin(), selection_.end(), index);
}

void VirtualList::clear_selection() {
    pending_selected_row_.reset();
    set_selection_sanitized({});
}

void VirtualList::select_row(std::size_t index) {
    if (row_count_ == 0 || index >= row_count_) {
        pending_selected_row_ = index;
        set_selection_sanitized({});
        return;
    }
    pending_selected_row_.reset();
    choose_row(index, false, false);
}

void VirtualList::toggle_row_selection(std::size_t index) {
    choose_row(index, false, true);
}

void VirtualList::set_focused_index(std::size_t index) {
    if (row_count_ == 0) {
        focused_index_.reset();
        return;
    }
    focused_index_ = std::min(index, row_count_ - 1);
    if (!scroll_to_row_internal(*focused_index_)) return;
    for (auto& slot : row_slots_) update_row_accessibility(slot);
}

float VirtualList::content_height() const {
    return static_cast<float>(row_count_) * row_height_;
}

float VirtualList::max_scroll_y() const {
    return std::max(0.0f, content_height() - local_bounds().height);
}

void VirtualList::scroll_to_row(std::size_t index) {
    (void)scroll_to_row_internal(index);
}

bool VirtualList::scroll_to_row_internal(std::size_t index) {
    if (row_count_ == 0 || local_bounds().height <= 0.0f || index >= row_count_) {
        pending_scroll_to_row_ = index;
        return true;
    }
    pending_scroll_to_row_.reset();
    index = std::min(index, row_count_ - 1);
    const float top = static_cast<float>(index) * row_height_;
    const float bottom = top + row_height_;
    const float view_h = local_bounds().height;

    if (top < scroll_y_) {
        return set_scroll_y_internal(top, true) != UpdateResult::interrupted;
    } else if (bottom > scroll_y_ + view_h) {
        return set_scroll_y_internal(bottom - view_h, true) != UpdateResult::interrupted;
    } else {
        return update_window(false);
    }
    return true;
}

void VirtualList::set_scroll_y(float y) {
    (void)set_scroll_y_internal(y, true);
}

void VirtualList::scroll_by(float dy) {
    set_scroll_y(scroll_y_ + dy);
}

View* VirtualList::realized_row_at_slot(std::size_t slot) {
    return slot < row_slots_.size() ? row_slots_[slot].row : nullptr;
}

const View* VirtualList::realized_row_at_slot(std::size_t slot) const {
    return slot < row_slots_.size() ? row_slots_[slot].row : nullptr;
}

std::optional<std::size_t> VirtualList::bound_index_for_slot(std::size_t slot) const {
    if (slot >= row_slots_.size()) return std::nullopt;
    return row_slots_[slot].index;
}

std::vector<std::size_t> VirtualList::realized_indices() const {
    std::vector<std::size_t> out;
    out.reserve(row_slots_.size());
    for (const auto& slot : row_slots_) {
        if (slot.index) out.push_back(*slot.index);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void VirtualList::clear_dirty() {
    dirty_tracker_.clear();
}

void VirtualList::on_resized() {
    dirty_tracker_.set_viewport(local_bounds().width, local_bounds().height);
    if (set_scroll_y_internal(scroll_y_, false) == UpdateResult::interrupted) return;
    dirty_tracker_.invalidate_all();
    if (!update_window(false)) return;
    if (!apply_pending_scroll_to_row()) return;
}

void VirtualList::layout_children() {
    if (!update_window(false)) return;
    for (auto& slot : row_slots_) {
        if (slot.row && slot.index) slot.row->layout_children();
    }
    clear_layout_dirty();
}

void VirtualList::paint(canvas::Canvas& canvas) {
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

void VirtualList::on_mouse_event(const MouseEvent& event) {
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

    auto index = row_at(event.position);
    if (!index) return;
    auto alive = alive_;
    choose_row(*index, event.isShiftDown(),
               event.isMainModifier() || event.isCtrlDown(), true);
    if (!alive->load(std::memory_order_acquire)) return;
    if (event.click_count >= 2 && on_row_activated_) {
        auto activate = on_row_activated_;
        activate(*index);
    }
}

void VirtualList::on_mouse_drag(Point pos) {
    if (!dragging_scrollbar_ || !scrollbar_visible()) return;
    const auto b = local_bounds();
    const float thumb_h = scrollbar_thumb_length();
    const float track_h = std::max(1.0f, b.height - 2.0f * kScrollbarPad - thumb_h);
    const float local_y = pos.y - scrollbar_drag_offset_ - kScrollbarPad;
    const float t = std::clamp(local_y / track_h, 0.0f, 1.0f);
    set_scroll_y(t * max_scroll_y());
}

bool VirtualList::on_key_event(const KeyEvent& event) {
    if (!event.is_down || row_count_ == 0) return false;

    const auto anchor = keyboard_anchor_index();
    const bool has_anchor = anchor.has_value();
    const std::size_t current = anchor.value_or(0);
    std::size_t next = current;

    switch (event.key) {
        case KeyCode::up:
            next = current > 0 ? current - 1 : 0;
            break;
        case KeyCode::down:
            next = has_anchor ? std::min(row_count_ - 1, current + 1) : 0;
            break;
        case KeyCode::page_up: {
            const auto delta = page_row_delta();
            next = current > delta ? current - delta : 0;
            break;
        }
        case KeyCode::page_down:
            next = std::min(row_count_ - 1, current + page_row_delta());
            break;
        case KeyCode::home:
            next = 0;
            break;
        case KeyCode::end_:
            next = row_count_ - 1;
            break;
        case KeyCode::enter:
            if (focused_index_ && on_row_activated_) {
                const auto index = *focused_index_;
                auto activate = on_row_activated_;
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

void VirtualList::on_text_input(const TextInputEvent& event) {
    if (!type_to_search_ || event.text.empty() || row_count_ == 0) return;
    type_buffer_ += event.text;
    const std::size_t from = focused_index_ ? std::min(row_count_ - 1, *focused_index_ + 1) : 0;
    auto search = type_to_search_;
    auto alive = alive_;
    auto match = search(type_buffer_, from);
    if (!alive->load(std::memory_order_acquire)) return;
    if (!match || *match >= row_count_) return;
    move_focus_to(*match, false);
}

View* VirtualList::hit_test(Point local_point) {
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

bool VirtualList::wants_wheel_scroll() const {
    return scrollbar_visible();
}

std::size_t VirtualList::desired_pool_size() const {
    const auto b = local_bounds();
    if (row_count_ == 0 || row_height_ <= 0.0f || b.height <= 0.0f) return 0;
    const auto visible_rows =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(b.height / row_height_)) + 1);
    const auto overscan = static_cast<std::size_t>(std::max(0, overscan_rows_)) * 2U;
    return std::min(row_count_, visible_rows + overscan);
}

bool VirtualList::clear_pool() {
    if (pool_resize_in_progress_) return false;
    auto alive = alive_;
    pool_resize_in_progress_ = true;
    auto finish = [&](bool result) {
        if (!alive->load(std::memory_order_acquire)) return false;
        pool_resize_in_progress_ = false;
        return result;
    };

    while (!row_slots_.empty()) {
        if (!release_row(row_slots_.back())) return finish(false);
        row_slots_.pop_back();
    }
    first_realized_index_ = 0;
    return finish(true);
}

bool VirtualList::ensure_pool_size(std::size_t desired) {
    if (pool_resize_in_progress_) return false;
    auto alive = alive_;
    pool_resize_in_progress_ = true;
    auto finish = [&](bool result) {
        if (!alive->load(std::memory_order_acquire)) return false;
        pool_resize_in_progress_ = false;
        return result;
    };

    while (row_slots_.size() > desired) {
        if (!release_row(row_slots_.back())) return finish(false);
        row_slots_.pop_back();
    }

    while (row_slots_.size() < desired) {
        const std::size_t slot = row_slots_.size();
        const auto generation = window_generation_;
        auto factory = row_factory_;
        auto row = factory ? factory(slot) : std::make_unique<View>();
        if (!alive->load(std::memory_order_acquire)) return false;
        if (!destroying_ && generation != window_generation_) return finish(false);
        if (!row) row = std::make_unique<View>();
        row->set_overflow(Overflow::hidden);
        row->set_position(View::Position::absolute);
        row->set_left(0.0f);
        row->set_top(0.0f);
        auto* ptr = row.get();
        add_child(std::move(row));
        row_slots_.push_back({ptr, std::nullopt});
    }
    return finish(true);
}

bool VirtualList::release_row(RowSlot& slot) {
    if (!slot.row) return true;
    auto* row = slot.row;
    auto removed = remove_child(row);
    slot.row = nullptr;
    slot.index.reset();
    auto releaser = row_releaser_;
    if (removed && releaser) {
        auto alive = alive_;
        const auto generation = window_generation_;
        releaser(*removed);
        if (!alive->load(std::memory_order_acquire)) return false;
        if (!destroying_ && generation != window_generation_) return false;
    }
    (void)removed;
    return true;
}

bool VirtualList::update_window(bool force_rebind) {
    if (pool_resize_in_progress_) return true;
    const auto update_generation = ++window_generation_;
    const std::size_t desired = desired_pool_size();
    if (!ensure_pool_size(desired)) return false;
    if (desired == 0) {
        first_realized_index_ = 0;
        return true;
    }

    const std::size_t visible_start =
        static_cast<std::size_t>(std::max(0.0f, std::floor(scroll_y_ / row_height_)));
    const std::size_t before = static_cast<std::size_t>(std::max(0, overscan_rows_));
    const std::size_t max_first = row_count_ > desired ? row_count_ - desired : 0;
    first_realized_index_ = std::min(max_first, visible_start > before ? visible_start - before : 0);
    const std::size_t last = std::min(row_count_, first_realized_index_ + desired);

    for (std::size_t slot_index = 0; slot_index < row_slots_.size(); ++slot_index) {
        const auto index = first_realized_index_ + slot_index;
        if (index >= last) break;
        auto& slot = row_slots_[slot_index];
        if (!bind_slot(slot, index, force_rebind, update_generation)) return false;
        position_slot(slot);
    }
    return true;
}

bool VirtualList::bind_slot(RowSlot& slot, std::size_t index, bool force_rebind,
                            std::uint64_t update_generation) {
    if (!slot.row) return true;
    const bool changed = !slot.index || *slot.index != index;
    slot.index = index;
    if (changed || force_rebind) {
        slot.row->set_access_label("");
        slot.row->set_access_value("");
    }
    auto binder = row_binder_;
    if ((changed || force_rebind) && binder) {
        auto* row = slot.row;
        auto alive = alive_;
        binder(*row, index);
        if (!alive->load(std::memory_order_acquire)) return false;
        if (update_generation != window_generation_) return false;
        if (slot.row != row || !slot.index || *slot.index != index) return false;
    }
    update_row_accessibility(slot);
    return true;
}

void VirtualList::position_slot(RowSlot& slot) {
    if (!slot.row || !slot.index) return;
    const float width = row_width();
    const float y = static_cast<float>(*slot.index) * row_height_ - scroll_y_;
    auto& flex = slot.row->flex();
    flex.preferred_width = width;
    flex.preferred_height = row_height_;
    slot.row->set_left(0.0f);
    slot.row->set_top(y);
    slot.row->set_bounds({0.0f, y, width, row_height_});
}

void VirtualList::update_row_accessibility(RowSlot& slot) {
    if (!slot.row || !slot.index) return;
    const auto position = row_value(*slot.index, row_count_);
    slot.row->set_access_role(AccessRole::group);
    if (slot.row->access_label().empty()) {
        slot.row->set_access_label(position);
    }
    slot.row->set_access_value(position);
    if (focused_index_ && *focused_index_ == *slot.index) {
        slot.row->set_focus(true);
    } else {
        slot.row->set_focus(false);
    }
    if (selection_mode_ == SelectionMode::none) {
        slot.row->set_access_checked("");
    } else {
        slot.row->set_access_checked(is_selected(*slot.index) ? "true" : "false");
    }
}

VirtualList::UpdateResult VirtualList::set_scroll_y_internal(float y, bool request) {
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

bool VirtualList::apply_pending_scroll_to_row() {
    if (!pending_scroll_to_row_) return true;
    const auto index = *pending_scroll_to_row_;
    pending_scroll_to_row_.reset();
    auto alive = alive_;
    return scroll_to_row_internal(index) && alive->load(std::memory_order_acquire);
}

bool VirtualList::apply_pending_selected_row() {
    if (!pending_selected_row_ || row_count_ == 0) return true;
    const auto index = *pending_selected_row_;
    if (index >= row_count_) return true;
    pending_selected_row_.reset();
    auto alive = alive_;
    choose_row(index, false, false);
    return alive->load(std::memory_order_acquire);
}

void VirtualList::mark_scroll_dirty(float old_y, float new_y) {
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

float VirtualList::row_width() const {
    const auto b = local_bounds();
    const float gutter = scrollbar_visible() ? (scrollbar_width() + 2.0f * kScrollbarPad) : 0.0f;
    return std::max(0.0f, b.width - gutter);
}

std::optional<std::size_t> VirtualList::row_at(Point local) const {
    const auto b = local_bounds();
    if (row_height_ <= 0.0f || !b.contains(local)) return std::nullopt;
    if (scrollbar_visible() && local.x >= row_width()) return std::nullopt;
    const float y = local.y + scroll_y_;
    if (y < 0.0f) return std::nullopt;
    const auto index = static_cast<std::size_t>(y / row_height_);
    if (index >= row_count_) return std::nullopt;
    return index;
}

void VirtualList::choose_row(std::size_t index, bool extend, bool toggle, bool claim_focus) {
    if (row_count_ == 0 || index >= row_count_) return;
    focused_index_ = index;
    if (claim_focus) {
        on_focus_changed(true);
        claim_input_focus();
    }

    if (selection_mode_ == SelectionMode::none) {
        for (auto& slot : row_slots_) update_row_accessibility(slot);
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

VirtualList::SelectionUpdate VirtualList::set_selection_sanitized(std::vector<std::size_t> next,
                                                                  bool notify) {
    next.erase(std::remove_if(next.begin(), next.end(),
                              [this](std::size_t i) { return i >= row_count_; }),
               next.end());
    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());
    if (selection_mode_ == SelectionMode::single && next.size() > 1) {
        next.erase(next.begin(), next.end() - 1);
    }
    if (selection_mode_ == SelectionMode::none) next.clear();
    if (next == selection_) {
        for (auto& slot : row_slots_) update_row_accessibility(slot);
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

void VirtualList::move_focus_to(std::size_t index, bool extend) {
    if (row_count_ == 0) return;
    index = std::min(index, row_count_ - 1);
    focused_index_ = index;
    if (!scroll_to_row_internal(index)) return;
    choose_row(index, extend, false);
}

std::optional<std::size_t> VirtualList::keyboard_anchor_index() const {
    if (focused_index_ && *focused_index_ < row_count_) return *focused_index_;
    if (!selection_.empty() && selection_.back() < row_count_) return selection_.back();
    return std::nullopt;
}

std::size_t VirtualList::page_row_delta() const {
    if (row_height_ <= 0.0f) return 1;
    return std::max<std::size_t>(1, static_cast<std::size_t>(local_bounds().height / row_height_));
}

bool VirtualList::scrollbar_visible() const {
    return content_height() > local_bounds().height;
}

float VirtualList::scrollbar_width() const {
    return kScrollbarWidth;
}

float VirtualList::scrollbar_thumb_length() const {
    const auto b = local_bounds();
    if (!scrollbar_visible()) return 0.0f;
    const float track_h = std::max(0.0f, b.height - 2.0f * kScrollbarPad);
    const float ratio = b.height / std::max(b.height, content_height());
    return std::min(track_h, std::max(kMinThumbLength, track_h * ratio));
}

float VirtualList::scrollbar_thumb_y() const {
    const auto b = local_bounds();
    const float thumb_h = scrollbar_thumb_length();
    const float track_h = std::max(1.0f, b.height - 2.0f * kScrollbarPad - thumb_h);
    const float max_scroll = max_scroll_y();
    const float t = max_scroll > 0.0f ? scroll_y_ / max_scroll : 0.0f;
    return kScrollbarPad + t * track_h;
}

bool VirtualList::point_in_scrollbar(Point local) const {
    const auto b = local_bounds();
    return scrollbar_visible() && local.x >= b.width - kScrollbarHitWidth &&
           local.x <= b.width && local.y >= 0.0f && local.y <= b.height;
}

bool VirtualList::point_in_scrollbar_thumb(Point local) const {
    if (!point_in_scrollbar(local)) return false;
    const float y = scrollbar_thumb_y();
    return local.y >= y && local.y <= y + scrollbar_thumb_length();
}

} // namespace pulp::view
