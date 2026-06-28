#include <catch2/catch_test_macros.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/breadcrumb.hpp>
#include <pulp/canvas/canvas.hpp>

#include <memory>

using namespace pulp::view;
using namespace pulp::canvas;

namespace {
// True if any fill_rect command has the given width (f[2]); used to detect the
// 3px accent left-edge bar and full-width selection fills without asserting on
// theme-resolved colours.
bool has_fill_rect_width(const RecordingCanvas& canvas, float w) {
    for (const auto& c : canvas.commands())
        if (c.type == DrawCommand::Type::fill_rect && c.f[2] == w) return true;
    return false;
}
bool has_glyph(const RecordingCanvas& canvas, const std::string& g) {
    for (const auto& c : canvas.commands())
        if (c.type == DrawCommand::Type::fill_text && c.text == g) return true;
    return false;
}
}  // namespace

// ── ComboBox ─────────────────────────────────────────────────────────────

TEST_CASE("ComboBox set items and select", "[view][combo]") {
    ComboBox combo;
    combo.set_items({"Sine", "Saw", "Square"});
    REQUIRE(combo.items().size() == 3);
    REQUIRE(combo.selected() == 0);

    combo.set_selected(2);
    REQUIRE(combo.selected() == 2);
    REQUIRE(combo.selected_text() == "Square");
}

TEST_CASE("ComboBox on_change fires on selection", "[view][combo]") {
    ComboBox combo;
    combo.set_items({"A", "B", "C"});

    int changed_to = -1;
    combo.on_change = [&](int idx) { changed_to = idx; };

    combo.set_selected(1);
    REQUIRE(changed_to == 1);
}

TEST_CASE("ComboBox out-of-range select is ignored", "[view][combo]") {
    ComboBox combo;
    combo.set_items({"X", "Y"});
    combo.set_selected(0);

    combo.set_selected(5); // out of range
    REQUIRE(combo.selected() == 0);

    combo.set_selected(-1); // negative
    REQUIRE(combo.selected() == 0);
}

TEST_CASE("ComboBox same index re-select is no-op", "[view][combo]") {
    ComboBox combo;
    combo.set_items({"A", "B"});
    combo.set_selected(0);

    int call_count = 0;
    combo.on_change = [&](int) { ++call_count; };

    combo.set_selected(0); // same index
    REQUIRE(call_count == 0);
}

TEST_CASE("ComboBox paint produces draw commands", "[view][combo]") {
    ComboBox combo;
    combo.set_items({"Sine", "Saw"});
    combo.set_bounds({0, 0, 150, 30});

    RecordingCanvas canvas;
    combo.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) > 0);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) > 0);
}

TEST_CASE("ComboBox paint truncates long selected text",
          "[view][combo]") {
    ComboBox combo;
    combo.set_items({"SuperLongOscillatorName"});
    combo.set_bounds({0, 0, 75, 28});

    RecordingCanvas canvas;
    combo.paint(canvas);

    bool truncated = false;
    for (const auto& command : canvas.commands()) {
        if (command.type != DrawCommand::Type::fill_text) continue;
        if (command.text.size() >= 3 &&
            command.text.substr(command.text.size() - 3) == "...") {
            truncated = true;
        }
    }
    REQUIRE(truncated);
}

TEST_CASE("ComboBox key up/down changes selection", "[view][combo]") {
    ComboBox combo;
    combo.set_items({"A", "B", "C"});
    combo.set_selected(1);

    KeyEvent up;
    up.key = KeyCode::up;
    up.is_down = true;
    REQUIRE(combo.on_key_event(up));
    REQUIRE(combo.selected() == 0);

    KeyEvent down;
    down.key = KeyCode::down;
    down.is_down = true;
    REQUIRE(combo.on_key_event(down));
    REQUIRE(combo.selected() == 1);
}

TEST_CASE("ComboBox helper edges and keyboard popup controls", "[view][combo]") {
    ComboBox::close_active_popup();

    ComboBox empty;
    REQUIRE(empty.selected_text().empty());

    ComboBox separator_only;
    separator_only.set_bounds({0, 0, 100, 28});
    separator_only.set_items({"--- very long separator label ignored", "A"});
    REQUIRE(separator_only.dropdown_width_hint() < 150.0f);

    ComboBox combo;
    combo.set_bounds({0, 0, 80, 28});
    combo.set_items({"Alpha", "--- divider", "Beta", "GammaLongName"});

    int changes = 0;
    combo.on_change = [&](int) { ++changes; };

    combo.set_selected_silent(2);
    REQUIRE(combo.selected() == 2);
    REQUIRE(combo.selected_text() == "Beta");
    REQUIRE(changes == 0);
    REQUIRE(combo.dropdown_width_hint() > 80.0f);

    TextInputEvent empty_input;
    combo.on_text_input(empty_input);
    REQUIRE(combo.selected() == 2);

    TextInputEvent match_input;
    match_input.text = "g";
    combo.on_text_input(match_input);
    REQUIRE(combo.selected() == 3);
    REQUIRE(combo.is_open());
    REQUIRE(ComboBox::active_popup_ == &combo);
    REQUIRE(changes == 1);

    KeyEvent escape;
    escape.key = KeyCode::escape;
    escape.is_down = true;
    REQUIRE(combo.on_key_event(escape));
    REQUIRE_FALSE(combo.is_open());

    KeyEvent enter;
    enter.key = KeyCode::enter;
    enter.is_down = true;
    REQUIRE(combo.on_key_event(enter));
    REQUIRE(combo.is_open());
    REQUIRE(combo.on_key_event(enter));
    REQUIRE_FALSE(combo.is_open());

    KeyEvent ignored;
    ignored.key = KeyCode::enter;
    ignored.is_down = false;
    REQUIRE_FALSE(combo.on_key_event(ignored));

    ComboBox::close_active_popup();
}

