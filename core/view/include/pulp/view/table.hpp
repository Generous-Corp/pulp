#pragma once

// TableListBox — sortable, scrollable table with columns and cell renderers.
// Each column has a header, width, and renderer. Rows are provided by a data model.

#include <pulp/view/view.hpp>
#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <algorithm>

namespace pulp::view {

/// Column definition for TableListBox
struct TableColumn {
    std::string header;          // Column header text
    float width = 100.0f;       // Column width in pixels
    bool sortable = true;       // Whether the column can be sorted
    bool resizable = true;      // Whether the user can resize the column

    enum class Align { left, center, right };
    Align align = Align::left;
};

/// Data model for TableListBox — implement this to provide row data
class TableModel {
public:
    virtual ~TableModel() = default;

    /// Number of rows in the table
    virtual int row_count() const = 0;

    /// Cell text for a given row and column index
    virtual std::string cell_text(int row, int column) const = 0;

    /// Optional: sort the data by column. Return true if handled.
    virtual bool sort(int column, bool ascending) { (void)column; (void)ascending; return false; }

    /// Optional: called when a row is selected
    virtual void row_selected(int row) { (void)row; }

    /// Optional: called when a row is double-clicked
    virtual void row_double_clicked(int row) { (void)row; }
};

/// Simple in-memory table model backed by a vector of string vectors
class SimpleTableModel : public TableModel {
public:
    void set_data(std::vector<std::vector<std::string>> rows) {
        rows_ = std::move(rows);
    }

    void add_row(std::vector<std::string> row) {
        rows_.push_back(std::move(row));
    }

    int row_count() const override { return static_cast<int>(rows_.size()); }

    std::string cell_text(int row, int column) const override {
        if (row < 0 || row >= row_count()) return "";
        auto& r = rows_[static_cast<size_t>(row)];
        if (column < 0 || column >= static_cast<int>(r.size())) return "";
        return r[static_cast<size_t>(column)];
    }

    bool sort(int column, bool ascending) override {
        if (column < 0) return false;
        auto col = static_cast<size_t>(column);
        std::sort(rows_.begin(), rows_.end(),
            [col, ascending](const auto& a, const auto& b) {
                std::string va = col < a.size() ? a[col] : "";
                std::string vb = col < b.size() ? b[col] : "";
                return ascending ? va < vb : va > vb;
            });
        return true;
    }

private:
    std::vector<std::vector<std::string>> rows_;
};

/// Scrollable table with sortable columns
class TableListBox : public View {
public:
    TableListBox() { set_focusable(true); }

    /// Set the data model (not owned — caller must keep alive)
    void set_model(TableModel* model) { model_ = model; }

    /// Add a column definition
    void add_column(TableColumn column) { columns_.push_back(std::move(column)); }

    /// Clear all columns
    void clear_columns() { columns_.clear(); }

    /// Get/set selected row (-1 = none)
    void set_selected_row(int row) { selected_row_ = row; }
    int selected_row() const { return selected_row_; }

    /// Row height
    void set_row_height(float h) { row_height_ = h; }
    float row_height() const { return row_height_; }

    /// Header height
    void set_header_height(float h) { header_height_ = h; }

    /// Number of columns
    int column_count() const { return static_cast<int>(columns_.size()); }

    /// Called when selection changes
    std::function<void(int row)> on_selection_changed;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;

private:
    TableModel* model_ = nullptr;
    std::vector<TableColumn> columns_;
    int selected_row_ = -1;
    int sort_column_ = -1;
    bool sort_ascending_ = true;
    float row_height_ = 24.0f;
    float header_height_ = 28.0f;
    float scroll_offset_ = 0.0f;
};

}  // namespace pulp::view
