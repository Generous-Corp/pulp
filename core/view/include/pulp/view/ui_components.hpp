#pragma once

/// @file ui_components.hpp
/// Additional UI components: ComboBox, Tooltip, ProgressBar, CallOutBox,
/// TabPanel, ListBox, ScrollView.

#include <pulp/view/view.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/canvas/canvas.hpp>
#include <functional>
#include <string>
#include <vector>
#include <memory>

namespace pulp::view {

// ── ComboBox ─────────────────────────────────────────────────────────────

/// Drop-down selector for enumerated parameters.
///
/// @code
/// ComboBox combo;
/// combo.set_items({"Sine", "Saw", "Square", "Triangle"});
/// combo.set_selected(0);
/// combo.on_change = [&](int index) { set_waveform(index); };
/// @endcode
class ComboBox : public View {
public:
    ComboBox() {
        set_focusable(true);
        set_access_role(AccessRole::slider);
    }

    // pulp #1818 — clear the static `active_popup_` slot if this dying
    // ComboBox holds it. Without this, an unmounted React dropdown leaves
    // a dangling pointer that the platform window host dereferences on
    // the next mouseDown (PAC failure on the vtable load — exact crash
    // signature in the issue). Pattern matches `~View()` for `active_overlay_`
    // and `focused_input_`.
    ~ComboBox() override {
        if (active_popup_ == this) active_popup_ = nullptr;
    }

    void set_items(std::vector<std::string> items) { items_ = std::move(items); }
    const std::vector<std::string>& items() const { return items_; }

    int selected() const { return selected_; }
    void set_selected(int index);
    void set_selected_silent(int index);

    const std::string& selected_text() const;

    /// Called when selection changes.
    std::function<void(int index)> on_change;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_event(const MouseEvent& event) override;
    bool on_key_event(const KeyEvent& event) override;

    void on_text_input(const TextInputEvent& event) override;

    bool is_open() const { return open_; }
    float dropdown_width_hint() const;

    /// Close any currently open ComboBox (call before opening a new one).
    static void close_active_popup();

    /// Called by the root view on any mouse click — closes popup if click is outside.
    static void notify_global_click(View* target);

private:
    void set_selected_impl(int index, bool notify);
    void open_dropdown();
    void close_dropdown();

    std::vector<std::string> items_;
    int selected_ = 0;
    int hover_index_ = -1;  ///< Currently hovered item in dropdown (-1 = none)
    bool open_ = false;

public:
    static ComboBox* active_popup_;
private:
    static const std::string empty_string_;
};

// ── Tooltip ──────────────────────────────────────────────────────────────

/// Hover tooltip that displays text near a target view.
/// Attach to any view via set_tooltip().
class Tooltip : public View {
public:
    explicit Tooltip(std::string text = {}) : text_(std::move(text)), opacity_(0.0f) {
        set_visible(false);
    }

    void set_text(std::string text) { text_ = std::move(text); }
    const std::string& text() const { return text_; }

    /// Show the tooltip near a given position (with fade-in).
    void show_at(Point position);

    /// Hide the tooltip (with fade-out).
    void hide();

    void paint(canvas::Canvas& canvas) override;

    // Animation accessors for testing
    float opacity() const { return opacity_.value(); }
    void advance_animations(float dt);

private:
    std::string text_;
    ValueAnimation opacity_;
};

// ── ProgressBar ──────────────────────────────────────────────────────────

/// Visual progress indicator for long-running operations.
class ProgressBar : public View {
public:
    /// Set progress value (0.0 to 1.0). Values < 0 show indeterminate state.
    void set_progress(float value) { progress_ = value; }
    float progress() const { return progress_; }

    /// Optional label shown inside the bar.
    void set_label(std::string label) { label_ = std::move(label); }

    void paint(canvas::Canvas& canvas) override;

private:
    float progress_ = 0.0f;
    std::string label_;
};

// ── CallOutBox ───────────────────────────────────────────────────────────

/// Floating alert/notification box.
///
/// @code
/// auto alert = CallOutBox::confirm("Delete preset?",
///     [&] { delete_preset(); },
///     [&] { /* cancel */ });
/// root.add_child(std::move(alert));
/// @endcode
class CallOutBox : public View {
public:
    CallOutBox() = default;

    void set_message(std::string msg) { message_ = std::move(msg); }
    const std::string& message() const { return message_; }

    /// Called when the user confirms (OK/Yes).
    std::function<void()> on_confirm;
    /// Called when the user cancels (Cancel/No) or presses Escape.
    std::function<void()> on_cancel;

    /// Auto-dismiss after N seconds (0 = no auto-dismiss).
    float auto_dismiss_seconds = 0;

