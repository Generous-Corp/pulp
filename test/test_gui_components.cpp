#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/table.hpp>
#include <pulp/view/toolbar.hpp>
#include <pulp/view/concertina_panel.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/lasso.hpp>
#include <memory>

using namespace pulp::canvas;
using namespace pulp::view;

namespace {

class ToolbarProbeView final : public View {
public:
    int mouse_down_count = 0;
    int paint_count = 0;
    Point last_position{-1.0f, -1.0f};

    void on_mouse_down(Point pos) override {
        ++mouse_down_count;
        last_position = pos;
    }

    void paint(pulp::canvas::Canvas& canvas) override {
        ++paint_count;
        canvas.set_fill_color(pulp::canvas::Color::rgba8(1, 2, 3));
        canvas.fill_rect(0, 0, bounds().width, bounds().height);
    }
};

class PanelProbeView final : public View {
public:
    explicit PanelProbeView(float intrinsic_height) : intrinsic_height_(intrinsic_height) {}

    int paint_count = 0;

    float intrinsic_height() const override { return intrinsic_height_; }

    void paint(pulp::canvas::Canvas& canvas) override {
        ++paint_count;
        canvas.set_fill_color(pulp::canvas::Color::rgba8(4, 5, 6));
        canvas.fill_rect(0, 0, bounds().width, bounds().height);
    }

private:
    float intrinsic_height_ = 0.0f;
};

class TrackingTableModel final : public TableModel {
public:
    explicit TrackingTableModel(std::vector<std::vector<std::string>> rows)
        : rows_(std::move(rows)) {}

    int sort_calls = 0;
    int selected_calls = 0;
    int last_sort_column = -1;
    int last_selected_row = -1;
    bool last_sort_ascending = false;

    int row_count() const override { return static_cast<int>(rows_.size()); }

    std::string cell_text(int row, int column) const override {
        if (row < 0 || row >= row_count())
            return "";
        const auto& row_values = rows_[static_cast<size_t>(row)];
        if (column < 0 || column >= static_cast<int>(row_values.size()))
            return "";
        return row_values[static_cast<size_t>(column)];
    }

    bool sort(int column, bool ascending) override {
        ++sort_calls;
        last_sort_column = column;
        last_sort_ascending = ascending;
        const auto col = static_cast<size_t>(column);
        std::sort(rows_.begin(), rows_.end(), [col, ascending](const auto& a, const auto& b) {
            const std::string av = col < a.size() ? a[col] : "";
            const std::string bv = col < b.size() ? b[col] : "";
            return ascending ? av < bv : av > bv;
        });
        return true;
    }

    void row_selected(int row) override {
        ++selected_calls;
        last_selected_row = row;
    }

private:
    std::vector<std::vector<std::string>> rows_;
};

}  // namespace

// ── SimpleTableModel ────────────────────────────────────────────────────

TEST_CASE("SimpleTableModel stores and retrieves data", "[gui][table]") {
    SimpleTableModel model;
    model.add_row({"Alice", "25", "Engineer"});
    model.add_row({"Bob", "30", "Designer"});
    model.add_row({"Charlie", "22", "Intern"});

    REQUIRE(model.row_count() == 3);
    REQUIRE(model.cell_text(0, 0) == "Alice");
    REQUIRE(model.cell_text(1, 1) == "30");
    REQUIRE(model.cell_text(2, 2) == "Intern");
}

TEST_CASE("SimpleTableModel sorts by column", "[gui][table]") {
    SimpleTableModel model;
    model.add_row({"Charlie", "22"});
    model.add_row({"Alice", "25"});
    model.add_row({"Bob", "30"});

    REQUIRE(model.sort(0, true));
    REQUIRE(model.cell_text(0, 0) == "Alice");
    REQUIRE(model.cell_text(1, 0) == "Bob");
    REQUIRE(model.cell_text(2, 0) == "Charlie");

    REQUIRE(model.sort(0, false));
    REQUIRE(model.cell_text(0, 0) == "Charlie");
}

TEST_CASE("SimpleTableModel out-of-bounds returns empty", "[gui][table]") {
    SimpleTableModel model;
    model.add_row({"a"});
    REQUIRE(model.cell_text(-1, 0) == "");
    REQUIRE(model.cell_text(0, 5) == "");
    REQUIRE(model.cell_text(99, 0) == "");
}

TEST_CASE("TableListBox selection", "[gui][table]") {
    TableListBox table;
    SimpleTableModel model;
    model.add_row({"Row 0"});
    model.add_row({"Row 1"});
    model.add_row({"Row 2"});

    table.set_model(&model);
    table.add_column({"Name", 200.0f});

    REQUIRE(table.selected_row() == -1);
    table.set_selected_row(1);
    REQUIRE(table.selected_row() == 1);
}

