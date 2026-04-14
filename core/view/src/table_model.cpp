#include <pulp/view/table_model.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::view {

namespace {

// Compare two cells for sort. Numeric keys win when BOTH have them;
// otherwise compare by text.
int compare_cells(const TableCell& a, const TableCell& b) {
    const bool an = !std::isnan(a.sort_key_num);
    const bool bn = !std::isnan(b.sort_key_num);
    if (an && bn) {
        if (a.sort_key_num < b.sort_key_num) return -1;
        if (a.sort_key_num > b.sort_key_num) return 1;
        return 0;
    }
    if (a.text < b.text) return -1;
    if (a.text > b.text) return 1;
    return 0;
}

} // namespace

void TableModel::toggle_sort(std::size_t col) {
    if (col >= columns_.size() || !columns_[col].sortable) return;
    if (sort_column_ != static_cast<int>(col)) {
        sort_by(col, TableSortOrder::Ascending);
        return;
    }
    switch (sort_order_) {
        case TableSortOrder::None:       sort_by(col, TableSortOrder::Ascending);  break;
        case TableSortOrder::Ascending:  sort_by(col, TableSortOrder::Descending); break;
        case TableSortOrder::Descending: clear_sort(); break;
    }
}

void TableModel::sort_by(std::size_t col, TableSortOrder order) {
    if (col >= columns_.size()) return;
    sort_column_ = static_cast<int>(col);
    sort_order_ = order;
    apply_sort_();
}

void TableModel::apply_sort_() {
    if (original_order_.empty() || original_order_.size() != rows_.size()) {
        original_order_.resize(rows_.size());
        for (std::size_t i = 0; i < rows_.size(); ++i) original_order_[i] = i;
    }
    if (sort_order_ == TableSortOrder::None || sort_column_ < 0) return;

    // Build index permutation and sort it, then reorder rows + the
    // parallel original_order_.
    std::vector<std::size_t> perm(rows_.size());
    for (std::size_t i = 0; i < perm.size(); ++i) perm[i] = i;

    const auto col = static_cast<std::size_t>(sort_column_);
    const bool ascending = (sort_order_ == TableSortOrder::Ascending);

    std::stable_sort(perm.begin(), perm.end(),
        [&](std::size_t a, std::size_t b) {
            int cmp = compare_cells(rows_[a][col], rows_[b][col]);
            return ascending ? cmp < 0 : cmp > 0;
        });

    std::vector<std::vector<TableCell>> rows_out(rows_.size());
    std::vector<std::size_t> orig_out(original_order_.size());
    for (std::size_t i = 0; i < perm.size(); ++i) {
        rows_out[i] = std::move(rows_[perm[i]]);
        orig_out[i] = original_order_[perm[i]];
    }
    rows_ = std::move(rows_out);
    original_order_ = std::move(orig_out);
}

void TableModel::restore_original_order_() {
    if (original_order_.size() != rows_.size()) return;

    // Build the inverse permutation: for each current row position, the
    // spot it originally occupied. Applying it returns rows_ to its
    // insertion order.
    std::vector<std::vector<TableCell>> rows_out(rows_.size());
    std::vector<std::size_t> identity(rows_.size());
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        rows_out[original_order_[i]] = std::move(rows_[i]);
        identity[i] = i;
    }
    rows_ = std::move(rows_out);
    original_order_ = std::move(identity);
}

}  // namespace pulp::view
