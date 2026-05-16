// Cross-platform a11y test harness (workstream 04 slice 4.4).
// Verifies snapshot_accessibility_tree walks the View tree and captures
// role/label/value metadata without needing a platform screen reader.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/accessibility.hpp>
#include <pulp/view/accessibility_tree.hpp>
#include <pulp/view/view.hpp>

#include <string>
#include <utility>

using namespace pulp::view;

namespace {

class Probe : public View {};

class RangeProbe : public View, public AccessibilityValueInterface {
public:
    explicit RangeProbe(double current) : current_(current) {}

    double get_current_value() const override { return current_; }
    void set_current_value(double value) override { current_ = value; }
    double get_minimum_value() const override { return -12.0; }
    double get_maximum_value() const override { return 12.0; }

private:
    double current_ = 0.0;
};

class DegenerateRangeProbe : public View, public AccessibilityValueInterface {
public:
    explicit DegenerateRangeProbe(double current) : current_(current) {}

    double get_current_value() const override { return current_; }
    void set_current_value(double value) override { current_ = value; }
    double get_minimum_value() const override { return 5.0; }
    double get_maximum_value() const override { return 5.0; }

private:
    double current_ = 0.0;
};

class TextProbe : public AccessibilityTextInterface {
public:
    explicit TextProbe(std::string text) : text_(std::move(text)) {}

    std::string get_text() const override { return text_; }
    void set_text(std::string_view text) override { text_ = std::string(text); }

private:
    std::string text_;
};

class TableProbe : public AccessibilityTableInterface {
public:
    int get_row_count() const override { return 3; }
    int get_column_count() const override { return 2; }
    std::string get_column_header(int column) const override {
        return column == 0 ? "Name" : "Value";
    }
};

class CellProbe : public AccessibilityCellInterface {
public:
    std::string get_cell_text(int row, int column) const override {
        return std::to_string(row) + ":" + std::to_string(column);
    }
};

} // namespace

TEST_CASE("snapshot captures role + label + value", "[a11y][harness]") {
    Probe root;
    root.set_access_role(View::AccessRole::group);
    root.set_access_label("Root");

    auto knob = std::make_unique<Probe>();
    knob->set_access_role(View::AccessRole::slider);
    knob->set_access_label("Gain");
    knob->set_access_value("0 dB");
    root.add_child(std::move(knob));

    auto label = std::make_unique<Probe>();
    label->set_access_role(View::AccessRole::label);
    label->set_access_label("Title");
    root.add_child(std::move(label));

    // #192 review: root excluded to match platform bridge output;
    // only descendants reported, child depth resets to 0.
    auto nodes = snapshot_accessibility_tree(root);
    REQUIRE(nodes.size() == 2);
    REQUIRE(nodes[0].role == View::AccessRole::slider);
    REQUIRE(nodes[0].label == "Gain");
    REQUIRE(nodes[0].value == "0 dB");
    REQUIRE(nodes[0].depth == 0);
    REQUIRE(nodes[1].role == View::AccessRole::label);
    REQUIRE(nodes[1].depth == 0);
}

TEST_CASE("snapshot excludes root even when it is announceable",
          "[a11y][harness]") {
    Probe root;
    root.set_access_role(View::AccessRole::group);
    root.set_access_label("Root");

    REQUIRE(snapshot_accessibility_tree(root).empty());
    REQUIRE(count_announceable(root) == 0);
    REQUIRE(find_by_role_and_label(root, View::AccessRole::group, "Root")
            == nullptr);
}

TEST_CASE("count_announceable excludes AccessRole::none", "[a11y][harness]") {
    Probe root;
    // root has role::none by default
    auto a = std::make_unique<Probe>();
    a->set_access_role(View::AccessRole::slider);
    root.add_child(std::move(a));
    auto b = std::make_unique<Probe>();
    // b stays none — invisible to screen readers
    root.add_child(std::move(b));
    auto c = std::make_unique<Probe>();
    c->set_access_role(View::AccessRole::meter);
    root.add_child(std::move(c));
    REQUIRE(count_announceable(root) == 2);
}

TEST_CASE("snapshot walks through non-announceable containers",
          "[a11y][harness]") {
    Probe root;

    auto hidden_container = std::make_unique<Probe>();
    hidden_container->set_access_label("Layout Only");

    auto meter = std::make_unique<Probe>();
    meter->set_access_role(View::AccessRole::meter);
    meter->set_access_label("Output");
    hidden_container->add_child(std::move(meter));

    root.add_child(std::move(hidden_container));

    auto nodes = snapshot_accessibility_tree(root);
    REQUIRE(nodes.size() == 2);
    REQUIRE(nodes[0].role == View::AccessRole::none);
    REQUIRE(nodes[0].label == "Layout Only");
    REQUIRE(nodes[0].depth == 0);
    REQUIRE(nodes[1].role == View::AccessRole::meter);
    REQUIRE(nodes[1].label == "Output");
    REQUIRE(nodes[1].depth == 1);
    REQUIRE(count_announceable(root) == 1);
}

TEST_CASE("find_by_role_and_label returns nullptr when absent",
          "[a11y][harness]") {
    Probe root;
    root.set_access_role(View::AccessRole::group);
    root.set_access_label("Root");
    auto knob = std::make_unique<Probe>();
    knob->set_access_role(View::AccessRole::slider);
    knob->set_access_label("Gain");
    root.add_child(std::move(knob));

    REQUIRE(find_by_role_and_label(root, View::AccessRole::slider, "Gain")
            != nullptr);
    REQUIRE(find_by_role_and_label(root, View::AccessRole::slider, "Absent")
            == nullptr);
    REQUIRE(find_by_role_and_label(root, View::AccessRole::meter, "Gain")
            == nullptr);
}