TEST_CASE("TableListBox column management", "[gui][table]") {
    TableListBox table;
    REQUIRE(table.column_count() == 0);

    table.add_column({"Name", 150.0f});
    table.add_column({"Value", 100.0f});
    REQUIRE(table.column_count() == 2);

    table.clear_columns();
    REQUIRE(table.column_count() == 0);
}

TEST_CASE("TableListBox header sorting and row selection edge paths",
          "[gui][table][issue-493]") {
    TrackingTableModel model({
        {"Charlie", "Synth", "3"},
        {"Alice", "Bass", "1"},
        {"Bob", "Lead", "2"},
    });

    TableListBox table;
    table.set_bounds({0, 0, 180, 120});
    table.set_model(&model);
    table.add_column({"Name", 120.0f, true});
    table.add_column({"Type", 120.0f, false});
    table.add_column({"Rank", 60.0f, true});

    table.on_mouse_down({20, 8});
    REQUIRE(model.sort_calls == 1);
    REQUIRE(model.last_sort_column == 0);
    REQUIRE(model.last_sort_ascending);
    REQUIRE(model.cell_text(0, 0) == "Alice");

    table.on_mouse_down({20, 8});
    REQUIRE(model.sort_calls == 2);
    REQUIRE(model.last_sort_column == 0);
    REQUIRE_FALSE(model.last_sort_ascending);
    REQUIRE(model.cell_text(0, 0) == "Charlie");

    table.on_mouse_down({90, 8});
    REQUIRE(model.sort_calls == 2);

    table.on_mouse_down({160, 8});
    REQUIRE(model.sort_calls == 3);
    REQUIRE(model.last_sort_column == 2);
    REQUIRE(model.last_sort_ascending);

    int selected = -1;
    table.on_selection_changed = [&](int row) { selected = row; };
    table.on_mouse_down({15, 58});
    REQUIRE(table.selected_row() == 1);
    REQUIRE(selected == 1);
    REQUIRE(model.selected_calls == 1);
    REQUIRE(model.last_selected_row == 1);

    table.on_mouse_down({15, 400});
    REQUIRE(table.selected_row() == 1);
    REQUIRE(model.selected_calls == 1);
}

TEST_CASE("TableListBox paint covers empty guards scaled columns and alignment",
          "[gui][table][issue-493]") {
    TableListBox empty;
    empty.set_bounds({0, 0, 120, 80});
    RecordingCanvas canvas;
    empty.paint(canvas);
    REQUIRE(canvas.command_count() == 0);

    TrackingTableModel model({
        {"Alice", "Bass", "10"},
        {"Bob", "Lead", "2"},
        {"Charlie", "Pad", "30"},
    });

    TableListBox table;
    table.set_bounds({0, 0, 160, 86});
    table.set_model(&model);
    table.add_column({"Name", 120.0f, true, true, TableColumn::Align::left});
    table.add_column({"Type", 100.0f, true, true, TableColumn::Align::center});
    table.add_column({"Rank", 100.0f, true, true, TableColumn::Align::right});
    table.set_selected_row(1);
    table.on_mouse_down({12, 8});

    table.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) >= 4);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) >= 4);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 13);

    bool saw_name_header = false;
    bool saw_rank_header = false;
    bool saw_selected_cell = false;
    bool saw_sort_indicator = false;
    for (const auto& command : canvas.commands()) {
        if (command.type != DrawCommand::Type::fill_text)
            continue;
        saw_name_header = saw_name_header || command.text == "Name";
        saw_rank_header = saw_rank_header || command.text == "Rank";
        saw_selected_cell = saw_selected_cell || command.text == "Bob";
        saw_sort_indicator = saw_sort_indicator || command.text == "\xe2\x96\xb2";
    }

    REQUIRE(saw_name_header);
    REQUIRE(saw_rank_header);
    REQUIRE(saw_selected_cell);
    REQUIRE(saw_sort_indicator);
}

// ── Toolbar ─────────────────────────────────────────────────────────────

TEST_CASE("Toolbar add items", "[gui][toolbar]") {
    Toolbar toolbar;
    REQUIRE(toolbar.item_count() == 0);

    toolbar.add_button("play", "Play", []() {});
    toolbar.add_separator();
    toolbar.add_toggle("loop", "Loop", [](bool) {});
    toolbar.add_spacer();

    REQUIRE(toolbar.item_count() == 4);
}

TEST_CASE("Toolbar toggle state", "[gui][toolbar]") {
    Toolbar toolbar;
    toolbar.add_toggle("mute", "Mute", [](bool) {});

    REQUIRE_FALSE(toolbar.is_toggled("mute"));
    toolbar.set_toggled("mute", true);
    REQUIRE(toolbar.is_toggled("mute"));
}