TEST_CASE("ComboBox popup overlay paints separators hover and selection guards",
          "[view][combo]") {
    ComboBox::close_active_popup();
    View::overlay_queue().clear();

    ComboBox combo;
    combo.set_bounds({0, 0, 96, 28});
    combo.set_items({"One", "---", "Three"});
    combo.set_selected(0);

    MouseEvent open;
    open.position = {48.0f, 14.0f};
    open.is_down = true;
    combo.on_mouse_event(open);
    REQUIRE(combo.is_open());

    MouseEvent separator_click;
    separator_click.position = {48.0f, 66.0f};
    separator_click.is_down = true;
    combo.on_mouse_event(separator_click);
    REQUIRE_FALSE(combo.is_open());
    REQUIRE(combo.selected() == 0);

    combo.on_mouse_event(open);
    REQUIRE(combo.is_open());

    MouseEvent hover;
    hover.position = {48.0f, 82.0f};
    hover.is_down = false;
    combo.on_mouse_event(hover);

    RecordingCanvas canvas;
    combo.paint(canvas);
    REQUIRE(View::overlay_queue().size() == 1);

    auto before_overlay = canvas.command_count();
    View::paint_overlays(canvas);
    REQUIRE(View::overlay_queue().empty());
    REQUIRE(canvas.command_count() > before_overlay);

    ComboBox::close_active_popup();
}

TEST_CASE("ComboBox mouse selection and hover guard paths",
          "[view][combo]") {
    ComboBox::close_active_popup();

    ComboBox combo;
    combo.set_bounds({0, 0, 96, 28});
    combo.set_items({"One", "Two", "Three"});

    int changed_to = -1;
    combo.on_change = [&](int index) { changed_to = index; };

    MouseEvent open;
    open.position = {48.0f, 14.0f};
    open.is_down = true;
    combo.on_mouse_event(open);
    REQUIRE(combo.is_open());

    MouseEvent hover_above;
    hover_above.position = {48.0f, 10.0f};
    hover_above.is_down = false;
    combo.on_mouse_event(hover_above);

    MouseEvent hover_beyond;
    hover_beyond.position = {48.0f, 180.0f};
    hover_beyond.is_down = false;
    combo.on_mouse_event(hover_beyond);

    MouseEvent pick_three;
    pick_three.position = {48.0f, 84.0f};
    pick_three.is_down = true;
    combo.on_mouse_event(pick_three);

    REQUIRE_FALSE(combo.is_open());
    REQUIRE(combo.selected() == 2);
    REQUIRE(changed_to == 2);
}

TEST_CASE("ComboBox returns false for unrelated key presses",
          "[view][combo]") {
    ComboBox combo;
    combo.set_items({"One", "Two"});

    KeyEvent tab;
    tab.key = KeyCode::tab;
    tab.is_down = true;
    REQUIRE_FALSE(combo.on_key_event(tab));
}

TEST_CASE("ComboBox global click closes only outside active popup",
          "[view][combo][issue-493]") {
    ComboBox::close_active_popup();

    ComboBox combo;
    combo.set_items({"One", "Two"});

    KeyEvent open;
    open.key = KeyCode::space;
    open.is_down = true;
    REQUIRE(combo.on_key_event(open));
    REQUIRE(combo.is_open());
    REQUIRE(ComboBox::active_popup_ == &combo);

    ComboBox::notify_global_click(&combo);
    REQUIRE(combo.is_open());

    View outside;
    ComboBox::notify_global_click(&outside);
    REQUIRE_FALSE(combo.is_open());
    REQUIRE(ComboBox::active_popup_ == nullptr);
}

