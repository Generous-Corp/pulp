// Tests for pulp::view::WidgetMetrics — the sizing delegate — and for the two
// widgets that were reworked to consult it.
//
// A painter answers "what pixels?". Metrics answers "how big?". They are
// separate objects because they run at different times: metrics is consulted
// during LAYOUT, where there is no canvas and no frame, possibly many times.
// The cases below pin exactly that, plus the properties a skin depends on:
//
//   * A menu can compute its whole geometry with NO canvas and NO paint. Before
//     this, a menu could only discover its own size while painting, which meant
//     nothing could ask it how big it wanted to be before showing it.
//   * Every hook declines by default, so a delegate that only wants menus wider
//     is not conscripted into sizing everything else.
//   * Rows are not assumed uniform. A skin may make a separator two pixels tall
//     and a label row twenty, so hit-testing walks the row list rather than
//     dividing by a constant.
//   * A Label can open a real inline editor, which is a real child view, so an
//     ancestor's delegate styles it like any other field.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/view/context_menu.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_metrics.hpp>
#include <pulp/view/widgets.hpp>

#include <memory>
#include <vector>

using namespace pulp::view;

namespace {

/// Sizes menus to fixed, obviously-not-stock numbers so a test can tell whether
/// the delegate was consulted at all. Each hook can be switched off individually
/// to prove the fall-through is per-hook, not all-or-nothing.
struct FixedMetrics : WidgetMetrics {
    bool do_item = true;
    bool do_border = true;
    bool do_font = true;

    float row_w = 200.0f;
    float row_h = 30.0f;
    float sep_h = 3.0f;
    float border = 8.0f;

    int item_queries = 0;

    bool menu_item_size(const MenuItemMetricsQuery& q, BoxSize& out, View&) override {
        ++item_queries;
        if (!do_item) return false;
        out.width = row_w;
        out.height = q.separator ? sep_h : row_h;
        return true;
    }
    bool menu_border(float& out, View&) override {
        if (!do_border) return false;
        out = border;
        return true;
    }
    bool menu_font(FontSpec& out, View&) override {
        if (!do_font) return false;
        out.family = "Georgia";
        out.size = 19.0f;
        return true;
    }
};

std::vector<ContextMenu::Item> three_rows_and_a_separator() {
    return {
        {1, "Cut"},
        {2, "Copy"},
        ContextMenu::Item::make_separator(),
        {3, "Paste"},
    };
}

} // namespace

TEST_CASE("a view with no metrics delegate resolves to none", "[view][metrics]") {
    View v;
    REQUIRE(v.effective_metrics() == nullptr);
}

TEST_CASE("a metrics delegate applies to the whole subtree", "[view][metrics]") {
    auto root = std::make_unique<View>();
    auto* root_raw = root.get();
    auto leaf = std::make_unique<View>();
    auto* leaf_raw = leaf.get();
    root_raw->add_child(std::move(leaf));

    auto m = std::make_shared<FixedMetrics>();
    root_raw->set_metrics(m);

    REQUIRE(root_raw->effective_metrics() == m.get());
    REQUIRE(leaf_raw->effective_metrics() == m.get());
}

TEST_CASE("a menu computes its geometry with no canvas and no paint",
          "[view][metrics][menu]") {
    // The whole point of separating metrics from painting: a caller can ask a
    // menu how big it wants to be BEFORE it is ever shown. Nothing below
    // constructs a canvas or paints a frame.
    ContextMenu menu;
    menu.set_items(three_rows_and_a_separator());

    const auto lay = menu.layout();

    REQUIRE(lay.box.width > 0.0f);
    REQUIRE(lay.box.height > 0.0f);
    REQUIRE(lay.rows.size() == 4);          // one rect per item, in item order
    REQUIRE(menu.measured_width() == lay.box.width);
}

TEST_CASE("a menu's stock rows are not uniform height", "[view][metrics][menu]") {
    // A separator is a hairline, not a full row. Code that hit-tests by dividing
    // by a row constant is wrong the moment a separator exists.
    ContextMenu menu;
    menu.set_items(three_rows_and_a_separator());

    const auto lay = menu.layout();
    const float label_h = lay.rows[0].height;
    const float sep_h   = lay.rows[2].height;   // the separator

    REQUIRE(sep_h < label_h);

    // And the rows stack: each one starts where the last ended.
    for (size_t i = 1; i < lay.rows.size(); ++i)
        REQUIRE_THAT(lay.rows[i].y,
                     Catch::Matchers::WithinAbs(lay.rows[i - 1].y + lay.rows[i - 1].height,
                                                0.01));
}