TEST_CASE("Toolbar enable/disable", "[gui][toolbar]") {
    Toolbar toolbar;
    bool clicked = false;
    toolbar.add_button("save", "Save", [&]() { clicked = true; });

    toolbar.set_enabled("save", false);
    // Disabled items shouldn't trigger (tested via on_mouse_down in real usage)
    toolbar.set_enabled("missing", false);
    toolbar.on_mouse_down({10, 10});
    REQUIRE_FALSE(clicked);
}

TEST_CASE("Toolbar remove item", "[gui][toolbar]") {
    Toolbar toolbar;
    toolbar.add_button("a", "A", []() {});
    toolbar.add_button("b", "B", []() {});
    REQUIRE(toolbar.item_count() == 2);

    toolbar.remove_item("missing");
    REQUIRE(toolbar.item_count() == 2);

    toolbar.remove_item("a");
    REQUIRE(toolbar.item_count() == 1);
}

TEST_CASE("Toolbar missing ids and sizing fallbacks are stable",
          "[gui][toolbar][coverage][issue-653]") {
    Toolbar toolbar;
    toolbar.add_toggle("solo", "Solo", [](bool) {});

    toolbar.set_toggled("missing", true);
    toolbar.set_enabled("missing", false);
    REQUIRE_FALSE(toolbar.is_toggled("missing"));
    REQUIRE_FALSE(toolbar.is_toggled("solo"));

    toolbar.set_item_size(18.0f);
    toolbar.set_spacing(2.0f);
    REQUIRE(toolbar.intrinsic_height() == 26.0f);

    toolbar.set_orientation(Toolbar::Orientation::vertical);
    REQUIRE(toolbar.intrinsic_height() == 0.0f);

    int clicks = 0;
    toolbar.add_button("go", "Go", [&] { ++clicks; });
    toolbar.on_mouse_down({40, 40});
    REQUIRE(clicks == 0);
}

TEST_CASE("Toolbar mouse interaction skips separators and disabled items",
          "[gui][toolbar][coverage]") {
    Toolbar toolbar;
    int play_clicks = 0;
    int save_clicks = 0;
    bool loop_state = false;
    int loop_calls = 0;

    toolbar.add_button("play", "Play", [&]() { ++play_clicks; });
    toolbar.add_separator();
    toolbar.add_toggle("loop", "Loop", [&](bool state) {
        loop_state = state;
        ++loop_calls;
    });
    toolbar.add_spacer();
    toolbar.add_button("save", "Save", [&]() { ++save_clicks; });
    toolbar.set_bounds({0, 0, 200, 40});

    toolbar.on_mouse_down({10, 12});
    REQUIRE(play_clicks == 1);

    toolbar.on_mouse_down({38, 12});
    REQUIRE(loop_calls == 0);

    toolbar.on_mouse_down({50, 12});
    REQUIRE(toolbar.is_toggled("loop"));
    REQUIRE(loop_state);
    REQUIRE(loop_calls == 1);

    toolbar.set_enabled("loop", false);
    toolbar.on_mouse_down({50, 12});
    REQUIRE(toolbar.is_toggled("loop"));
    REQUIRE(loop_calls == 1);

    toolbar.set_enabled("save", false);
    toolbar.on_mouse_down({100, 12});
    REQUIRE(save_clicks == 0);

    toolbar.set_enabled("save", true);
    toolbar.on_mouse_down({100, 12});
    REQUIRE(save_clicks == 1);
}

TEST_CASE("Toolbar forwards custom item clicks with local coordinates",
          "[gui][toolbar][coverage]") {
    Toolbar horizontal;
    horizontal.add_spacer();
    auto horizontal_custom = std::make_unique<ToolbarProbeView>();
    auto* horizontal_probe = horizontal_custom.get();
    horizontal.add_custom("custom", std::move(horizontal_custom));

    horizontal.on_mouse_down({30, 9});
    REQUIRE(horizontal_probe->mouse_down_count == 1);
    REQUIRE(horizontal_probe->last_position.x == 6.0f);
    REQUIRE(horizontal_probe->last_position.y == 9.0f);

    Toolbar vertical;
    vertical.set_orientation(Toolbar::Orientation::vertical);
    vertical.add_button("first", "First", []() {});
    auto vertical_custom = std::make_unique<ToolbarProbeView>();
    auto* vertical_probe = vertical_custom.get();
    vertical.add_custom("custom", std::move(vertical_custom));

    vertical.on_mouse_down({7, 44});
    REQUIRE(vertical_probe->mouse_down_count == 1);
    REQUIRE(vertical_probe->last_position.x == 7.0f);
    REQUIRE(vertical_probe->last_position.y == 8.0f);
}