TEST_CASE("ComboBox opening another popup closes the previous one",
          "[view][combo][issue-493]") {
    ComboBox::close_active_popup();

    ComboBox first;
    first.set_items({"One", "Two"});
    ComboBox second;
    second.set_items({"Alpha", "Beta"});

    KeyEvent space;
    space.key = KeyCode::space;
    space.is_down = true;

    REQUIRE(first.on_key_event(space));
    REQUIRE(first.is_open());
    REQUIRE(ComboBox::active_popup_ == &first);

    REQUIRE(second.on_key_event(space));
    REQUIRE_FALSE(first.is_open());
    REQUIRE(second.is_open());
    REQUIRE(ComboBox::active_popup_ == &second);

    int changes = 0;
    second.on_change = [&](int) { ++changes; };
    TextInputEvent no_match;
    no_match.text = "z";
    second.on_text_input(no_match);
    REQUIRE(second.selected() == 0);
    REQUIRE(changes == 0);
    REQUIRE(second.is_open());

    ComboBox::close_active_popup();
}

TEST_CASE("ComboBox destructor clears active popup slot",
          "[view][combo]") {
    ComboBox::close_active_popup();

    {
        ComboBox combo;
        combo.set_items({"One", "Two"});

        KeyEvent space;
        space.key = KeyCode::space;
        space.is_down = true;
        REQUIRE(combo.on_key_event(space));
        REQUIRE(combo.is_open());
        REQUIRE(ComboBox::active_popup_ == &combo);
    }

    REQUIRE(ComboBox::active_popup_ == nullptr);
}

// ── Tooltip ──────────────────────────────────────────────────────────────

TEST_CASE("Tooltip show and hide", "[view][tooltip]") {
    Tooltip tip("Hover text");
    REQUIRE(tip.text() == "Hover text");
    REQUIRE_FALSE(tip.visible());

    tip.show_at({100, 200});
    REQUIRE(tip.visible());

    tip.hide();
    // Tooltip now fades out via animation — advance until opacity settles
    for (int i = 0; i < 30; i++) tip.advance_animations(0.016f);
    REQUIRE_FALSE(tip.visible());
}

TEST_CASE("Tooltip paint renders text", "[view][tooltip]") {
    Tooltip tip("Info");
    tip.set_bounds({0, 0, 100, 22});
    tip.set_visible(true);

    RecordingCanvas canvas;
    tip.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 1);
}

// ── ProgressBar ──────────────────────────────────────────────────────────

TEST_CASE("ProgressBar value clamping on paint", "[view][progress]") {
    ProgressBar bar;
    bar.set_progress(0.5f);
    REQUIRE(bar.progress() == 0.5f);

    bar.set_progress(-1.0f); // indeterminate
    REQUIRE(bar.progress() < 0);
}

TEST_CASE("ProgressBar paint with label", "[view][progress]") {
    ProgressBar bar;
    bar.set_progress(0.75f);
    bar.set_label("75%");
    bar.set_bounds({0, 0, 200, 20});

    RecordingCanvas canvas;
    bar.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) >= 2); // track + fill
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 1);
}

TEST_CASE("ProgressBar paint clamps high progress and skips indeterminate fill",
          "[view][progress][issue-493]") {
    ProgressBar high;
    high.set_bounds({0, 0, 120, 12});
    high.set_progress(2.0f);

    RecordingCanvas high_canvas;
    high.paint(high_canvas);

    bool found_full_width_fill = false;
    for (const auto& command : high_canvas.commands()) {
        if (command.type != DrawCommand::Type::fill_rounded_rect) continue;
        if (command.f[2] == 120.0f && command.f[3] == 12.0f) {
            found_full_width_fill = true;
        }
    }
    REQUIRE(found_full_width_fill);

    ProgressBar indeterminate;
    indeterminate.set_bounds({0, 0, 120, 12});
    indeterminate.set_progress(-1.0f);

    RecordingCanvas indeterminate_canvas;
    indeterminate.paint(indeterminate_canvas);
    REQUIRE(indeterminate_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(indeterminate_canvas.count(DrawCommand::Type::fill_text) == 0);
}

// ── CallOutBox ───────────────────────────────────────────────────────────

TEST_CASE("CallOutBox confirm factory", "[view][callout]") {
    bool confirmed = false;
    auto box = CallOutBox::confirm("Delete?", [&] { confirmed = true; });
    REQUIRE(box != nullptr);
    REQUIRE(box->message() == "Delete?");

    KeyEvent enter;
    enter.key = KeyCode::enter;
    enter.is_down = true;
    box->on_key_event(enter);
    REQUIRE(confirmed);
}

TEST_CASE("CallOutBox escape triggers cancel", "[view][callout]") {
    bool cancelled = false;
    auto box = CallOutBox::confirm("Sure?", {}, [&] { cancelled = true; });

    KeyEvent esc;
    esc.key = KeyCode::escape;
    esc.is_down = true;
    box->on_key_event(esc);
    REQUIRE(cancelled);
}

TEST_CASE("CallOutBox notify factory sets auto-dismiss", "[view][callout]") {
    auto box = CallOutBox::notify("Saved!", 2.0f);
    REQUIRE(box->message() == "Saved!");
    REQUIRE(box->auto_dismiss_seconds == 2.0f);
}

TEST_CASE("CallOutBox ignores key-up and unrelated keys",
          "[view][callout][issue-493]") {
    bool confirmed = false;
    bool cancelled = false;
    auto box = CallOutBox::confirm("Apply?", [&] { confirmed = true; },
                                   [&] { cancelled = true; });

    KeyEvent enter_up;
    enter_up.key = KeyCode::enter;
    enter_up.is_down = false;
    REQUIRE_FALSE(box->on_key_event(enter_up));

    KeyEvent space;
    space.key = KeyCode::space;
    space.is_down = true;
    REQUIRE_FALSE(box->on_key_event(space));

    REQUIRE_FALSE(confirmed);
    REQUIRE_FALSE(cancelled);

    box->set_bounds({0, 0, 160, 40});
    RecordingCanvas canvas;
    box->paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_rounded_rect) == 1);
}