TEST_CASE("a menu takes its row sizes and border and font from the delegate",
          "[view][metrics][menu]") {
    ContextMenu menu;
    menu.set_items(three_rows_and_a_separator());

    auto m = std::make_shared<FixedMetrics>();
    menu.set_metrics(m);

    const auto lay = menu.layout();

    REQUIRE(m->item_queries >= 4);                     // asked about every row
    REQUIRE_THAT(lay.border, Catch::Matchers::WithinAbs(8.0, 1e-4));
    REQUIRE(lay.font.family == "Georgia");
    REQUIRE_THAT(lay.font.size, Catch::Matchers::WithinAbs(19.0, 1e-4));

    // Panel width = widest row + the border on both sides.
    REQUIRE_THAT(lay.box.width,
                 Catch::Matchers::WithinAbs(200.0 + 2 * 8.0, 0.01));

    // Height = the stacked rows (3 label rows + 1 separator) + border on both.
    REQUIRE_THAT(lay.box.height,
                 Catch::Matchers::WithinAbs(3 * 30.0 + 3.0 + 2 * 8.0, 0.01));

    REQUIRE_THAT(lay.rows[0].height, Catch::Matchers::WithinAbs(30.0, 0.01));
    REQUIRE_THAT(lay.rows[2].height, Catch::Matchers::WithinAbs(3.0, 0.01));
}

TEST_CASE("a delegate that declines a hook leaves that ONE metric stock",
          "[view][metrics][menu]") {
    // Partial override, per hook. A skin that only wants a thicker border must
    // not be dragged into sizing every row and choosing a font.
    ContextMenu stock;
    stock.set_items(three_rows_and_a_separator());
    const auto stock_lay = stock.layout();

    ContextMenu menu;
    menu.set_items(three_rows_and_a_separator());

    auto m = std::make_shared<FixedMetrics>();
    m->do_item = false;      // decline row sizes
    m->do_font = false;      // decline the font
    m->do_border = true;     // ONLY override the border
    menu.set_metrics(m);

    const auto lay = menu.layout();

    REQUIRE(m->item_queries >= 4);                        // it was still ASKED
    REQUIRE_THAT(lay.border, Catch::Matchers::WithinAbs(8.0, 1e-4));  // and honoured here

    // ...but the declined hooks fell back to the stock look, unchanged.
    REQUIRE(lay.font.family == stock_lay.font.family);
    REQUIRE_THAT(lay.rows[0].height,
                 Catch::Matchers::WithinAbs(stock_lay.rows[0].height, 0.01));
}

TEST_CASE("a menu asks the delegate what KIND of row each one is",
          "[view][metrics][menu]") {
    // The query carries the row's nature — separator, header, ticked, submenu —
    // because a skin sizes a section header differently from a command row, and
    // there is no other way for it to know.
    struct Recording : WidgetMetrics {
        std::vector<MenuItemMetricsQuery> seen;
        bool menu_item_size(const MenuItemMetricsQuery& q, BoxSize&, View&) override {
            seen.push_back(q);
            return false;
        }
    };

    ContextMenu menu;
    menu.set_items({
        ContextMenu::Item::make_header("Edit"),
        {1, "Cut"},
        ContextMenu::Item::make_separator(),
        {2, "Paste"},
    });

    auto m = std::make_shared<Recording>();
    menu.set_metrics(m);
    (void)menu.layout();

    REQUIRE(m->seen.size() == 4);
    REQUIRE(m->seen[0].header);
    REQUIRE(m->seen[0].text == "Edit");
    REQUIRE_FALSE(m->seen[1].header);
    REQUIRE_FALSE(m->seen[1].separator);
    REQUIRE(m->seen[1].text == "Cut");
    REQUIRE(m->seen[2].separator);

    // And each query carries the height the widget WOULD use, so a delegate can
    // honour a caller's row height instead of forcing its own.
    REQUIRE(m->seen[1].standard_height > 0.0f);
}