TEST_CASE("Toolbar paint emits button, separator, custom, and orientation commands",
          "[gui][toolbar][coverage]") {
    Toolbar toolbar;
    toolbar.set_bounds({0, 0, 180, 40});
    toolbar.add_button("play", "Play", []() {});
    toolbar.add_separator();
    toolbar.add_toggle("loop", "Loop", [](bool) {});
    toolbar.set_toggled("loop", true);
    toolbar.add_button("empty", "", []() {});
    toolbar.set_enabled("empty", false);

    auto custom = std::make_unique<ToolbarProbeView>();
    auto* custom_probe = custom.get();
    toolbar.add_custom("custom", std::move(custom));

    pulp::canvas::RecordingCanvas canvas;
    toolbar.paint(canvas);

    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_rect) >= 2);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_rounded_rect) == 3);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::stroke_line) >= 2);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_text) >= 3);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::save) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::restore) == 1);
    REQUIRE(custom_probe->paint_count == 1);

    Toolbar vertical;
    vertical.set_orientation(Toolbar::Orientation::vertical);
    vertical.set_bounds({0, 0, 40, 140});
    vertical.add_button("one", "One", []() {});
    vertical.add_separator();
    vertical.add_button("two", "Two", []() {});

    pulp::canvas::RecordingCanvas vertical_canvas;
    vertical.paint(vertical_canvas);
    REQUIRE(vertical_canvas.count(pulp::canvas::DrawCommand::Type::fill_rounded_rect) == 2);
    REQUIRE(vertical_canvas.count(pulp::canvas::DrawCommand::Type::stroke_line) >= 2);
}

// ── Extended Buttons ────────────────────────────────────────────────────

TEST_CASE("TextButton disabled state suppresses click and still paints",
          "[gui][buttons][coverage][phase3]") {
    TextButton button("Render");
    button.set_bounds({0, 0, 96, 32});

    int clicks = 0;
    button.on_click = [&] { ++clicks; };
    button.on_mouse_enter();
    button.on_mouse_down({8, 8});
    REQUIRE(clicks == 1);

    button.set_enabled(false);
    button.on_mouse_down({8, 8});
    REQUIRE(clicks == 1);
    REQUIRE_FALSE(button.is_enabled());

    RecordingCanvas canvas;
    button.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 1);
}

TEST_CASE("ArrowButton paints all directions and invokes click callback",
          "[gui][buttons][coverage][phase3]") {
    ArrowButton button;
    button.set_bounds({0, 0, 24, 24});

    int clicks = 0;
    button.on_click = [&] { ++clicks; };

    for (auto direction : {ArrowDirection::up,
                           ArrowDirection::down,
                           ArrowDirection::left,
                           ArrowDirection::right}) {
        button.set_direction(direction);
        REQUIRE(button.direction() == direction);

        RecordingCanvas canvas;
        REQUIRE_NOTHROW(button.paint(canvas));
    }

    button.on_mouse_down({12, 12});
    REQUIRE(clicks == 1);
}

TEST_CASE("ShapeButton reports hover and pressed state to custom painter",
          "[gui][buttons][coverage][phase3]") {
    ShapeButton button;
    button.set_bounds({0, 0, 32, 20});

    std::vector<std::pair<bool, bool>> states;
    int clicks = 0;
    button.set_shape([&](Canvas& canvas, float width, float height, bool hovered, bool pressed) {
        states.push_back({hovered, pressed});
        canvas.set_fill_color(Color::rgba8(10, 20, 30));
        canvas.fill_rect(0, 0, width, height);
    });
    button.on_click = [&] { ++clicks; };

    RecordingCanvas first;
    button.paint(first);
    button.on_mouse_enter();
    RecordingCanvas hovered;
    button.paint(hovered);
    button.on_mouse_down({3, 4});
    RecordingCanvas pressed;
    button.paint(pressed);
    button.on_mouse_leave();
    RecordingCanvas left;
    button.paint(left);

    REQUIRE(clicks == 1);
    REQUIRE(states == std::vector<std::pair<bool, bool>>{
        {false, false}, {true, false}, {true, true}, {false, false}});
    REQUIRE(pressed.count(DrawCommand::Type::fill_rect) == 1);
}

TEST_CASE("ImageButton selects normal hover and pressed image paths",
          "[gui][buttons][coverage][phase3]") {
    ImageButton button;
    button.set_bounds({0, 0, 40, 24});
    button.set_image("normal.png");
    button.set_hover_image("hover.png");
    button.set_pressed_image("pressed.png");

    RecordingCanvas normal;
    button.paint(normal);
    REQUIRE(normal.count(DrawCommand::Type::draw_image) == 1);
    REQUIRE(normal.commands().back().text == "normal.png");

    button.on_mouse_enter();
    RecordingCanvas hover;
    button.paint(hover);
    REQUIRE(hover.commands().back().text == "hover.png");

    int clicks = 0;
    button.on_click = [&] { ++clicks; };
    button.on_mouse_down({2, 2});
    RecordingCanvas pressed;
    button.paint(pressed);
    REQUIRE(clicks == 1);
    REQUIRE(pressed.commands().back().text == "pressed.png");

    button.on_mouse_leave();
    RecordingCanvas after_leave;
    button.paint(after_leave);
    REQUIRE(after_leave.commands().back().text == "normal.png");
}