// ── TabPanel ─────────────────────────────────────────────────────────────

TEST_CASE("TabPanel add tabs and switch", "[view][tabs]") {
    TabPanel tabs;
    tabs.set_bounds({0, 0, 400, 300});

    auto page1 = std::make_unique<View>();
    auto page2 = std::make_unique<View>();

    tabs.add_tab("Basic", std::move(page1));
    tabs.add_tab("Advanced", std::move(page2));

    REQUIRE(tabs.tab_count() == 2);
    REQUIRE(tabs.active_tab() == 0);

    // First tab's content should be visible
    REQUIRE(tabs.child_at(0)->visible());
    REQUIRE_FALSE(tabs.child_at(1)->visible());

    tabs.set_active_tab(1);
    REQUIRE(tabs.active_tab() == 1);
    REQUIRE_FALSE(tabs.child_at(0)->visible());
    REQUIRE(tabs.child_at(1)->visible());
}

TEST_CASE("TabPanel on_tab_change callback", "[view][tabs]") {
    TabPanel tabs;
    tabs.add_tab("A", std::make_unique<View>());
    tabs.add_tab("B", std::make_unique<View>());

    int changed_to = -1;
    tabs.on_tab_change = [&](int idx) { changed_to = idx; };

    tabs.set_active_tab(1);
    REQUIRE(changed_to == 1);
}

TEST_CASE("TabPanel out-of-range ignored", "[view][tabs]") {
    TabPanel tabs;
    tabs.add_tab("Only", std::make_unique<View>());

    tabs.set_active_tab(5);
    REQUIRE(tabs.active_tab() == 0);

    tabs.set_active_tab(-1);
    REQUIRE(tabs.active_tab() == 0);
}

TEST_CASE("TabPanel mouse selection paint and empty guard",
          "[view][tabs][issue-493]") {
    TabPanel empty;
    empty.set_bounds({0, 0, 0, 60});
    MouseEvent empty_click;
    empty_click.position = {0.0f, 0.0f};
    empty_click.is_down = true;
    empty.on_mouse_event(empty_click);
    REQUIRE(empty.active_tab() == 0);

    TabPanel tabs;
    tabs.set_bounds({0, 0, 200, 120});
    tabs.add_tab("One", std::make_unique<View>());
    tabs.add_tab("Two", std::make_unique<View>());

    MouseEvent below_bar;
    below_bar.position = {150.0f, 40.0f};
    below_bar.is_down = true;
    tabs.on_mouse_event(below_bar);
    REQUIRE(tabs.active_tab() == 0);

    MouseEvent second_tab;
    second_tab.position = {150.0f, 12.0f};
    second_tab.is_down = true;
    tabs.on_mouse_event(second_tab);
    REQUIRE(tabs.active_tab() == 1);

    RecordingCanvas canvas;
    tabs.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 2);
    // Ink & Signal tab bar: tab-bar background + the active tab's teal underline
    // (active state is an underline now, not a filled block).
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) >= 2);
}

// ── ScrollView ───────────────────────────────────────────────────────────

TEST_CASE("ScrollView scroll clamping", "[view][scroll]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 100});
    sv.set_content_size({200, 500});

    sv.set_scroll(0, 300);
    REQUIRE(sv.scroll_y() == 300.0f);

    sv.set_scroll(0, 600); // beyond content
    REQUIRE(sv.scroll_y() == 400.0f); // 500 - 100
}

TEST_CASE("ScrollView scroll_x clamped", "[view][scroll]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 100, 100});
    sv.set_content_size({300, 100});
    sv.set_direction(ScrollView::Direction::horizontal);

    sv.set_scroll(250, 0);
    REQUIRE(sv.scroll_x() == 200.0f); // 300 - 100

    sv.set_scroll(-10, 0);
    REQUIRE(sv.scroll_x() == 0.0f);
}