TEST_CASE("a menu's rows all sit inside its panel", "[view][metrics][menu]") {
    // A geometry invariant worth pinning: whatever the delegate says, no row may
    // escape the box that was sized to hold it.
    ContextMenu menu;
    menu.set_items(three_rows_and_a_separator());
    auto m = std::make_shared<FixedMetrics>();
    menu.set_metrics(m);

    const auto lay = menu.layout();
    for (const auto& r : lay.rows) {
        REQUIRE(r.x >= lay.box.x - 0.01f);
        REQUIRE(r.y >= lay.box.y - 0.01f);
        REQUIRE(r.x + r.width  <= lay.box.x + lay.box.width  + 0.01f);
        REQUIRE(r.y + r.height <= lay.box.y + lay.box.height + 0.01f);
    }
}

TEST_CASE("a label opens a real inline editor on the trigger it was given",
          "[view][metrics][label]") {
    // The editor is a real child view, not a bespoke overlay, which is what lets
    // an ancestor's painter and metrics delegate style it like any other field.
    Label label("Rename me");
    label.set_bounds({0, 0, 120, 20});
    label.set_editable(Label::EditTrigger::single_click);

    REQUIRE(label.wants_mouse_input());
    REQUIRE_FALSE(label.editor_active());

    label.on_mouse_down({10.0f, 10.0f});

    REQUIRE(label.editor_active());
    REQUIRE(label.editor() != nullptr);
}

TEST_CASE("a non-editable label does not want the mouse at all",
          "[view][metrics][label]") {
    Label label("Just text");
    label.set_bounds({0, 0, 120, 20});

    REQUIRE_FALSE(label.wants_mouse_input());

    label.on_mouse_down({10.0f, 10.0f});
    REQUIRE_FALSE(label.editor_active());
}

TEST_CASE("a label commits an edit and reports the new text once",
          "[view][metrics][label]") {
    Label label("old");
    label.set_bounds({0, 0, 120, 20});
    label.set_editable(Label::EditTrigger::single_click);

    std::vector<std::string> committed;
    label.on_text_change = [&](const std::string& t) { committed.push_back(t); };

    label.show_editor();
    REQUIRE(label.editor_active());
    label.editor()->set_text("new");

    label.hide_editor(/*commit=*/true);

    REQUIRE_FALSE(label.editor_active());
    REQUIRE(label.text() == "new");
    REQUIRE(committed.size() == 1);          // committed, not per-keystroke
    REQUIRE(committed.front() == "new");
}

TEST_CASE("a label reverting an edit keeps the old text and says nothing",
          "[view][metrics][label]") {
    Label label("old");
    label.set_bounds({0, 0, 120, 20});
    label.set_editable(Label::EditTrigger::single_click);

    int fired = 0;
    label.on_text_change = [&](const std::string&) { ++fired; };

    label.show_editor();
    label.editor()->set_text("discarded");
    label.hide_editor(/*commit=*/false);

    REQUIRE(label.text() == "old");
    REQUIRE(fired == 0);                     // a reverted edit is not a change
}

TEST_CASE("a trailing separator divides nothing and takes no space",
          "[view][metrics][menu]") {
    // A separator is a divider BETWEEN rows. One at the end divides nothing, so
    // it is dropped rather than adding a strip of empty panel to the bottom of
    // the menu. Its rect survives, empty, so rows stays index-aligned with items
    // -- code that walks both together must not have to special-case the drop.
    ContextMenu menu;
    menu.set_items({{1, "Cut"}, {2, "Copy"}, ContextMenu::Item::make_separator()});

    const auto lay = menu.layout();

    REQUIRE(lay.rows.size() == 3);                  // still one rect per item
    REQUIRE_THAT(lay.rows[2].height, Catch::Matchers::WithinAbs(0.0, 1e-4));

    // And the panel is exactly as tall as a menu with no separator at all.
    ContextMenu bare;
    bare.set_items({{1, "Cut"}, {2, "Copy"}});
    REQUIRE_THAT(lay.box.height,
                 Catch::Matchers::WithinAbs(bare.layout().box.height, 1e-4));
}

TEST_CASE("a separator is a hairline rather than a row", "[view][metrics][menu]") {
    // It was sized as a full 24px row, which left every divider floating in an
    // empty row's worth of space and made the panel taller than its contents.
    ContextMenu with_sep;
    with_sep.set_items({{1, "Cut"}, ContextMenu::Item::make_separator(), {2, "Paste"}});

    ContextMenu without;
    without.set_items({{1, "Cut"}, {2, "Paste"}});

    const auto bare = without.layout();
    const float row_h = bare.rows[0].height;
    const float sep_cost = with_sep.layout().box.height - bare.box.height;

    REQUIRE(sep_cost > 0.0f);            // it does occupy SOME space
    REQUIRE(sep_cost < row_h * 0.5f);    // but nothing like a full row
}