TEST_CASE("ImageButton falls back to normal image for missing state assets",
          "[gui][buttons][coverage][phase3]") {
    ImageButton button;
    button.set_bounds({0, 0, 40, 24});
    button.set_image("normal.png");

    button.on_mouse_enter();
    RecordingCanvas hover;
    button.paint(hover);
    REQUIRE(hover.commands().back().text == "normal.png");

    button.on_mouse_down({1, 1});
    RecordingCanvas pressed;
    button.paint(pressed);
    REQUIRE(pressed.commands().back().text == "normal.png");
}

TEST_CASE("ResizableCorner reports drag deltas from mouse down origin",
          "[gui][buttons][coverage][phase3]") {
    ResizableCorner corner;
    corner.set_bounds({0, 0, 16, 16});

    std::vector<std::pair<float, float>> deltas;
    corner.on_resize = [&](float dx, float dy) {
        deltas.push_back({dx, dy});
    };

    corner.on_mouse_down({4, 6});
    corner.on_mouse_drag({10, 3});
    corner.on_mouse_drag({1, 9});

    REQUIRE(deltas == std::vector<std::pair<float, float>>{{6.0f, -3.0f}, {-3.0f, 3.0f}});

    RecordingCanvas canvas;
    corner.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 3);
}

// ── ConcertinaPanel ─────────────────────────────────────────────────────

TEST_CASE("ConcertinaPanel add sections", "[gui][concertina]") {
    ConcertinaPanel panel;
    REQUIRE(panel.section_count() == 0);

    panel.add_section("Section A", nullptr, true);
    panel.add_section("Section B", nullptr, false);

    REQUIRE(panel.section_count() == 2);
    REQUIRE(panel.is_expanded(0));
    REQUIRE_FALSE(panel.is_expanded(1));
}

TEST_CASE("ConcertinaPanel toggle", "[gui][concertina]") {
    ConcertinaPanel panel;
    panel.add_section("Test", nullptr, false);

    REQUIRE_FALSE(panel.is_expanded(0));
    panel.toggle(0);
    REQUIRE(panel.is_expanded(0));
    panel.toggle(0);
    REQUIRE_FALSE(panel.is_expanded(0));
}

TEST_CASE("ConcertinaPanel exclusive mode", "[gui][concertina]") {
    ConcertinaPanel panel;
    panel.set_exclusive(true);
    panel.add_section("A", nullptr, true);
    panel.add_section("B", nullptr, false);

    REQUIRE(panel.is_expanded(0));
    REQUIRE_FALSE(panel.is_expanded(1));

    panel.expand(1);
    REQUIRE_FALSE(panel.is_expanded(0));  // Auto-collapsed
    REQUIRE(panel.is_expanded(1));
}

TEST_CASE("ConcertinaPanel guards indices and syncs content visibility",
          "[gui][concertina][issue-493]") {
    ConcertinaPanel panel;
    auto first = std::make_unique<PanelProbeView>(24.0f);
    auto* first_ptr = first.get();
    auto second = std::make_unique<PanelProbeView>(0.0f);
    auto* second_ptr = second.get();

    panel.add_section("First", std::move(first), true);
    panel.add_section("Second", std::move(second), false);

    REQUIRE(first_ptr->visible());
    REQUIRE_FALSE(second_ptr->visible());
    REQUIRE_FALSE(panel.is_expanded(-1));
    REQUIRE_FALSE(panel.is_expanded(99));

    panel.expand(-1);
    panel.collapse(99);
    panel.toggle(99);
    REQUIRE(panel.is_expanded(0));
    REQUIRE_FALSE(panel.is_expanded(1));

    panel.expand(1);
    REQUIRE(second_ptr->visible());
    panel.collapse(0);
    REQUIRE_FALSE(first_ptr->visible());

    panel.layout_sections();
    REQUIRE(second_ptr->bounds().height == 100.0f);
}

