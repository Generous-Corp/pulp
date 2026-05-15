#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/table.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace pulp::view;
using namespace pulp::canvas;

namespace {

class RecordingTableModel : public TableModel {
public:
    explicit RecordingTableModel(std::vector<std::vector<std::string>> rows)
        : rows_(std::move(rows)) {}

    int row_count() const override { return static_cast<int>(rows_.size()); }

    std::string cell_text(int row, int column) const override {
        if (row < 0 || row >= row_count()) return {};
        const auto& r = rows_[static_cast<size_t>(row)];
        if (column < 0 || column >= static_cast<int>(r.size())) return {};
        return r[static_cast<size_t>(column)];
    }

    bool sort(int column, bool ascending) override {
        sort_calls.push_back({column, ascending});
        return true;
    }

    void row_selected(int row) override {
        selected_rows.push_back(row);
    }

    std::vector<std::pair<int, bool>> sort_calls;
    std::vector<int> selected_rows;

private:
    std::vector<std::vector<std::string>> rows_;
};

bool has_text(const RecordingCanvas& canvas, const std::string& text) {
    return std::any_of(canvas.commands().begin(),
                       canvas.commands().end(),
                       [&](const DrawCommand& cmd) {
                           return cmd.type == DrawCommand::Type::fill_text &&
                                  cmd.text == text;
                       });
}

const DrawCommand* find_text(const RecordingCanvas& canvas, const std::string& text) {
    auto it = std::find_if(canvas.commands().begin(),
                           canvas.commands().end(),
                           [&](const DrawCommand& cmd) {
                               return cmd.type == DrawCommand::Type::fill_text &&
                                      cmd.text == text;
                           });
    return it == canvas.commands().end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("TableListBox paint returns early without model or columns", "[gui][table][coverage]") {
    TableListBox table;
    table.set_bounds({0, 0, 200, 100});

    RecordingCanvas canvas;
    table.paint(canvas);
    REQUIRE(canvas.command_count() == 0);

    RecordingTableModel model({{"A"}});
    table.set_model(&model);
    table.paint(canvas);
    REQUIRE(canvas.command_count() == 0);
}

TEST_CASE("TableListBox paints scaled aligned headers rows and sort indicator",
          "[gui][table][coverage]") {
    RecordingTableModel model({
        {"Alpha", "Synth", "10"},
        {"Beta", "Effect", "2"},
    });

    TableListBox table;
    table.set_bounds({0, 0, 120, 90});
    table.set_header_height(20.0f);
    table.set_row_height(18.0f);
    table.set_model(&model);
    table.add_column({"Name", 100.0f, true, true, TableColumn::Align::left});
    table.add_column({"Type", 100.0f, true, true, TableColumn::Align::center});
    table.add_column({"Value", 100.0f, true, true, TableColumn::Align::right});

    table.on_mouse_down({45.0f, 5.0f});
    REQUIRE(model.sort_calls == std::vector<std::pair<int, bool>>{{1, true}});

    RecordingCanvas canvas;
    table.paint(canvas);

    REQUIRE(has_text(canvas, "Name"));
    REQUIRE(has_text(canvas, "Type"));
    REQUIRE(has_text(canvas, "Value"));
    REQUIRE(has_text(canvas, "Alpha"));
    REQUIRE(has_text(canvas, "Effect"));
    REQUIRE(has_text(canvas, "10"));
    REQUIRE(has_text(canvas, "\xe2\x96\xb2"));
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) >= 4);

    auto* left = find_text(canvas, "Name");
    auto* center = find_text(canvas, "Type");
    auto* right = find_text(canvas, "Value");
    REQUIRE(left != nullptr);
    REQUIRE(center != nullptr);
    REQUIRE(right != nullptr);
    REQUIRE(left->f[0] == 6.0f);
    REQUIRE(center->f[0] == 46.0f);
    REQUIRE(right->f[0] == 79.0f);

    table.on_mouse_down({45.0f, 5.0f});
    REQUIRE(model.sort_calls == std::vector<std::pair<int, bool>>{{1, true}, {1, false}});

    RecordingCanvas descending_canvas;
    table.paint(descending_canvas);
    REQUIRE(has_text(descending_canvas, "\xe2\x96\xbc"));
}

TEST_CASE("TableListBox ignores unsortable header clicks and selects visible rows",
          "[gui][table][coverage]") {
    RecordingTableModel model({
        {"Row 0", "A"},
        {"Row 1", "B"},
        {"Row 2", "C"},
    });

    TableListBox table;
    table.set_bounds({0, 0, 180, 100});
    table.set_header_height(20.0f);
    table.set_row_height(10.0f);
    table.set_model(&model);
    table.add_column({"Name", 90.0f, false});
    table.add_column({"Value", 90.0f, true});

    table.on_mouse_down({30.0f, 5.0f});
    REQUIRE(model.sort_calls.empty());
    REQUIRE(table.selected_row() == -1);

    std::vector<int> changed_rows;
    table.on_selection_changed = [&](int row) { changed_rows.push_back(row); };

    table.on_mouse_down({10.0f, 35.0f});
    REQUIRE(table.selected_row() == 1);
    REQUIRE(changed_rows == std::vector<int>{1});
    REQUIRE(model.selected_rows == std::vector<int>{1});

    table.on_mouse_down({10.0f, 500.0f});
    REQUIRE(table.selected_row() == 1);
    REQUIRE(changed_rows == std::vector<int>{1});
    REQUIRE(model.selected_rows == std::vector<int>{1});
}

TEST_CASE("SimpleTableModel handles negative sort columns and sparse rows",
          "[gui][table][coverage]") {
    SimpleTableModel model;
    model.add_row({"B"});
    model.add_row({"A", "two"});

    REQUIRE_FALSE(model.sort(-1, true));
    REQUIRE(model.cell_text(0, 0) == "B");

    REQUIRE(model.sort(1, true));
    REQUIRE(model.cell_text(0, 0) == "B");
    REQUIRE(model.cell_text(0, 1).empty());
    REQUIRE(model.cell_text(1, 1) == "two");
}

TEST_CASE("TableListBox clear columns and model-less clicks are stable",
          "[gui][table][coverage][issue-653]") {
    TableListBox table;
    table.set_bounds({0, 0, 160, 80});
    table.set_header_height(20.0f);
    table.add_column({"Name", 80.0f});
    table.add_column({"Value", 80.0f});
    REQUIRE(table.column_count() == 2);

    table.on_mouse_down({10.0f, 5.0f});
    table.on_mouse_down({10.0f, 35.0f});
    REQUIRE(table.selected_row() == -1);

    table.clear_columns();
    REQUIRE(table.column_count() == 0);

    RecordingCanvas canvas;
    table.paint(canvas);
    REQUIRE(canvas.command_count() == 0);
}

TEST_CASE("TableListBox paints selected row and ignores negative body clicks",
          "[gui][table][coverage][issue-653]") {
    RecordingTableModel model({
        {"First", "A"},
        {"Second", "B"},
    });

    TableListBox table;
    table.set_bounds({0, 0, 180, 80});
    table.set_header_height(20.0f);
    table.set_row_height(20.0f);
    table.set_model(&model);
    table.add_column({"Name", 90.0f});
    table.add_column({"Value", 90.0f});
    table.set_selected_row(1);

    table.on_mouse_down({10.0f, -5.0f});
    REQUIRE(model.sort_calls.empty());
    REQUIRE(table.selected_row() == 1);

    RecordingCanvas canvas;
    table.paint(canvas);
    REQUIRE(has_text(canvas, "First"));
    REQUIRE(has_text(canvas, "Second"));
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) >= 3);
}