TEST_CASE("ScrollView scrollbar paint hit test and drag update offsets",
          "[view][scroll]") {
    ScrollView sv;
    sv.set_direction(ScrollView::Direction::both);
    sv.set_bounds({0, 0, 100, 100});
    sv.set_content_size({300, 500});
    sv.set_scroll(50, 100);
    sv.on_mouse_enter();
    sv.advance_animations(1.0f);

    RecordingCanvas canvas;
    sv.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) >= 2);
    REQUIRE(sv.hit_test({98.0f, 25.0f}) == &sv);
    REQUIRE(sv.hit_test({25.0f, 98.0f}) == &sv);

    MouseEvent vertical_down;
    vertical_down.position = {98.0f, 25.0f};
    vertical_down.is_down = true;
    vertical_down.button = MouseButton::left;
    sv.on_mouse_event(vertical_down);
    sv.on_mouse_drag({98.0f, 85.0f});
    REQUIRE(sv.scroll_y() > 300.0f);

    MouseEvent up;
    up.is_down = false;
    sv.on_mouse_event(up);

    MouseEvent horizontal_down;
    horizontal_down.position = {25.0f, 98.0f};
    horizontal_down.is_down = true;
    horizontal_down.button = MouseButton::left;
    sv.on_mouse_event(horizontal_down);
    sv.on_mouse_drag({90.0f, 98.0f});
    REQUIRE(sv.scroll_x() > 150.0f);

    sv.on_mouse_event(up);
}

TEST_CASE("ScrollView wheel respects horizontal direction and track clicks",
          "[view][scroll]") {
    ScrollView horizontal;
    horizontal.set_direction(ScrollView::Direction::horizontal);
    horizontal.set_bounds({0, 0, 100, 100});
    horizontal.set_content_size({300, 500});

    MouseEvent wheel;
    wheel.is_wheel = true;
    wheel.scroll_delta_x = 40.0f;
    wheel.scroll_delta_y = 40.0f;
    horizontal.on_mouse_event(wheel);
    horizontal.advance_animations(1.0f);
    REQUIRE(horizontal.scroll_x() > 0.0f);
    REQUIRE(horizontal.scroll_y() == 0.0f);
    REQUIRE(horizontal.bar_opacity() == 0.6f);
    REQUIRE(horizontal.bar_width() == 6.0f);

    ScrollView track_click;
    track_click.set_bounds({0, 0, 100, 100});
    track_click.set_content_size({100, 500});

    MouseEvent down;
    down.position = {98.0f, 90.0f};
    down.is_down = true;
    down.button = MouseButton::left;
    track_click.on_mouse_event(down);
    REQUIRE(track_click.target_scroll_y() > 0.0f);
}

TEST_CASE("ScrollView vertical wheel leave and horizontal track click edges",
          "[view][scroll][issue-493]") {
    ScrollView vertical;
    vertical.set_direction(ScrollView::Direction::vertical);
    vertical.set_bounds({0, 0, 100, 100});
    vertical.set_content_size({300, 500});

    MouseEvent wheel;
    wheel.is_wheel = true;
    wheel.scroll_delta_x = 40.0f;
    wheel.scroll_delta_y = 40.0f;
    vertical.on_mouse_event(wheel);
    vertical.advance_animations(1.0f);
    REQUIRE(vertical.scroll_x() == 0.0f);
    REQUIRE(vertical.scroll_y() > 0.0f);

    vertical.on_mouse_enter();
    vertical.advance_animations(1.0f);
    REQUIRE(vertical.bar_width() == 8.0f);
    vertical.on_mouse_leave();
    vertical.advance_animations(1.0f);
    REQUIRE(vertical.bar_width() == 4.0f);

    ScrollView horizontal_track;
    horizontal_track.set_direction(ScrollView::Direction::horizontal);
    horizontal_track.set_bounds({0, 0, 100, 100});
    horizontal_track.set_content_size({300, 100});

    MouseEvent down;
    down.position = {90.0f, 98.0f};
    down.is_down = true;
    down.button = MouseButton::left;
    horizontal_track.on_mouse_event(down);
    horizontal_track.advance_animations(1.0f);
    REQUIRE(horizontal_track.scroll_x() > 0.0f);
}

TEST_CASE("ScrollView auto scroll behavior updates immediately",
          "[view][scroll]") {
    ScrollView sv;
    sv.set_direction(ScrollView::Direction::both);
    sv.set_bounds({0, 0, 100, 100});
    sv.set_content_size({300, 300});
    sv.set_scroll_behavior("auto");

    sv.scroll_by(40.0f, 60.0f);

    REQUIRE(sv.scroll_x() == 40.0f);
    REQUIRE(sv.scroll_y() == 60.0f);
    REQUIRE_FALSE(sv.scroll_animating());
}

TEST_CASE("ScrollView layout children restores viewport bounds",
          "[view][scroll]") {
    ScrollView sv;
    sv.set_bounds({3, 4, 100, 50});
    sv.set_content_size({220, 180});
    sv.add_child(std::make_unique<View>());

    sv.layout_children();

    REQUIRE(sv.bounds().x == 3.0f);
    REQUIRE(sv.bounds().y == 4.0f);
    REQUIRE(sv.bounds().width == 100.0f);
    REQUIRE(sv.bounds().height == 50.0f);
}