TEST_CASE("ConcertinaPanel paint and mouse hit testing cover content offsets",
          "[gui][concertina][issue-493]") {
    ConcertinaPanel panel;
    panel.set_bounds({0, 0, 180, 160});
    panel.set_header_height(20.0f);

    auto first = std::make_unique<PanelProbeView>(40.0f);
    auto* first_ptr = first.get();
    auto second = std::make_unique<PanelProbeView>(30.0f);
    auto* second_ptr = second.get();

    panel.add_section("First", std::move(first), true);
    panel.add_section("Second", std::move(second), false);

    RecordingCanvas canvas;
    panel.paint(canvas);
    REQUIRE(first_ptr->paint_count == 1);
    REQUIRE(second_ptr->paint_count == 0);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) >= 4);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::save) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::restore) == 1);

    panel.on_mouse_down({10, 35});
    REQUIRE(panel.is_expanded(0));
    REQUIRE_FALSE(panel.is_expanded(1));

    panel.on_mouse_down({10, 65});
    REQUIRE(panel.is_expanded(1));

    canvas.clear();
    panel.paint(canvas);
    REQUIRE(second_ptr->paint_count == 1);
    REQUIRE(canvas.count(DrawCommand::Type::save) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::restore) == 2);
}

// ── TextButton ──────────────────────────────────────────────────────────

TEST_CASE("TextButton click callback", "[gui][button]") {
    TextButton btn("Click Me");
    REQUIRE(btn.label() == "Click Me");

    bool clicked = false;
    btn.on_click = [&]() { clicked = true; };

    btn.on_mouse_down({0, 0});
    REQUIRE(clicked);
}

TEST_CASE("TextButton disabled doesn't fire", "[gui][button]") {
    TextButton btn("Disabled");
    btn.set_enabled(false);

    bool clicked = false;
    btn.on_click = [&]() { clicked = true; };

    btn.on_mouse_down({0, 0});
    REQUIRE_FALSE(clicked);
}

TEST_CASE("TextButton paint covers enabled hover and disabled states",
          "[gui][button][coverage]") {
    TextButton btn("Render");
    btn.set_bounds({0, 0, 120, 36});

    RecordingCanvas canvas;
    btn.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 1);

    canvas.clear();
    btn.on_mouse_enter();
    btn.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);

    canvas.clear();
    btn.on_mouse_leave();
    btn.set_enabled(false);
    btn.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 1);
    REQUIRE_FALSE(btn.is_enabled());
}

// pulp #1407 — CSS `text-overflow: ellipsis` on a TextButton truncates the
// painted label so a long string never bleeds past the rounded background.
TEST_CASE("TextButton paint truncates long labels with U+2026 when text-overflow set",
          "[gui][button][issue-1407]") {
    static constexpr const char* kEllipsis = "\xe2\x80\xa6";

    TextButton btn("Mid-band attenuation with high-shelf compensation");
    btn.set_bounds({0, 0, 80, 28});
    btn.set_text_overflow_ellipsis(true);

    RecordingCanvas canvas;
    btn.paint(canvas);

    std::string drawn;
    int fill_text_count = 0;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::fill_text) { drawn = cmd.text; ++fill_text_count; }
    }
    REQUIRE(fill_text_count == 1);
    REQUIRE(drawn.size() >= 3);
    REQUIRE(drawn.compare(drawn.size() - 3, 3, kEllipsis) == 0);
    // Drawn label fits inside the button minus its 8 px horizontal padding.
    REQUIRE(canvas.measure_text(drawn) <= 80.0f - 16.0f);
}

// pulp #1407 (Codex post-merge sweep) — a TextButton narrower than its
// 2 × 8 px horizontal padding budget collapses the available text-box
// to ≤ 0. Without the non-positive guard inside truncate_to_width, the
// helper would short-circuit to the full string and visibly leak the
// label past the button's rounded background. After the guard the
// tiny button paints just "…", which is what the bounding box can hold.
TEST_CASE("TextButton paint with tiny width still truncates instead of leaking the label",
          "[gui][button][issue-1407]") {
    static constexpr const char* kEllipsis = "\xe2\x80\xa6";

    TextButton btn("Mid-band attenuation with high-shelf compensation");
    btn.set_bounds({0, 0, 12, 28});  // 12 < 2 * 8 px padding
    btn.set_text_overflow_ellipsis(true);

    RecordingCanvas canvas;
    btn.paint(canvas);

    std::string drawn;
    int fill_text_count = 0;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::fill_text) { drawn = cmd.text; ++fill_text_count; }
    }
    REQUIRE(fill_text_count == 1);
    REQUIRE(drawn == kEllipsis);
}

TEST_CASE("HyperlinkButton paint underlines only while hovered",
          "[gui][button][coverage]") {
    HyperlinkButton link("Docs", "https://example.invalid/docs");
    link.set_bounds({0, 0, 160, 24});
    REQUIRE(link.text() == "Docs");
    REQUIRE(link.url() == "https://example.invalid/docs");

    link.set_text("Guide");
    link.set_url("https://example.invalid/guide");
    REQUIRE(link.text() == "Guide");
    REQUIRE(link.url() == "https://example.invalid/guide");

    RecordingCanvas canvas;
    link.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 0);

    canvas.clear();
    link.on_mouse_enter();
    link.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 1);

    link.on_mouse_down({4, 4});
    link.on_mouse_leave();
    canvas.clear();
    link.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 0);
}

