// TableModel tests (workstream 07 slice 7.3 — data/sort layer).

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/table_model.hpp>

using namespace pulp::view;

namespace {

TableModel make_preset_table() {
    TableModel m;
    m.add_column({"Name"});
    m.add_column({"Author"});
    m.add_column({"Rating"});
    m.add_row({{"Dreamy Pad"}, {"Alice"}, {"4.5", 4.5}});
    m.add_row({{"Stab"},       {"Bob"},   {"3.0", 3.0}});
    m.add_row({{"Lush"},       {"Alice"}, {"5.0", 5.0}});
    return m;
}

} // namespace

TEST_CASE("empty model has zero rows + columns", "[ui][table-model]") {
    TableModel m;
    REQUIRE(m.row_count() == 0);
    REQUIRE(m.column_count() == 0);
    REQUIRE(m.sort_column() == -1);
    REQUIRE(m.sort_order() == TableSortOrder::None);
}

TEST_CASE("sort_by orders rows by text column", "[ui][table-model]") {
    auto m = make_preset_table();
    m.sort_by(0, TableSortOrder::Ascending);
    REQUIRE(m.cell(0, 0).text == "Dreamy Pad");
    REQUIRE(m.cell(1, 0).text == "Lush");
    REQUIRE(m.cell(2, 0).text == "Stab");

    m.sort_by(0, TableSortOrder::Descending);
    REQUIRE(m.cell(0, 0).text == "Stab");
    REQUIRE(m.cell(2, 0).text == "Dreamy Pad");
}

TEST_CASE("sort_by uses numeric key when provided", "[ui][table-model]") {
    auto m = make_preset_table();
    m.sort_by(2, TableSortOrder::Ascending);
    // Ratings: 3.0, 4.5, 5.0 — numeric sort, not the lexical "3.0 < 4.5 < 5.0"
    // (which happens to agree; add a row that tests the numeric path).
    m.add_row({{"Big"}, {"Carl"}, {"10", 10.0}});
    m.sort_by(2, TableSortOrder::Ascending);
    // Numerically 3 < 4.5 < 5 < 10 — but textually "10" < "3.0" because
    // '1' < '3'. We stored numeric keys, so the order must be numeric.
    REQUIRE(m.cell(0, 2).text == "3.0");
    REQUIRE(m.cell(1, 2).text == "4.5");
    REQUIRE(m.cell(2, 2).text == "5.0");
    REQUIRE(m.cell(3, 2).text == "10");
}

TEST_CASE("toggle_sort cycles ascending -> descending -> none",
          "[ui][table-model]") {
    auto m = make_preset_table();
    m.toggle_sort(0);  REQUIRE(m.sort_order() == TableSortOrder::Ascending);
    m.toggle_sort(0);  REQUIRE(m.sort_order() == TableSortOrder::Descending);
    m.toggle_sort(0);  REQUIRE(m.sort_order() == TableSortOrder::None);
    // And back to the insertion order.
    REQUIRE(m.cell(0, 0).text == "Dreamy Pad");
    REQUIRE(m.cell(1, 0).text == "Stab");
    REQUIRE(m.cell(2, 0).text == "Lush");
}

TEST_CASE("switching sort column resets to ascending",
          "[ui][table-model]") {
    auto m = make_preset_table();
    m.toggle_sort(0);                              // ascending by name
    m.toggle_sort(1);                              // switch to author
    REQUIRE(m.sort_column() == 1);
    REQUIRE(m.sort_order() == TableSortOrder::Ascending);
}

TEST_CASE("non-sortable columns are ignored by toggle_sort",
          "[ui][table-model]") {
    TableModel m;
    TableColumn c{"Lock"};
    c.sortable = false;
    m.add_column(c);
    m.add_row({{"B"}});
    m.add_row({{"A"}});
    m.toggle_sort(0);
    REQUIRE(m.sort_order() == TableSortOrder::None);
    REQUIRE(m.cell(0, 0).text == "B");  // unchanged
}

TEST_CASE("clear_sort restores original insertion order",
          "[ui][table-model]") {
    auto m = make_preset_table();
    m.sort_by(0, TableSortOrder::Ascending);
    m.clear_sort();
    REQUIRE(m.cell(0, 0).text == "Dreamy Pad");
    REQUIRE(m.cell(1, 0).text == "Stab");
    REQUIRE(m.cell(2, 0).text == "Lush");
    REQUIRE(m.sort_order() == TableSortOrder::None);
}

TEST_CASE("adding rows after sort then clear_sort restores insertion order",
          "[ui][table-model]") {
    // Regression: original_order_ bookkeeping must survive add_row
    // after a sort so clear_sort() returns to true insertion order.
    // Without the fix, new rows ended up on top of a stale identity
    // permutation and clear_sort() either no-op'd or stayed on the
    // post-sort layout.
    auto m = make_preset_table();
    m.sort_by(0, TableSortOrder::Ascending);
    m.add_row({{"Kick"},   {"Carl"},  {"4.0", 4.0}});
    m.add_row({{"Atlas"},  {"Dora"},  {"4.2", 4.2}});
    m.sort_by(0, TableSortOrder::Descending);
    m.clear_sort();
    REQUIRE(m.cell(0, 0).text == "Dreamy Pad");
    REQUIRE(m.cell(1, 0).text == "Stab");
    REQUIRE(m.cell(2, 0).text == "Lush");
    REQUIRE(m.cell(3, 0).text == "Kick");
    REQUIRE(m.cell(4, 0).text == "Atlas");
}
