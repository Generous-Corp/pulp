#pragma once

/// @file context_menu.hpp
/// Pulp-drawn (Canvas/Skia) popup / context menu rendered in the view tree.
///
/// Unlike a native OS / GTK popup menu, ContextMenu is a plain View subclass:
/// a full-window transparent overlay that draws a menu box at an anchor point.
/// It works identically on every platform and pulls in no new dependencies.
/// Rows are selectable; selection fires a callback; outside-click and Escape
/// dismiss the menu. It mirrors the ComboBox dropdown idioms (24px rows, hover
/// highlight, separators, checkmarks, keyboard nav skipping separators) and the
/// ModalOverlay full-window overlay pattern (minus the dimmed backdrop — a
/// context menu does not darken what is behind it).

#include <pulp/view/view.hpp>
#include <pulp/view/widget_painter.hpp>
#include <pulp/view/widget_metrics.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/canvas/canvas.hpp>
#include <functional>
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pulp::view {

/// Floating, view-tree-drawn context / popup menu.
///
/// @code
/// ContextMenu::show(root, {x, y},
///     {{1, "Cut"}, {2, "Copy"}, ContextMenu::Item::make_separator(), {3, "Paste"}},
///     [&](std::optional<int> id) { if (id) handle_command(*id); });
/// @endcode
class ContextMenu : public View {
public:
    struct Item {
        enum class ControlKind { action, toggle, slider, choice };
        int id = 0;
        std::string label;
        bool enabled = true;
        bool checked = false;
        ControlKind control_kind = ControlKind::action;
        float normalized_value = 0.0f;
        float normalized_step = 0.05f;
        std::vector<std::string> choices;
        std::size_t choice_index = 0;
        std::function<void(const Item&)> on_change;
        bool separator = false;
        /// A non-selectable title row that groups the items below it.
        bool header = false;
        bool has_submenu = false;
        static Item make_separator() {
            Item i;
            i.separator = true;
            return i;
        }
        static Item make_header(std::string title) {
            Item i;
            i.label = std::move(title);
            i.enabled = false;
            i.header = true;
            return i;
        }
        static Item make_toggle(int id, std::string label, bool checked,
                                std::function<void(const Item&)> on_change) {
            Item i;
            i.id = id;
            i.label = std::move(label);
            i.checked = checked;
            i.control_kind = ControlKind::toggle;
            i.on_change = std::move(on_change);
            return i;
        }
        static Item make_slider(int id, std::string label, float normalized, float step,
                                std::function<void(const Item&)> on_change) {
            Item i;
            i.id = id;
            i.label = std::move(label);
            i.control_kind = ControlKind::slider;
            i.normalized_value = std::clamp(normalized, 0.0f, 1.0f);
            i.normalized_step = step > 0.0f ? step : 0.05f;
            i.on_change = std::move(on_change);
            return i;
        }
        static Item make_choice(int id, std::string label, std::vector<std::string> choices,
                                std::size_t selected, std::function<void(const Item&)> on_change) {
            Item i;
            i.id = id;
            i.label = std::move(label);
            i.control_kind = ControlKind::choice;
            i.choices = std::move(choices);
            i.choice_index = i.choices.empty() ? 0 : std::min(selected, i.choices.size() - 1);
            i.on_change = std::move(on_change);
            return i;
        }
        void set_toggled(bool value) {
            if (control_kind != ControlKind::toggle || checked == value)
                return;
            checked = value;
            if (on_change)
                on_change(*this);
        }
        void set_normalized_value(float value) {
            if (control_kind != ControlKind::slider)
                return;
            const float next = std::clamp(value, 0.0f, 1.0f);
            if (normalized_value == next)
                return;
            normalized_value = next;
            if (on_change)
                on_change(*this);
        }
        void set_choice_index(std::size_t value) {
            if (control_kind != ControlKind::choice || choices.empty())
                return;
            const auto next = std::min(value, choices.size() - 1);
            if (choice_index == next)
                return;
            choice_index = next;
            if (on_change)
                on_change(*this);
        }
    };

    ContextMenu() { set_focusable(true); }

    void set_items(std::vector<Item> items) { items_ = std::move(items); }
    std::vector<Item>& items() {
        return items_;
    }
    const std::vector<Item>& items() const { return items_; }

    /// Fired with the chosen item id on select, or with std::nullopt on dismiss
    /// (Escape / outside-click). Called exactly once per show lifecycle: the
    /// first fire latches `closed_` so subsequent input is ignored.
    std::function<void(std::optional<int>)> on_close;

    /// Anchor (in the overlay/root LOCAL coordinate space) where the menu's
    /// top-left should appear; flips left/up if it would spill past bounds.
    void set_anchor(Point anchor) { anchor_ = anchor; }

    int hovered_index() const { return hover_index_; }  ///< for tests; -1 = none

    /// Mount `items` as a full-window overlay child of `root` at `pos`, returning
    /// a raw pointer to the live ContextMenu (root owns it). The wrapped on_close
    /// removes the menu from `root` after firing the caller's callback.
    static ContextMenu* show(View* root, Point pos, std::vector<Item> items,
                             std::function<void(std::optional<int>)> on_close);

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_event(const MouseEvent& event) override;
    bool on_key_event(const KeyEvent& event) override;
    /// An open menu owns the arrow keys, the same way an open ComboBox does.
    /// Without this the host never routes them here: both the plugin-editor
    /// path and the standalone path gate navigation keys on this predicate,
    /// so a focused menu's arrow keys went back to the DAW and `on_key_event`
    /// -- which has handled up/down since it shipped -- was simply never
    /// called. Every keyboard test drove `on_key_event` directly and so could
    /// not see it.
    bool accepts_navigation_input() const override { return !closed_; }
    View* hit_test(Point local_point) override;  // whole overlay is hit (catch outside clicks)
    bool wants_mouse_input() const override { return true; }

    /// The menu's resolved geometry: the panel rect and one rect per row, in
    /// this view's LOCAL coordinates. Rows are NOT assumed to be uniform height
    /// — a skin may make a separator two pixels tall and a label row twenty —
    /// so hit-testing and painting both walk this list rather than dividing by
    /// a row constant.
    struct MenuLayout {
        Rect box{};
        float border = 0.0f;         ///< panel-edge padding on all four sides
        FontSpec font{};             ///< the font the rows are measured/drawn with
        std::vector<Rect> rows{};    ///< one per item, in item order
        /// Height the rows want, before the panel is capped to the overlay.
        float content_height = 0.0f;
        /// `content_height - box.height`, clamped at 0. Non-zero means the
        /// panel could not show every row and the rows scroll inside it.
        float max_scroll = 0.0f;
    };

    /// Current scroll offset in pixels, clamped to `[0, max_scroll]`. Exposed
    /// so a test can assert that a row was brought into view rather than
    /// asserting on painted pixels.
    float scroll_offset() const {
        return scroll_;
    }

    /// Compute the menu's own size and row positions, with no canvas and no
    /// paint. This is the sizing entry point: it asks the metrics delegate
    /// (`effective_metrics()`) for each row's natural size, the panel border,
    /// and the row font, and falls back to the stock look for anything the
    /// delegate declines. A caller can therefore ask a menu how big it wants to
    /// be BEFORE it is ever shown.
    MenuLayout layout() const;

    /// Convenience: the panel width `layout()` would choose.
    float measured_width() const { return layout().box.width; }

    /// A lane is the product-independent state needed by the standard two-LFO
    /// modulation popup. Products keep ownership of their state and receive a
    /// copy after each edit through `on_change`; the SDK owns the hierarchy,
    /// controls, keyboard behavior, hover state, and dismissal lifecycle.
    struct ModulationLane {
        std::string label; ///< e.g. "LFO 1"
        bool enabled = false;
        float rate_normalized = 0.25f;
        float depth_normalized = 0.0f;
        std::vector<std::string> shapes; ///< e.g. Sine, Triangle, Square
        std::size_t shape_index = 0;
        std::function<void(const ModulationLane&)> on_change;
    };

    struct ModulationMenuSpec {
        std::string target_label;
        ModulationLane lanes[2];
        std::size_t lane_count = 2;
        std::function<void()> on_reset;
        std::function<void()> on_dismiss;
    };

    /// Build the canonical Spectr-style two-LFO menu. This is deliberately a
    /// factory for the regular ContextMenu rather than a second popup class,
    /// so all products inherit the same focus, pointer capture, keyboard,
    /// hover, and outside-click behavior automatically.
    static std::vector<Item> make_modulation_items(const ModulationMenuSpec& spec);

    /// Show the canonical modulation popup at a root-local anchor. The reset
    /// action is returned as the final command and invokes `on_reset`; all lane
    /// edits invoke the lane's callback while the menu remains open.
    static ContextMenu* show_modulation(View* root, Point pos, ModulationMenuSpec spec);

  private:
    // Row index under a local point, or -1 if outside the box / on a non-row.
    int row_at(Point local_point, const MenuLayout& lay) const;
    // Keyboard nav must not move the hover somewhere the panel is not showing.
    void scroll_row_into_view(int index);
    void move_hover(int delta);    // keyboard nav, skipping separators + disabled
    void move_hover_to_edge(bool last);  // Home / End
    void adjust_control(Item& item, float delta);
    bool set_control_from_pointer(Item& item, const Rect& row, Point point);
    void fire_close(std::optional<int> result);

    std::vector<Item> items_;
    Point anchor_{0, 0};
    int hover_index_ = -1;
    int active_slider_index_ = -1;
    int active_pointer_id_ = 0;
    bool closed_ = false;
    float scroll_ = 0.0f;

    static constexpr float kRowHeight = 24.0f;
    static constexpr float kHPad = 34.0f;   // total horizontal label padding
    static constexpr float kMinWidth = 120.0f;
    static constexpr float kRadius = 4.0f;
    /// A separator is a hairline with breathing room, not a row. It was 24 —
    /// the same as a full label row — which made the `separator ? … : …` branch
    /// in layout() a no-op and left every divider floating in an empty row's
    /// worth of space, with the panel taller than its contents by 17px per
    /// separator.
    static constexpr float kSeparatorHeight = 7.0f;
    static constexpr float kFontSize = 12.0f;
    /// Gap kept between the panel and the overlay edge when the panel has to
    /// be capped. Without it a full-height menu sits flush against the window
    /// edge and reads as clipped rather than scrollable.
    static constexpr float kEdgeMargin = 4.0f;
};

/// Design-system alias. The Ink & Signal Figma library names this primitive
/// "PopupMenu"; the Pulp framework class is ContextMenu. The alias lets
/// design-system code use the Figma name without a breaking rename. See
/// docs/reference/design-system-naming.md for the full Figma↔SDK name map.
/// (No `class PopupMenu` exists in pulp::view, so this is collision-free.)
using PopupMenu = ContextMenu;

} // namespace pulp::view