TEST_CASE("a delegate can hand back the standard height and keep the stock rhythm",
          "[view][metrics][menu]") {
    // standard_height exists so a skin that only cares about WIDTH can keep the
    // menu's vertical rhythm without rediscovering it. It was passed as a
    // constant zero, so a delegate echoing it back collapsed every row to nothing
    // -- the field was documented, and dead.
    struct WidenOnly : WidgetMetrics {
        bool menu_item_size(const MenuItemMetricsQuery& q, BoxSize& out, View&) override {
            out.width = 300.0f;              // the only thing this skin has an opinion about
            out.height = q.standard_height;  // ...and defer on the other axis
            return true;
        }
    };

    ContextMenu stock;
    stock.set_items(three_rows_and_a_separator());
    const auto stock_lay = stock.layout();

    ContextMenu wide;
    wide.set_items(three_rows_and_a_separator());
    wide.set_metrics(std::make_shared<WidenOnly>());
    const auto wide_lay = wide.layout();

    REQUIRE_THAT(wide_lay.box.width, Catch::Matchers::WithinAbs(300.0, 0.01));

    // Every row kept exactly the height it would have had, separator included.
    REQUIRE(wide_lay.rows.size() == stock_lay.rows.size());
    for (size_t i = 0; i < wide_lay.rows.size(); ++i)
        REQUIRE_THAT(wide_lay.rows[i].height,
                     Catch::Matchers::WithinAbs(stock_lay.rows[i].height, 0.01));

    REQUIRE_THAT(wide_lay.box.height,
                 Catch::Matchers::WithinAbs(stock_lay.box.height, 0.01));
}

TEST_CASE("every metric hook declines by default", "[view][metrics]") {
    // The same contract the paint delegate has, and for the same reason: a skin
    // that only wants wider menus must not be conscripted into sizing buttons,
    // combo boxes, text fields and carets as well.
    //
    // Every hook is called here on a bare delegate. Each must decline AND leave
    // its out-parameter untouched — a hook that returned false but had already
    // written to `out` would hand the widget a half-populated value it believes
    // it is free to ignore, which is precisely how a "declined" metric leaks.
    struct Bare : WidgetMetrics {};
    Bare m;

    View v;
    v.set_bounds({0, 0, 100, 30});

    MenuItemMetricsQuery query;
    query.text = "Paste";
    query.standard_height = 24.0f;

    BoxSize size{7.0f, 9.0f};                 // sentinels
    CHECK_FALSE(m.menu_item_size(query, size, v));
    CHECK(size.width == 7.0f);                // untouched
    CHECK(size.height == 9.0f);

    float border = 3.0f;
    CHECK_FALSE(m.menu_border(border, v));
    CHECK(border == 3.0f);

    FontSpec font;
    font.family = "Sentinel";
    font.size = 11.0f;
    CHECK_FALSE(m.menu_font(font, v));
    CHECK_FALSE(m.button_font(font, v));
    CHECK_FALSE(m.combo_box_font(font, v));
    CHECK_FALSE(m.text_field_font(font, v));
    CHECK(font.family == "Sentinel");
    CHECK(font.size == 11.0f);

    EdgeInsets insets{1.0f, 2.0f, 3.0f, 4.0f};
    CHECK_FALSE(m.text_field_insets(insets, v));
    CHECK(insets.top == 1.0f);
    CHECK(insets.left == 4.0f);

    float caret = 2.5f;
    CHECK_FALSE(m.caret_width(caret, v));
    CHECK(caret == 2.5f);
}

TEST_CASE("EdgeInsets sums the axes a caller actually needs", "[view][metrics]") {
    const EdgeInsets e{1.0f, 2.0f, 4.0f, 8.0f};   // top right bottom left
    REQUIRE_THAT(e.horizontal(), Catch::Matchers::WithinAbs(10.0, 1e-6));  // left + right
    REQUIRE_THAT(e.vertical(), Catch::Matchers::WithinAbs(5.0, 1e-6));     // top + bottom
}
