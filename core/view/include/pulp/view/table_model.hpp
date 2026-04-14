#pragma once

// TableModel — data + sort layer for the eventual sortable TableView
// widget (workstream 07 slice 7.3). The audit spec calls out "sortable
// columns, column reordering, virtualized rows". This slice delivers the
// data-model core: rows, columns, per-column sort order, click-to-sort
// logic. The widget shell (header row, drag-to-reorder, virtualization)
// lives in a follow-up slice.
//
// The model is string-first: each cell carries a display string plus an
// optional sort key. A numeric sort key lets a column display "3 dB" but
// sort as 3.0. For bool / category columns callers can fill a single
// sort_key_*; everything stays opt-in.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace pulp::view {

struct TableCell {
    std::string text;
    /// Optional numeric sort key. NaN means "no numeric key — sort by
    /// text instead". Lets "3 dB" display as text but sort numerically.
    double sort_key_num = std::numeric_limits<double>::quiet_NaN();
};

struct TableColumn {
    std::string title;
    bool sortable = true;
    float default_width = 100.0f;
};

enum class TableSortOrder : uint8_t {
    None       = 0,
    Ascending  = 1,
    Descending = 2,
};

class TableModel {
public:
    void add_column(TableColumn column) {
        columns_.push_back(std::move(column));
        for (auto& row : rows_) row.resize(columns_.size());
    }

    /// Add a row. Pads with empty cells if short.
    ///
    /// If a previous sort was active, sort state is dropped but the
    /// insertion-order mapping is restored first so a later clear_sort()
    /// can still return rows to their original order. This matches the
    /// clear_sort contract documented above; without the restore step a
    /// dynamic table that receives new rows after sorting would get
    /// stuck at the post-sort layout. Fix per #200 review.
    void add_row(std::vector<TableCell> cells) {
        cells.resize(columns_.size());
        if (sort_order_ != TableSortOrder::None && !original_order_.empty()
                                                && original_order_.size() == rows_.size()) {
            restore_original_order_();
        }
        rows_.push_back(std::move(cells));
        if (!original_order_.empty()) {
            original_order_.push_back(rows_.size() - 1);
        }
        sort_column_ = -1;
        sort_order_ = TableSortOrder::None;
    }

    std::size_t column_count() const { return columns_.size(); }
    std::size_t row_count() const { return rows_.size(); }
    const TableColumn& column(std::size_t i) const { return columns_[i]; }
    const TableCell& cell(std::size_t row, std::size_t col) const {
        return rows_[row][col];
    }

    /// Sort rows by column `col`. Passing the same column cycles
    /// None → Ascending → Descending → None, matching how spreadsheet
    /// UIs toggle via header clicks.
    void toggle_sort(std::size_t col);

    /// Explicit sort (used by tests and by code that wants a specific order).
    void sort_by(std::size_t col, TableSortOrder order);

    /// Clear any sort; original insertion order is restored.
    void clear_sort() {
        sort_column_ = -1;
        sort_order_ = TableSortOrder::None;
        // Rows are kept; `apply_row_permutation_` restores on next sort.
        restore_original_order_();
    }

    int sort_column() const { return sort_column_; }
    TableSortOrder sort_order() const { return sort_order_; }

private:
    void apply_sort_();
    void restore_original_order_();

    std::vector<TableColumn> columns_;
    std::vector<std::vector<TableCell>> rows_;
    // Permutation of original row indices, parallel to `rows_`.
    // Lets clear_sort() restore insertion order without storing two
    // copies of the data.
    std::vector<std::size_t> original_order_;
    int sort_column_ = -1;
    TableSortOrder sort_order_ = TableSortOrder::None;
};

}  // namespace pulp::view
