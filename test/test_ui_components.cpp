#include <catch2/catch_test_macros.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/canvas/canvas.hpp>

using namespace pulp::view;
using namespace pulp::canvas;

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

TEST_CASE("ComboBox helper edges and keyboard popup controls", "[view][combo][coverage]") {
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
          "[view][combo][coverage]") {
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
          "[view][scroll][coverage]") {
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
          "[view][scroll][coverage]") {
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
          "[view][listbox][coverage]") {
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