TEST_CASE("snapshot captures nested range metadata", "[a11y][harness]") {
    Probe root;

    auto group = std::make_unique<Probe>();
    group->set_access_role(View::AccessRole::group);
    group->set_access_label("Channel Strip");

    auto gain = std::make_unique<RangeProbe>(6.0);
    gain->set_access_role(View::AccessRole::slider);
    gain->set_access_label("Gain");
    group->add_child(std::move(gain));

    root.add_child(std::move(group));

    auto nodes = snapshot_accessibility_tree(root);
    REQUIRE(nodes.size() == 2);

    REQUIRE(nodes[0].role == View::AccessRole::group);
    REQUIRE(nodes[0].label == "Channel Strip");
    REQUIRE(nodes[0].depth == 0);
    REQUIRE_FALSE(nodes[0].has_value);

    REQUIRE(nodes[1].role == View::AccessRole::slider);
    REQUIRE(nodes[1].label == "Gain");
    REQUIRE(nodes[1].depth == 1);
    REQUIRE(nodes[1].has_value);
    REQUIRE(nodes[1].min_value == -12.0);
    REQUIRE(nodes[1].max_value == 12.0);
    REQUIRE(nodes[1].current_value == 6.0);
    REQUIRE(nodes[1].value_string == "75%");
}

// pulp #1737 — ARIA state attributes (aria-pressed, aria-checked,
// aria-disabled, aria-hidden) must surface through the cross-platform
// AccessibilityNodeSnapshot so AT bridges (NSAccessibility today;
// AT-SPI / UIA when wired per #217) and offline tests can read them.
// Pre-fix the View slots existed but the snapshot only carried role/
// label/value — assistive tech never saw the state.
TEST_CASE("Accessibility snapshot surfaces ARIA state attributes",
          "[a11y][issue-1737]") {
    using namespace pulp::view;
    Probe root;

    auto btn = std::make_unique<Probe>();
    btn->set_access_role(View::AccessRole::toggle);
    btn->set_access_label("Mute");
    btn->set_access_pressed("true");

    auto chk = std::make_unique<Probe>();
    chk->set_access_role(View::AccessRole::toggle);
    chk->set_access_label("Tri-state filter");
    chk->set_access_checked("mixed");
    chk->set_access_disabled("false");

    auto hidden = std::make_unique<Probe>();
    hidden->set_access_role(View::AccessRole::label);
    hidden->set_access_label("Decorative icon");
    hidden->set_access_hidden("true");

    root.add_child(std::move(btn));
    root.add_child(std::move(chk));
    root.add_child(std::move(hidden));

    auto nodes = snapshot_accessibility_tree(root);
    REQUIRE(nodes.size() == 3);

    REQUIRE(nodes[0].label == "Mute");
    REQUIRE(nodes[0].pressed  == "true");
    REQUIRE(nodes[0].checked.empty());
    REQUIRE(nodes[0].disabled.empty());
    REQUIRE(nodes[0].hidden.empty());

    REQUIRE(nodes[1].label == "Tri-state filter");
    REQUIRE(nodes[1].checked  == "mixed");
    REQUIRE(nodes[1].disabled == "false");
    REQUIRE(nodes[1].pressed.empty());

    REQUIRE(nodes[2].label == "Decorative icon");
    REQUIRE(nodes[2].hidden == "true");
}

TEST_CASE("Accessibility value interface default strings clamp and fall back",
          "[a11y][coverage][issue-654]") {
    Probe root;

    auto high = std::make_unique<RangeProbe>(30.0);
    high->set_access_role(View::AccessRole::slider);
    high->set_access_label("High");
    root.add_child(std::move(high));

    auto low = std::make_unique<RangeProbe>(-30.0);
    low->set_access_role(View::AccessRole::slider);
    low->set_access_label("Low");
    root.add_child(std::move(low));

    auto flat = std::make_unique<DegenerateRangeProbe>(7.25);
    flat->set_access_role(View::AccessRole::meter);
    flat->set_access_label("Flat");
    root.add_child(std::move(flat));

    auto nodes = snapshot_accessibility_tree(root);
    REQUIRE(nodes.size() == 3);
    REQUIRE(nodes[0].has_value);
    REQUIRE(nodes[0].value_string == "100%");
    REQUIRE(nodes[1].value_string == "0%");
    REQUIRE(nodes[2].value_string == "7.25");
}

TEST_CASE("Accessibility helper interfaces expose default selection behavior",
          "[a11y][coverage][issue-654]") {
    TextProbe text("abcdef");
    REQUIRE(text.get_character_count() == 6);
    REQUIRE(text.get_selection() == std::pair<int, int>{0, 0});
    REQUIRE_FALSE(text.is_editable());
    REQUIRE(text.get_text_range(-5, 3) == "abc");
    REQUIRE(text.get_text_range(2, 99) == "cdef");
    REQUIRE(text.get_text_range(4, 2).empty());
    text.set_selection(1, 4);
    REQUIRE(text.get_selection() == std::pair<int, int>{0, 0});
    text.set_text("xy");
    REQUIRE(text.get_character_count() == 2);

    TableProbe table;
    REQUIRE(table.get_selected_row() == -1);
    REQUIRE(table.get_selected_rows().empty());
    REQUIRE_FALSE(table.supports_multi_selection());
    table.select_row(1);
    REQUIRE(table.get_selected_row() == -1);

    CellProbe cell;
    REQUIRE(cell.get_cell_text(2, 1) == "2:1");
    REQUIRE_FALSE(cell.is_cell_editable(0, 0));
    REQUIRE(cell.get_cell_span(0, 0) == std::pair<int, int>{1, 1});
}