    /// Create a confirmation dialog with OK/Cancel buttons.
    static std::unique_ptr<CallOutBox> confirm(
        const std::string& message,
        std::function<void()> on_ok,
        std::function<void()> on_cancel = {});

    /// Create a notification that auto-dismisses.
    static std::unique_ptr<CallOutBox> notify(
        const std::string& message,
        float duration_seconds = 3.0f);

    void paint(canvas::Canvas& canvas) override;
    bool on_key_event(const KeyEvent& event) override;

private:
    std::string message_;
};

// ── TabPanel ─────────────────────────────────────────────────────────────

/// Tabbed container that shows one child at a time.
///
/// @code
/// TabPanel tabs;
/// tabs.add_tab("Basic", std::move(basic_panel));
/// tabs.add_tab("Advanced", std::move(advanced_panel));
/// tabs.set_active_tab(0);
/// @endcode
class TabPanel : public View {
public:
    struct Tab {
        std::string title;
        std::unique_ptr<View> content;
    };

    void add_tab(std::string title, std::unique_ptr<View> content);
    void set_active_tab(int index);
    int active_tab() const { return active_; }
    int tab_count() const { return static_cast<int>(tabs_.size()); }

    /// Called when the active tab changes.
    std::function<void(int index)> on_tab_change;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_event(const MouseEvent& event) override;

private:
    std::vector<Tab> tabs_;
    int active_ = 0;
    float tab_height_ = 32.0f;
};

// ── ScrollView ───────────────────────────────────────────────────────────

/// Scrollable container that clips and scrolls its content.
/// Scroll bars animate: fade in and widen on hover, fade out when idle.
class ScrollView : public View {
public:
    enum class Direction { vertical, horizontal, both };

    void set_direction(Direction d) { direction_ = d; }
    void set_content_size(Size size) { content_size_ = size; }
    Size content_size() const { return content_size_; }

    float scroll_x() const { return smooth_scroll_x_.value(); }
    float scroll_y() const { return smooth_scroll_y_.value(); }
    bool scroll_animating() const {
        return smooth_scroll_x_.animating() || smooth_scroll_y_.animating();
    }
    void set_scroll(float x, float y);

    /// Scroll by a delta (e.g., from mouse wheel). Animates smoothly.
    void scroll_by(float dx, float dy);

    void paint(canvas::Canvas& canvas) override;
    void paint_all(canvas::Canvas& canvas) override;
    View* hit_test(Point local_point) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;

    /// Layout children using content_size instead of view bounds.
    /// This prevents children from squishing when the ScrollView shrinks.
    void layout_children() override;

    // Scroll via mouse wheel, touch drag, or scrollbar drag
    void on_mouse_event(const MouseEvent& event) override;
    void on_mouse_drag(Point pos) override;

    // Animation accessors for testing
    float bar_opacity() const { return bar_opacity_.value(); }
    float bar_width() const { return bar_width_.value(); }
    float target_scroll_y() const { return target_scroll_y_; }
    void advance_animations(float dt);

private:
    void clamp_scroll_targets();

    Direction direction_ = Direction::vertical;
    Size content_size_{0, 0};
    float target_scroll_x_ = 0, target_scroll_y_ = 0;
    ValueAnimation smooth_scroll_x_{0.0f};  // smoothly interpolated scroll position
    ValueAnimation smooth_scroll_y_{0.0f};
    ValueAnimation bar_opacity_{0.0f};      // fade in/out on hover
    ValueAnimation bar_width_{4.0f};        // narrow when idle, wide on hover

    // Scrollbar drag state
    bool dragging_v_bar_ = false;
    bool dragging_h_bar_ = false;
    float drag_offset_ = 0;  // offset from top of thumb where drag started
};

// ── ListBox ──────────────────────────────────────────────────────────────

/// Scrollable list of selectable items.
///
/// @code
/// ListBox list;
/// list.set_items({"Item 1", "Item 2", "Item 3"});
/// list.on_select = [&](int index) { handle_selection(index); };
/// @endcode
class ListBox : public View {
public:
    ListBox() { set_focusable(true); }

    void set_items(std::vector<std::string> items) { items_ = std::move(items); }
    const std::vector<std::string>& items() const { return items_; }

    int selected() const { return selected_; }
    void set_selected(int index);

    float row_height() const { return row_height_; }
    void set_row_height(float h) { row_height_ = h; }

    /// Called when an item is selected.
    std::function<void(int index)> on_select;
    /// Called when an item is double-clicked.
    std::function<void(int index)> on_activate;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_event(const MouseEvent& event) override;
    bool on_key_event(const KeyEvent& event) override;

    /// Scroll to make the given item index visible in the viewport.
    void ensure_visible(int index);

private:
    std::vector<std::string> items_;
    int selected_ = -1;
    float row_height_ = 24.0f;
    float scroll_offset_ = 0.0f;
};

} // namespace pulp::view