// ── ArrowButton ─────────────────────────────────────────────────────────

TEST_CASE("ArrowButton direction and click", "[gui][button]") {
    ArrowButton btn(ArrowDirection::down);
    REQUIRE(btn.direction() == ArrowDirection::down);

    bool clicked = false;
    btn.on_click = [&]() { clicked = true; };

    btn.on_mouse_down({0, 0});
    REQUIRE(clicked);
}

TEST_CASE("ArrowButton paint covers all directions", "[gui][button][coverage]") {
    RecordingCanvas canvas;
    ArrowButton btn;
    btn.set_bounds({0, 0, 24, 24});

    for (auto direction : {ArrowDirection::up, ArrowDirection::down,
                           ArrowDirection::left, ArrowDirection::right}) {
        btn.set_direction(direction);
        REQUIRE(btn.direction() == direction);
        canvas.clear();
        btn.paint(canvas);
        REQUIRE(canvas.count(DrawCommand::Type::set_fill_color) == 1);
    }
}

TEST_CASE("ShapeButton reports hover and pressed states to draw callback",
          "[gui][button][coverage]") {
    ShapeButton btn;
    btn.set_bounds({0, 0, 32, 20});

    RecordingCanvas canvas;
    btn.paint(canvas);
    REQUIRE(canvas.command_count() == 0);

    bool saw_hover = false;
    bool saw_pressed = false;
    float last_width = 0.0f;
    float last_height = 0.0f;
    btn.set_shape([&](Canvas& canvas, float width, float height,
                      bool hovered, bool pressed) {
        last_width = width;
        last_height = height;
        saw_hover = saw_hover || hovered;
        saw_pressed = saw_pressed || pressed;
        canvas.fill_rect(0, 0, width, height);
    });

    btn.paint(canvas);
    REQUIRE(last_width == 32.0f);
    REQUIRE(last_height == 20.0f);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1);

    bool clicked = false;
    btn.on_click = [&]() { clicked = true; };
    btn.on_mouse_enter();
    btn.on_mouse_down({2, 3});
    canvas.clear();
    btn.paint(canvas);
    REQUIRE(clicked);
    REQUIRE(saw_hover);
    REQUIRE(saw_pressed);

    // pulp #1171 (Codex P2 on #1069) — verify hover state actually
    // resets after on_mouse_leave(). The previous version pre-set
    // saw_hover=true and only OR'd new values into it, so a regression
    // where leave failed to clear hover would not be caught.
    btn.on_mouse_leave();
    saw_hover = false;
    saw_pressed = false;
    canvas.clear();
    btn.paint(canvas);
    REQUIRE_FALSE(saw_pressed);
    REQUIRE_FALSE(saw_hover);
}

TEST_CASE("ImageButton chooses normal hover and pressed image paths",
          "[gui][button][coverage]") {
    ImageButton btn;
    btn.set_bounds({0, 0, 48, 24});

    RecordingCanvas canvas;
    btn.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::draw_image) == 0);

    btn.set_image("normal.png");
    btn.set_hover_image("hover.png");
    btn.set_pressed_image("pressed.png");

    canvas.clear();
    btn.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::draw_image) == 1);
    REQUIRE(canvas.commands().back().text == "normal.png");

    btn.on_mouse_enter();
    canvas.clear();
    btn.paint(canvas);
    REQUIRE(canvas.commands().back().text == "hover.png");

    bool clicked = false;
    btn.on_click = [&]() { clicked = true; };
    btn.on_mouse_down({1, 1});
    canvas.clear();
    btn.paint(canvas);
    REQUIRE(clicked);
    REQUIRE(canvas.commands().back().text == "pressed.png");

    btn.on_mouse_leave();
    canvas.clear();
    btn.paint(canvas);
    REQUIRE(canvas.commands().back().text == "normal.png");

    ImageButton fallback;
    fallback.set_bounds({0, 0, 48, 24});
    fallback.set_image("fallback.png");

    fallback.on_mouse_enter();
    canvas.clear();
    fallback.paint(canvas);
    REQUIRE(canvas.commands().back().text == "fallback.png");

    fallback.on_mouse_down({1, 1});
    canvas.clear();
    fallback.paint(canvas);
    REQUIRE(canvas.commands().back().text == "fallback.png");
}

TEST_CASE("ResizableCorner paints grip and reports drag deltas",
          "[gui][button][coverage]") {
    ResizableCorner corner;
    corner.set_bounds({0, 0, 16, 16});

    RecordingCanvas canvas;
    corner.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 3);

    float dx = 0.0f;
    float dy = 0.0f;
    corner.on_resize = [&](float x, float y) {
        dx = x;
        dy = y;
    };

    corner.on_mouse_down({4, 5});
    corner.on_mouse_drag({12, 2});
    REQUIRE(dx == 8.0f);
    REQUIRE(dy == -3.0f);
}

