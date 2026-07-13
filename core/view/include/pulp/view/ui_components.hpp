#pragma once

/// @file ui_components.hpp
/// Additional UI components: ComboBox, Tooltip, ProgressBar, CallOutBox,
/// TabPanel, ListBox, ScrollView.

#include <pulp/view/view.hpp>
#include <pulp/view/accessibility.hpp>
#include <pulp/view/input_events.hpp>
#include <algorithm>
#include <pulp/view/animation.hpp>
#include <pulp/view/appearance_tracker.hpp>
#include <pulp/canvas/canvas.hpp>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <memory>

namespace pulp::view {

// Fit a label into `avail` px. Prefer SHRINKING the font from `base` toward
// `min` to show the full text; only ellipsize (in-place, with "...") if it still
// overflows at `min`. `width_at(s, f)` returns the rendered width of `s` at font
// size `f` (e.g. a canvas set_font + measure_text). Returns the chosen font
// size and rewrites `text` to the string to draw. Pure logic — unit-tested
// independently of any canvas (a ComboBox box is often sized tight to a design's
// own label, so truncating reads worse than a slightly smaller full label).
float fit_combo_label(std::string& text, float avail, float base, float min,
                      const std::function<float(const std::string&, float)>& width_at);

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
    // A ComboBox is a dropdown, not a continuous range. It announced itself
    // as AccessRole::slider until the role vocabulary grew `combo_box`, so
    // VoiceOver / Narrator / Orca all told the user "slider" and offered
    // increment/decrement instead of a list of choices.
    ComboBox() {
        set_focusable(true);
        set_access_role(AccessRole::combo_box);
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

    /// Publishes the currently-selected item into access_value: the selected
    /// text IS a combo box's accessible value (what NSAccessibility /
    /// IValueProvider read), and selected_ defaults to 0 — so a combo that was
    /// never explicitly re-selected used to paint items[0] and announce nothing.
    void set_items(std::vector<std::string> items) {
        items_ = std::move(items);
        set_access_value(selected_text());
    }
    const std::vector<std::string>& items() const { return items_; }

    int selected() const { return selected_; }
    void set_selected(int index);
    void set_selected_silent(int index);

    const std::string& selected_text() const;

    /// Called when selection changes.
    std::function<void(int index)> on_change;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_event(const MouseEvent& event) override;
    void on_hover_move(Point local_pos) override;  // track hovered dropdown row
    bool on_key_event(const KeyEvent& event) override;
    View* hit_test(Point local_point) override;  // extend hit area over the open dropdown
    bool wants_mouse_input() const override { return true; }

    void on_text_input(const TextInputEvent& event) override;

    bool is_open() const { return open_; }
    /// True when the menu currently renders ABOVE the field (it would spill past
    /// the viewport bottom). Scroll-aware: reflects the field's on-screen
    /// position, so scrolling the page can change the answer.
    bool flips_up() const { return dropdown_local_top() < 0.0f; }
    /// Index of the row painted with the hover/keyboard highlight (-1 = none).
    int hovered_index() const { return hover_index_; }
    float dropdown_width_hint() const;

    /// On-screen rect (root/overlay space) of the open dropdown menu — already
    /// flip-aware, scroll-aware, and clamped inside the window. The platform
    /// window host uses this to route clicks over the menu to this combo so they
    /// don't fall through to whatever sibling view sits underneath. Returns false
    /// when the menu is closed.
    bool dropdown_window_rect(float& x, float& y, float& w, float& h) const;

    /// Close any currently open ComboBox (call before opening a new one).
    static void close_active_popup();

    /// Called by the root view on any mouse click — closes popup if click is outside.
    static void notify_global_click(View* target);

private:
    void set_selected_impl(int index, bool notify);
    void open_dropdown();
    void close_dropdown();
    // Top of the dropdown menu in this combo's LOCAL coords. Below the field normally;
    // negative (above the field) when flipped up because it would spill past the window.
    // Shared by paint, hit_test, and mouse hover so they always agree.
    float dropdown_local_top() const;
    // Flip/clamp/scroll-aware menu geometry in this combo's LOCAL coords, shared
    // by paint, hit_test, hover and mouse so they always agree. `top_local` is the
    // menu top relative to the field (negative = flipped above). `height` is the
    // painted menu height (clamped so the menu never spills past the window).
    // `first_row`/`visible_rows` select which items are drawn when the full list is
    // taller than the available space (scrolling via dropdown_scroll_).
    void dropdown_metrics(float& top_local, float& height,
                          int& first_row, int& visible_rows) const;
    // On-screen (overlay-space) geometry of the field: top-left x/y and the
    // height AND width of the nearest scroll viewport (root window when none).
    // The dropdown overlay paints at the root canvas with NO scroll translation,
    // so every ScrollView ancestor's scroll offset must be peeled off here —
    // otherwise the menu renders at the unscrolled position once the page is
    // scrolled, and the flip/clamp decisions compare against the wrong viewport.
    void overlay_anchor_(float& out_x, float& out_y, float& out_viewport_h,
                         float& out_viewport_w) const;
    void move_hover(int delta);  // keyboard: move the highlighted row, skipping separators

    std::vector<std::string> items_;
    int selected_ = 0;
    int hover_index_ = -1;  ///< Currently hovered item in dropdown (-1 = none)
    int dropdown_scroll_ = 0;  ///< First visible row when the menu is taller than the viewport.
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

// ── ThemeModeControl ──────────────────────────────────────────────────────

/// Compact 3-segment control to pick the theme mode: system / light / dark.
/// Each segment is an icon (auto split-disc, sun, moon); the active one is
/// highlighted and the hovered one shows its name. Clicking fires
/// `on_mode_change(ThemeMode)`. Pair it with a `ThemeManager` (call its
/// `set_mode()` from the callback, and `set_mode(manager.mode())` to reflect
/// the live OS-follow state). Opt-in: a developer offering only one mode just
/// never places this control.
class ThemeModeControl : public View {
public:
    ThemeModeControl() { set_focusable(true); }
    void set_mode(ThemeMode m) { if (m != mode_) { mode_ = m; request_repaint(); } }
    ThemeMode mode() const { return mode_; }
    std::function<void(ThemeMode)> on_mode_change;
    int hovered_segment() const { return hover_seg_; }  ///< for tests; -1 none
    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_event(const MouseEvent& event) override;  // hover tracking
    bool wants_mouse_input() const override { return true; }
    float intrinsic_height() const override { return 28.0f; }
private:
    int segment_at_(Point pos) const;  // 0=system,1=light,2=dark, -1 outside
    ThemeMode mode_ = ThemeMode::system;
    int hover_seg_ = -1;
};

// ── ProgressBar ──────────────────────────────────────────────────────────

/// Visual progress indicator for long-running operations.
///
/// Implements AccessibilityValueInterface: `AccessRole::progress_bar` advertises
/// the UIA RangeValue pattern / AT-SPI Value interface, and without a value
/// source behind it Narrator reads 0 at every progress and reports the
/// degenerate range min == max == 0 (a client computing (v-min)/(max-min)
/// divides by zero). The role and the value ship together.
class ProgressBar : public View, public AccessibilityValueInterface {
public:
    ProgressBar() { set_access_role(AccessRole::progress_bar); }

    /// Set progress value (0.0 to 1.0). Values < 0 show indeterminate state.
    void set_progress(float value) { progress_ = value; }
    float progress() const { return progress_; }

    /// True while `progress() < 0` — the bar is animating an unknown-duration
    /// task. AT reports 0 with the "indeterminate" value string.
    bool is_indeterminate() const { return progress_ < 0.0f; }

    // ── AccessibilityValueInterface ──────────────────────────────────────
    double get_current_value() const override {
        return is_indeterminate() ? 0.0
                                  : static_cast<double>(std::clamp(progress_, 0.0f, 1.0f));
    }
    /// A progress bar is read-only (it advertises RangeValue, not Value), but
    /// the interface is bidirectional; a host that does write lands on
    /// set_progress rather than being silently dropped.
    void set_current_value(double value) override {
        set_progress(static_cast<float>(value));
    }
    double get_minimum_value() const override { return 0.0; }
    double get_maximum_value() const override { return 1.0; }
    std::string get_value_string() const override {
        if (is_indeterminate()) return "indeterminate";
        return AccessibilityValueInterface::get_value_string();
    }

    /// Optional label shown inside the bar. Doubles as the accessible name —
    /// a progress bar that announces "60%" without saying 60% of WHAT is only
    /// half an announcement.
    void set_label(std::string label) {
        label_ = std::move(label);
        set_derived_access_label(label_);
    }

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

    /// Visual treatment of the tab bar. `filled` (default) paints a
    /// `bg.secondary` strip behind the row with a teal underline marking the
    /// active tab — the historic look, unchanged for every existing consumer.
    /// `underline` is the Ink & Signal navigation treatment: NO background
    /// strip (titles sit directly on the panel), the active tab marked by a
    /// teal underline, and a faint full-width `divider` rule under the whole
    /// row. Opt-in via set_tab_bar_style(); it never changes the default.
    enum class TabBarStyle { filled, underline };

    // Reserve the tab-bar height as top padding so tab content flows BELOW the tab bar
    // (instead of painting over it). Uses flex padding so child hit-testing stays correct.
    // No AccessRole::tab_list here, deliberately. The tab strip is PAINTED;
    // TabPanel's child Views are the tab CONTENT panels, not the tabs. A
    // `tab_list` whose children are panels is worse than no role at all —
    // VoiceOver's tab navigation would walk content instead of tabs. The role
    // lands with per-tab `AccessRole::tab` elements (docs/guides/modules/view.md).
    TabPanel() {
        flex().padding_top = tab_height_;
    }

    void add_tab(std::string title, std::unique_ptr<View> content);
    void set_active_tab(int index);
    int active_tab() const { return active_; }
    int tab_count() const { return static_cast<int>(tabs_.size()); }
    std::string_view tab_title(int index) const;
    int find_tab(std::string_view title) const;
    bool set_active_tab(std::string_view title);

    /// Hide the tab bar and use the panel purely as a navigable card stack (the active
    /// tab is shown; switch with set_active_tab()). Useful when an outer container should
    /// not show its own tabs — e.g. a standalone editor whose Settings is reached via a
    /// button rather than a tab.
    void set_show_tab_bar(bool show) {
        show_tab_bar_ = show;
        flex().padding_top = show ? tab_height_ : 0.0f;
    }

    /// Select the tab-bar visual treatment (see TabBarStyle). Opt-in; defaults
    /// to `filled` so existing apps render identically.
    void set_tab_bar_style(TabBarStyle style) { tab_bar_style_ = style; }
    TabBarStyle tab_bar_style() const { return tab_bar_style_; }

    /// Called when the active tab changes.
    std::function<void(int index)> on_tab_change;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_event(const MouseEvent& event) override;
    bool wants_mouse_input() const override { return true; }

private:
    std::vector<Tab> tabs_;
    int active_ = 0;
    float tab_height_ = 32.0f;
    bool show_tab_bar_ = true;
    TabBarStyle tab_bar_style_ = TabBarStyle::filled;
};

// ── SegmentedControl ───────────────────────────────────────────────────────

/// Horizontal segmented selector — a row of mutually-exclusive labelled
/// segments sharing one inset track, with the selected segment raised as an
/// "elevated" pill (the Ink & Signal navigation treatment, Figma 227:1763).
///
/// Distinct from `ThemeModeControl` (a fixed 3-icon system/light/dark picker)
/// and from `TabPanel` (which owns and swaps child content): SegmentedControl
/// is a stateless N-way switch that just reports the chosen index. Fully
/// token-driven (`bg.surface` track, `bg.elevated` active pill, `divider`
/// pill border, `text.primary`/`tab.inactive` labels). Brand-new widget — no
/// existing consumer, so it carries zero blast radius.
///
/// @code
/// SegmentedControl seg;
/// seg.set_segments({"Amp", "EQ", "Comp", "Reverb"});
/// seg.set_selected(0);
/// seg.on_change = [&](int i) { show_page(i); };
/// @endcode
class SegmentedControl : public View {
public:
    SegmentedControl() {
        set_focusable(true);
        set_access_role(AccessRole::group);
    }

    void set_segments(std::vector<std::string> segments) {
        segments_ = std::move(segments);
        if (selected_ >= static_cast<int>(segments_.size())) selected_ = 0;
        request_repaint();
    }
    const std::vector<std::string>& segments() const { return segments_; }

    int selected() const { return selected_; }
    void set_selected(int index);          ///< clamps + notifies on_change
    void set_selected_silent(int index);   ///< clamps, no callback

    /// Called when the selected segment changes (user click or set_selected).
    std::function<void(int index)> on_change;

    int hovered_segment() const { return hover_; }  ///< for tests; -1 = none

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_event(const MouseEvent& event) override;
    bool on_key_event(const KeyEvent& event) override;
    bool wants_mouse_input() const override { return true; }
    float intrinsic_height() const override { return 28.0f; }

private:
    int segment_at_(Point pos) const;  ///< -1 outside

    std::vector<std::string> segments_;
    int selected_ = 0;
    int hover_ = -1;
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
    bool wants_wheel_scroll() const override {
        const auto b = local_bounds();
        return content_size_.height > b.height || content_size_.width > b.width;
    }

    float scroll_x() const { return smooth_scroll_x_.value(); }
    float scroll_y() const { return smooth_scroll_y_.value(); }
    bool scroll_animating() const {
        return smooth_scroll_x_.animating() || smooth_scroll_y_.animating();
    }
    void set_scroll(float x, float y);

    /// Scroll by a delta.
    ///
    /// @param animate When true (the default, used for PROGRAMMATIC scroll
    ///        such as scroll-into-view), the offset eases to its target over
    ///        ~motion.duration.fast. When false, the offset jumps instantly —
    ///        used for WHEEL / trackpad input, where the OS already smooths
    ///        and inertia-pads the delta stream, so animating each frame lags
    ///        behind the fingers. The `scroll-behavior: auto` fast-path forces
    ///        instant regardless of this flag.
    void scroll_by(float dx, float dy, bool animate = true);

    void paint(canvas::Canvas& canvas) override;
    void paint_all(canvas::Canvas& canvas) override;

    /// paint_all() translates children by (-scroll_x, -scroll_y), so once
    /// scrolled the content no longer sits at a plain bounds offset. Reporting
    /// true here makes a descendant's bounded request_repaint(Rect) escalate to
    /// a full repaint instead of invalidating the wrong (unscrolled) root rect.
    bool applies_child_paint_offset() const override {
        return scroll_x() != 0.0f || scroll_y() != 0.0f;
    }

    View* hit_test(Point local_point) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;

    /// Layout children using content_size instead of view bounds.
    /// This prevents children from squishing when the ScrollView shrinks.
    void layout_children() override;

    // Scroll via mouse wheel, touch drag, or scrollbar drag
    void on_mouse_event(const MouseEvent& event) override;
    void on_mouse_drag(Point pos) override;
    bool wants_mouse_input() const override { return true; }

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

/// Find the deepest ScrollView whose bounds contain @p root_point (a point
/// in @p root's local coordinate space).
///
/// Unlike View::hit_test, this ignores hit_testable / pointer-events gates
/// and matches a ScrollView even over its EMPTY background (where hit_test
/// can return null if no child sits under the point). The mac window host
/// uses this so a wheel/trackpad scroll routes to the pane the cursor is
/// over even when hovering blank space inside it — no click required.
/// Returns nullptr if the point is over no ScrollView.
ScrollView* find_scroll_view_at(View& root, Point root_point);

/// Find the deepest view that participates in native wheel scrolling.
View* find_wheel_scroll_view_at(View& root, Point root_point);

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
    /// Selected-row treatment. `standard` (default) fills the row with
    /// `bg.elevated` and keeps the normal text colour — the historic look, so
    /// every existing list renders identically. `accent` is the Ink & Signal
    /// sidebar-nav treatment (Figma 227:1830): a translucent accent tint
    /// (`nav.selected.bg`) plus a teal left-edge bar, with the label drawn in
    /// the accent colour (`nav.selected.text`). Opt-in via set_selection_style().
    enum class SelectionStyle { standard, accent };

    // No AccessRole::list here, deliberately — same reason as TableListBox:
    // rows are painted, not child Views, so the role would export an empty
    // AXList ("list, 0 items"). See docs/guides/modules/view.md.
    ListBox() { set_focusable(true); }

    void set_items(std::vector<std::string> items) { items_ = std::move(items); }
    const std::vector<std::string>& items() const { return items_; }

    /// Optional per-row leading icon glyphs (parallel to items()). When set,
    /// each row paints its glyph before the label; rows past the icon list, or
    /// when no icons are set, render label-flush as before. Opt-in.
    void set_icons(std::vector<std::string> icons) { icons_ = std::move(icons); }

    /// Select the selected-row treatment (see SelectionStyle). Opt-in; defaults
    /// to `standard`.
    void set_selection_style(SelectionStyle s) { selection_style_ = s; }
    SelectionStyle selection_style() const { return selection_style_; }

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
    bool wants_mouse_input() const override { return true; }

    /// Scroll to make the given item index visible in the viewport.
    void ensure_visible(int index);

private:
    std::vector<std::string> items_;
    std::vector<std::string> icons_;
    SelectionStyle selection_style_ = SelectionStyle::standard;
    int selected_ = -1;
    float row_height_ = 24.0f;
    float scroll_offset_ = 0.0f;
};

} // namespace pulp::view