TEST_CASE("ScrollView paint_all applies opacity reset path",
          "[view][scroll]") {
    ScrollView sv;
    sv.set_direction(ScrollView::Direction::both);
    sv.set_bounds({0, 0, 100, 50});
    sv.set_content_size({200, 150});
    sv.set_scroll(10, 20);
    sv.set_opacity(0.5f);

    RecordingCanvas canvas;
    sv.paint_all(canvas);
    REQUIRE(canvas.command_count() > 0);
}

TEST_CASE("ScrollView box-none suppresses scrollbar self hits",
          "[view][scroll]") {
    ScrollView sv;
    sv.set_direction(ScrollView::Direction::both);
    sv.set_bounds({0, 0, 100, 100});
    sv.set_content_size({300, 500});
    sv.on_mouse_enter();
    sv.advance_animations(1.0f);
    sv.set_pointer_events(View::PointerEvents::box_none);

    REQUIRE(sv.hit_test({98.0f, 25.0f}) == nullptr);
}

TEST_CASE("ScrollView both-direction wheel preserves both deltas",
          "[view][scroll]") {
    ScrollView sv;
    sv.set_direction(ScrollView::Direction::both);
    sv.set_bounds({0, 0, 100, 100});
    sv.set_content_size({300, 500});

    MouseEvent wheel;
    wheel.is_wheel = true;
    wheel.scroll_delta_x = 25.0f;
    wheel.scroll_delta_y = 35.0f;
    sv.on_mouse_event(wheel);
    sv.advance_animations(1.0f);

    REQUIRE(sv.scroll_x() > 0.0f);
    REQUIRE(sv.scroll_y() > 0.0f);
}

TEST_CASE("ScrollView hit testing honors pointer modes with scrolled children",
          "[view][scroll][issue-493]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 100, 100});
    sv.set_content_size({100, 300});

    auto child = std::make_unique<View>();
    child->set_bounds({10, 150, 20, 20});
    auto* child_ptr = child.get();
    sv.add_child(std::move(child));
    sv.set_scroll(0, 100);

    REQUIRE(sv.hit_test({20, 60}) == child_ptr);

    sv.set_pointer_events(View::PointerEvents::box_only);
    REQUIRE(sv.hit_test({20, 60}) == &sv);

    sv.set_pointer_events(View::PointerEvents::box_none);
    REQUIRE(sv.hit_test({20, 60}) == child_ptr);
    REQUIRE(sv.hit_test({2, 2}) == nullptr);

    sv.set_pointer_events(View::PointerEvents::none);
    REQUIRE(sv.hit_test({20, 60}) == nullptr);
}

TEST_CASE("ScrollView paint_all clips translated content and skips invisible views",
          "[view][scroll][issue-493]") {
    ScrollView hidden;
    hidden.set_visible(false);
    RecordingCanvas hidden_canvas;
    hidden.paint_all(hidden_canvas);
    REQUIRE(hidden_canvas.command_count() == 0);

    ScrollView sv;
    sv.set_direction(ScrollView::Direction::both);
    sv.set_bounds({5, 7, 100, 50});
    sv.set_content_size({200, 150});
    sv.set_scroll(10, 20);

    auto child = std::make_unique<View>();
    child->set_bounds({0, 0, 20, 20});
    sv.add_child(std::move(child));

    RecordingCanvas canvas;
    sv.paint_all(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::save) >= 2);
    REQUIRE(canvas.count(DrawCommand::Type::clip_rect) >= 2);
    REQUIRE(canvas.count(DrawCommand::Type::translate) >= 2);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) >= 2);
}

// ── ListBox ──────────────────────────────────────────────────────────────

TEST_CASE("ListBox set items and select", "[view][listbox]") {
    ListBox list;
    list.set_items({"Alpha", "Beta", "Gamma"});
    REQUIRE(list.items().size() == 3);
    REQUIRE(list.selected() == -1); // nothing selected by default

    list.set_selected(1);
    REQUIRE(list.selected() == 1);
}

TEST_CASE("ListBox on_select fires", "[view][listbox]") {
    ListBox list;
    list.set_items({"A", "B"});

    int selected = -1;
    list.on_select = [&](int idx) { selected = idx; };

    list.set_selected(0);
    REQUIRE(selected == 0);
}

TEST_CASE("ListBox key navigation", "[view][listbox]") {
    ListBox list;
    list.set_items({"A", "B", "C"});
    list.set_selected(1);

    KeyEvent down;
    down.key = KeyCode::down;
    down.is_down = true;
    REQUIRE(list.on_key_event(down));
    REQUIRE(list.selected() == 2);

    KeyEvent up;
    up.key = KeyCode::up;
    up.is_down = true;
    REQUIRE(list.on_key_event(up));
    REQUIRE(list.selected() == 1);
}