// ── LassoComponent ──────────────────────────────────────────────────────

TEST_CASE("LassoComponent selection rect", "[gui][lasso]") {
    LassoComponent lasso;
    REQUIRE_FALSE(lasso.is_active());

    lasso.begin_selection(10.0f, 20.0f);
    REQUIRE(lasso.is_active());

    lasso.update_selection(50.0f, 60.0f);
    auto rect = lasso.selection_rect();
    REQUIRE(rect.x == 10.0f);
    REQUIRE(rect.y == 20.0f);
    REQUIRE(rect.width == 40.0f);
    REQUIRE(rect.height == 40.0f);

    lasso.end_selection();
    REQUIRE_FALSE(lasso.is_active());
}

TEST_CASE("LassoComponent handles reverse drag", "[gui][lasso]") {
    LassoComponent lasso;
    lasso.begin_selection(50.0f, 50.0f);
    lasso.update_selection(10.0f, 10.0f);

    auto rect = lasso.selection_rect();
    REQUIRE(rect.x == 10.0f);
    REQUIRE(rect.y == 10.0f);
    REQUIRE(rect.width == 40.0f);
    REQUIRE(rect.height == 40.0f);
}

TEST_CASE("LassoComponent callback fires", "[gui][lasso]") {
    LassoComponent lasso;
    SelectionRect last_rect;

    lasso.on_selection_changed = [&](const SelectionRect& r) { last_rect = r; };

    lasso.begin_selection(0, 0);
    lasso.update_selection(100, 100);

    REQUIRE(last_rect.width == 100.0f);
    REQUIRE(last_rect.height == 100.0f);
}

TEST_CASE("SelectionRect contains and intersects use half-open bounds",
          "[gui][lasso][coverage][phase3]") {
    SelectionRect rect{10.0f, 20.0f, 40.0f, 30.0f};

    REQUIRE(rect.contains(10.0f, 20.0f));
    REQUIRE(rect.contains(49.9f, 49.9f));
    REQUIRE_FALSE(rect.contains(50.0f, 20.0f));
    REQUIRE_FALSE(rect.contains(10.0f, 50.0f));
    REQUIRE_FALSE(rect.contains(9.9f, 25.0f));

    REQUIRE(rect.intersects(0.0f, 0.0f, 11.0f, 21.0f));
    REQUIRE(rect.intersects(49.0f, 49.0f, 5.0f, 5.0f));
    REQUIRE_FALSE(rect.intersects(50.0f, 20.0f, 10.0f, 10.0f));
    REQUIRE_FALSE(rect.intersects(0.0f, 50.0f, 100.0f, 10.0f));
}

TEST_CASE("LassoComponent inactive updates and end are no-ops",
          "[gui][lasso][coverage][phase3]") {
    LassoComponent lasso;
    int changed = 0;
    int completed = 0;
    lasso.on_selection_changed = [&](const SelectionRect&) { ++changed; };
    lasso.on_selection_complete = [&](const SelectionRect&) { ++completed; };

    lasso.update_selection(20.0f, 30.0f);
    lasso.end_selection();

    REQUIRE_FALSE(lasso.is_active());
    REQUIRE(changed == 0);
    REQUIRE(completed == 0);
    REQUIRE(lasso.selection_rect().width == 0.0f);
    REQUIRE(lasso.selection_rect().height == 0.0f);
}

TEST_CASE("LassoComponent completes selection and only paints while active",
          "[gui][lasso][coverage][phase3]") {
    LassoComponent lasso;
    SelectionRect completed;
    int complete_calls = 0;
    lasso.on_selection_complete = [&](const SelectionRect& r) {
        completed = r;
        ++complete_calls;
    };

    RecordingCanvas inactive;
    lasso.paint(inactive);
    REQUIRE(inactive.commands().empty());

    lasso.on_mouse_down({40.0f, 30.0f});
    lasso.on_mouse_drag({10.0f, 70.0f});

    RecordingCanvas active;
    lasso.paint(active);
    REQUIRE(active.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(active.count(DrawCommand::Type::stroke_rect) == 1);

    lasso.on_mouse_up({10.0f, 70.0f});
    REQUIRE_FALSE(lasso.is_active());
    REQUIRE(complete_calls == 1);
    REQUIRE(completed.x == 10.0f);
    REQUIRE(completed.y == 30.0f);
    REQUIRE(completed.width == 30.0f);
    REQUIRE(completed.height == 40.0f);

    lasso.end_selection();
    REQUIRE(complete_calls == 1);
}