TEST_CASE("ListBox key up at top doesn't go negative", "[view][listbox]") {
    ListBox list;
    list.set_items({"A", "B"});
    list.set_selected(0);

    KeyEvent up;
    up.key = KeyCode::up;
    up.is_down = true;
    REQUIRE_FALSE(list.on_key_event(up));
    REQUIRE(list.selected() == 0);
}

TEST_CASE("ListBox paint with selection", "[view][listbox]") {
    ListBox list;
    list.set_items({"Alpha", "Beta"});
    list.set_selected(0);
    list.set_bounds({0, 0, 200, 100});

    RecordingCanvas canvas;
    list.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) > 0); // selection highlight
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) > 0);
}

TEST_CASE("ListBox wheel scrolling double-click and enter activation",
          "[view][listbox]") {
    ListBox list;
    list.set_bounds({0, 0, 120, 48});
    list.set_items({"Alpha", "Beta", "Gamma", "Delta", "Epsilon"});

    int selects = 0;
    int activated = -1;
    list.on_select = [&](int) { ++selects; };
    list.on_activate = [&](int index) { activated = index; };

    list.set_selected(1);
    list.set_selected(1);
    REQUIRE(selects == 1);

    MouseEvent wheel;
    wheel.is_wheel = true;
    wheel.scroll_delta_y = 48.0f;
    list.on_mouse_event(wheel);

    MouseEvent double_click;
    double_click.position = {10.0f, 0.0f};
    double_click.is_down = true;
    double_click.click_count = 2;
    list.on_mouse_event(double_click);
    REQUIRE(list.selected() == 2);
    REQUIRE(activated == 2);

    KeyEvent enter;
    enter.key = KeyCode::enter;
    enter.is_down = true;
    REQUIRE(list.on_key_event(enter));
    REQUIRE(activated == 2);

    RecordingCanvas canvas;
    list.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
}

TEST_CASE("ListBox ignores boundary keys and out-of-range mouse presses",
          "[view][listbox][issue-493]") {
    ListBox list;
    list.set_bounds({0, 0, 120, 48});
    list.set_items({"Alpha", "Beta"});
    list.set_selected(1);

    int selects = 0;
    list.on_select = [&](int) { ++selects; };

    KeyEvent down;
    down.key = KeyCode::down;
    down.is_down = true;
    REQUIRE_FALSE(list.on_key_event(down));
    REQUIRE(list.selected() == 1);

    KeyEvent enter;
    enter.key = KeyCode::enter;
    enter.is_down = true;
    REQUIRE_FALSE(list.on_key_event(enter));

    KeyEvent up_released;
    up_released.key = KeyCode::up;
    up_released.is_down = false;
    REQUIRE_FALSE(list.on_key_event(up_released));

    MouseEvent far_click;
    far_click.position = {10.0f, 500.0f};
    far_click.is_down = true;
    list.on_mouse_event(far_click);
    REQUIRE(list.selected() == 1);
    REQUIRE(selects == 0);
}

// ── SegmentedControl ───────────────────────────────────────────────────────

TEST_CASE("SegmentedControl selection + on_change", "[view][segmented]") {
    SegmentedControl seg;
    seg.set_segments({"Amp", "EQ", "Comp", "Reverb"});
    REQUIRE(seg.segments().size() == 4);
    REQUIRE(seg.selected() == 0);

    int changed = -1;
    seg.on_change = [&](int i) { changed = i; };
    seg.set_selected(2);
    REQUIRE(seg.selected() == 2);
    REQUIRE(changed == 2);

    // set_selected_silent does not notify
    changed = -1;
    seg.set_selected_silent(1);
    REQUIRE(seg.selected() == 1);
    REQUIRE(changed == -1);

    // out-of-range clamps to the last segment
    seg.set_selected(99);
    REQUIRE(seg.selected() == 3);
}

TEST_CASE("SegmentedControl click selects the segment under the cursor",
          "[view][segmented]") {
    SegmentedControl seg;
    seg.set_segments({"A", "B", "C", "D"});
    seg.set_bounds({0, 0, 200, 28});   // four 50px-wide segments

    int changed = -1;
    seg.on_change = [&](int i) { changed = i; };

    MouseEvent e; e.position = {120.0f, 14.0f}; e.is_down = true;
    seg.on_mouse_event(e);             // x=120 → segment index 2
    REQUIRE(seg.selected() == 2);
    REQUIRE(changed == 2);
}

TEST_CASE("SegmentedControl arrow keys move the selection", "[view][segmented]") {
    SegmentedControl seg;
    seg.set_segments({"A", "B", "C"});
    seg.set_selected(1);

    KeyEvent right; right.key = KeyCode::right; right.is_down = true;
    REQUIRE(seg.on_key_event(right));
    REQUIRE(seg.selected() == 2);

    KeyEvent left; left.key = KeyCode::left; left.is_down = true;
    REQUIRE(seg.on_key_event(left));
    REQUIRE(seg.selected() == 1);
}

TEST_CASE("SegmentedControl paint: one label per segment + active pill",
          "[view][segmented]") {
    SegmentedControl seg;
    seg.set_segments({"Amp", "EQ", "Comp"});
    seg.set_selected(1);
    seg.set_bounds({0, 0, 240, 28});

    RecordingCanvas canvas;
    seg.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 3);          // labels
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) >= 2);  // track + pill
    REQUIRE(canvas.count(DrawCommand::Type::stroke_rounded_rect) >= 1);// pill border
}

// ── TabPanel underline variant ──────────────────────────────────────────────

TEST_CASE("TabPanel defaults to the filled tab bar", "[view][tab]") {
    TabPanel t;
    REQUIRE(t.tab_bar_style() == TabPanel::TabBarStyle::filled);
}

TEST_CASE("TabPanel underline style: no strip, full-width divider, teal marker",
          "[view][tab]") {
    auto make = [](TabPanel::TabBarStyle s) {
        auto t = std::make_unique<TabPanel>();
        t->set_tab_bar_style(s);
        t->add_tab("Amp", std::make_unique<View>());
        t->add_tab("Filter", std::make_unique<View>());
        t->set_active_tab(0);
        t->set_bounds({0, 0, 300, 80});
        return t;
    };

    {   // filled — a full-width strip fill_rect at the top of the bar
        auto t = make(TabPanel::TabBarStyle::filled);
        RecordingCanvas canvas; t->paint(canvas);
        bool strip = false;
        for (const auto& c : canvas.commands())
            if (c.type == DrawCommand::Type::fill_rect &&
                c.f[1] == 0.0f && c.f[2] >= 299.0f) strip = true;
        REQUIRE(strip);
    }
    {   // underline — no strip, but a full-width divider + the 2px teal marker
        auto t = make(TabPanel::TabBarStyle::underline);
        RecordingCanvas canvas; t->paint(canvas);
        bool strip = false, divider = false, marker = false;
        for (const auto& c : canvas.commands()) {
            if (c.type == DrawCommand::Type::fill_rect &&
                c.f[1] == 0.0f && c.f[2] >= 299.0f) strip = true;
            if (c.type == DrawCommand::Type::stroke_line &&
                c.f[0] <= 1.0f && c.f[2] >= 299.0f) divider = true;
            if (c.type == DrawCommand::Type::fill_rect && c.f[3] == 2.0f) marker = true;
        }
        REQUIRE_FALSE(strip);
        REQUIRE(divider);
        REQUIRE(marker);
    }
}

// ── ListBox accent selection + icons ────────────────────────────────────────

TEST_CASE("ListBox defaults to the standard selection style", "[view][listbox]") {
    ListBox list;
    REQUIRE(list.selection_style() == ListBox::SelectionStyle::standard);
}

TEST_CASE("ListBox accent selection draws a 3px left bar + leading icon",
          "[view][listbox]") {
    ListBox list;
    list.set_items({"Osc", "Filter", "Env"});
    list.set_icons({"\xe2\x99\xaa", "\xe2\x99\xaa", "\xe2\x99\xaa"});
    list.set_selection_style(ListBox::SelectionStyle::accent);
    list.set_selected(1);
    list.set_bounds({0, 0, 200, 120});

    RecordingCanvas canvas; list.paint(canvas);
    REQUIRE(has_fill_rect_width(canvas, 3.0f));   // teal left-edge bar
    REQUIRE(has_glyph(canvas, "\xe2\x99\xaa"));    // icon glyph drawn
}

TEST_CASE("ListBox standard selection: full-width fill, no 3px bar",
          "[view][listbox]") {
    ListBox list;
    list.set_items({"A", "B", "C"});
    list.set_selected(1);
    list.set_bounds({0, 0, 200, 120});

    RecordingCanvas canvas; list.paint(canvas);
    REQUIRE_FALSE(has_fill_rect_width(canvas, 3.0f));   // no accent bar
    REQUIRE(has_fill_rect_width(canvas, 200.0f));       // full-width selected row
}

// ── Breadcrumb flat variant ─────────────────────────────────────────────────

TEST_CASE("Breadcrumb defaults to showing its background card",
          "[view][breadcrumb]") {
    Breadcrumb b;
    REQUIRE(b.show_background());
}

TEST_CASE("Breadcrumb show_background toggles the card", "[view][breadcrumb]") {
    auto card_count = [](bool bg) {
        Breadcrumb b;
        b.set_show_background(bg);
        b.set_items({{"A", {}}, {"B", {}}});
        b.set_bounds({0, 0, 200, 32});
        RecordingCanvas canvas; b.paint(canvas);
        return canvas.count(DrawCommand::Type::fill_rounded_rect);
    };
    REQUIRE(card_count(true) >= 1);    // card present
    REQUIRE(card_count(false) == 0);   // flat — no card
}
