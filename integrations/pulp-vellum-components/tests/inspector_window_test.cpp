#include <catch2/catch_test_macros.hpp>
#include <pulp/vellum_components/inspector_window.hpp>

#include <vellum/canvas/recording_canvas.hpp>
#include <vellum/view/inspector_facts.hpp>
#include <vellum/view/theme.hpp>
#include <vellum/view/widgets.hpp>

#include "render_helpers.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::vellum_components;
using namespace pulp::vellum_components::testing;
using ::vellum::canvas::DrawCommand;
using ::vellum::canvas::RecordingCanvas;
using ::vellum::view::Fader;
using ::vellum::view::InspectorFacts;
using ::vellum::view::Knob;
using ::vellum::view::MouseEvent;
using ::vellum::view::TabPanel;
using ::vellum::view::Theme;
using ::vellum::view::Toggle;
using ::vellum::view::TreeView;

namespace {

class DescribedProbeView : public View, public InspectorFacts {
  public:
    std::string_view inspector_type_name() const override {
        return "DescribedProbe";
    }
    void inspector_facts(std::vector<Fact>& out) const override {
        out.push_back({"gain", "Gain", 0.5, false, true});
        out.push_back({"hidden", "Hidden", 9.0, false, false});
    }
};

template <typename T> void collect(View& root, std::vector<T*>& out) {
    if (auto* match = dynamic_cast<T*>(&root))
        out.push_back(match);
    for (std::size_t i = 0; i < root.child_count(); ++i)
        collect(*root.child_at(i), out);
}

template <typename T> T* first_of(View& root) {
    std::vector<T*> found;
    collect(root, found);
    return found.empty() ? nullptr : found.front();
}

bool has_label_containing(View& root, std::string_view text) {
    std::vector<Label*> labels;
    collect(root, labels);
    for (auto* label : labels)
        if (label->text().find(text) != std::string::npos)
            return true;
    return false;
}

TreeView* tree_of(InspectorWindow& window) {
    auto* tabs = first_of<TabPanel>(window);
    REQUIRE(tabs != nullptr);
    auto* tree = first_of<TreeView>(*tabs->child_at(0));
    REQUIRE(tree != nullptr);
    return tree;
}

// The tree row's bounds in the window's root space, derived from the row
// geometry the TreeView paints with, so a click lands on that row.
::vellum::view::Point row_point(TreeView& tree, int row) {
    const auto b = tree.bounds();
    float x = b.x, y = b.y;
    for (View* p = tree.parent(); p; p = p->parent()) {
        x += p->bounds().x;
        y += p->bounds().y;
    }
    return {x + 60.0f, y + (static_cast<float>(row) + 0.5f) * tree.row_height()};
}

// The view's position in the window's root space.
::vellum::view::Rect absolute_bounds(const View& view) {
    auto abs = view.bounds();
    for (const View* p = view.parent(); p; p = p->parent()) {
        abs.x += p->bounds().x;
        abs.y += p->bounds().y;
    }
    return abs;
}

struct Fixture {
    View root;
    Knob* knob = nullptr;
    Label* label = nullptr;
    Fixture() {
        root.set_id("root");
        root.set_bounds({0, 0, 480, 320});
        Theme theme;
        theme.colors["accent.primary"] = ::vellum::canvas::Color::rgba8(10, 20, 30);
        theme.colors["bg.surface"] = ::vellum::canvas::Color::rgba8(40, 50, 60);
        root.set_theme(theme);
        auto k = std::make_unique<Knob>();
        k->set_id("gain");
        k->set_bounds({12, 18, 48, 48});
        k->flex().direction = ::vellum::view::FlexDirection::row;
        k->flex().gap = 3;
        k->flex().padding = 4;
        k->flex().margin = 2;
        k->flex().flex_grow = 1.0f;
        k->flex().flex_shrink = 0.5f;
        k->set_background_color(::vellum::canvas::Color::rgba8(30, 40, 50, 200));
        k->set_border(::vellum::canvas::Color::rgba8(80, 90, 100), 2, 5);
        k->set_opacity(0.75f);
        k->set_value(0.625f);
        k->set_label("Gain");
        knob = k.get();
        root.add_child(std::move(k));
        auto l = std::make_unique<Label>("Ready");
        l->set_id("status");
        label = l.get();
        root.add_child(std::move(l));
    }
};

} // namespace

TEST_CASE("CollapsableSection toggles only from header mouse-down events",
          "[pulp-vellum-components][inspector-window]") {
    CollapsableSection section("Layout", true);
    section.set_bounds({0, 0, 200, 120});
    REQUIRE(section.is_expanded());
    REQUIRE(section.content()->visible());

    MouseEvent move;
    move.position = {8, 8};
    move.is_down = false;
    section.on_mouse_event(move);
    REQUIRE(section.is_expanded());

    MouseEvent content_click;
    content_click.position = {8, 48};
    content_click.is_down = true;
    section.on_mouse_event(content_click);
    REQUIRE(section.is_expanded());

    MouseEvent header_click;
    header_click.position = {8, 8};
    header_click.is_down = true;
    section.on_mouse_event(header_click);
    REQUIRE_FALSE(section.is_expanded());
    REQUIRE_FALSE(section.content()->visible());

    section.set_expanded(true);
    REQUIRE(section.content()->visible());
}

TEST_CASE("InspectorWindow lists the tree and shows the selected view's properties",
          "[pulp-vellum-components][inspector-window]") {
    Fixture fx;
    InspectorWindow window(fx.root);
    // The tool strip sits above the tab panel.
    REQUIRE(window.child_count() == 2);
    auto* tabs = first_of<TabPanel>(window);
    REQUIRE(tabs != nullptr);
    REQUIRE(tabs->tab_count() == 1);
    REQUIRE(tabs->active_tab() == 0);

    auto* tree = tree_of(window);
    auto* knob_node = tree->find_node_by_user_data(fx.knob);
    REQUIRE(knob_node != nullptr);
    REQUIRE(knob_node->label == "Knob #gain");
    REQUIRE(has_label_containing(window, "accent.primary"));
    REQUIRE(has_label_containing(window, "bg.surface"));

    View* picked = nullptr;
    window.on_view_selected = [&](View* v) { picked = v; };
    window.select_view(fx.knob);
    REQUIRE(picked == fx.knob);
    REQUIRE(has_label_containing(window, "Type: Knob"));
    REQUIRE(has_label_containing(window, "ID: gain"));
    REQUIRE(has_label_containing(window, "Bounds: 12, 18  48 x 48"));
    REQUIRE(has_label_containing(window, "Direction: row"));
    REQUIRE(has_label_containing(window, "Gap: 3.0"));
    REQUIRE(has_label_containing(window, "Padding: 4.0"));
    REQUIRE(has_label_containing(window, "Margin: 2.0"));
    REQUIRE(has_label_containing(window, "Grow: 1.0 / Shrink: 0.5"));
    REQUIRE(has_label_containing(window, "Opacity: 0.75"));
    // 0-255 inputs reach the label as the same 0-255 values, alpha as a fraction.
    REQUIRE(has_label_containing(window, "Background: rgba(30, 40, 50, 0.78)"));
    REQUIRE(has_label_containing(window, "Border: 2.0px"));
    REQUIRE(has_label_containing(window, "Corner radius: 5.0"));
    REQUIRE(has_label_containing(window, "Value: 0.625  Label: Gain"));

    Fader fader;
    fader.set_value(0.2f);
    fader.set_label("Mix");
    window.select_view(&fader);
    REQUIRE(has_label_containing(window, "Value: 0.200  Label: Mix"));

    Toggle toggle;
    toggle.set_on(true);
    toggle.set_label("Bypass");
    window.select_view(&toggle);
    REQUIRE(has_label_containing(window, "On: true  Label: Bypass"));

    window.select_view(fx.label);
    REQUIRE(has_label_containing(window, "Text: Ready"));
}

TEST_CASE("InspectorWindow shows InspectorFacts rows for widgets it does not name",
          "[pulp-vellum-components][inspector-window]") {
    View root;
    root.set_bounds({0, 0, 640, 480});
    auto owner = std::make_unique<DescribedProbeView>();
    auto* described = owner.get();
    root.add_child(std::move(owner));

    InspectorWindow window(root);
    window.select_view(described);
    REQUIRE(has_label_containing(window, "Gain: 0.500"));
    REQUIRE_FALSE(has_label_containing(window, "Hidden:"));
}

TEST_CASE("InspectorWindow selects from a headless tree-row click",
          "[pulp-vellum-components][inspector-window]") {
    Fixture fx;
    InspectorWindow window(fx.root);
    window.set_bounds({0, 0, 320, 560});
    window.layout_children();
    auto* tree = tree_of(window);
    REQUIRE(tree->visible_node_count() == 3); // root, knob, label

    View* picked = nullptr;
    int fired = 0;
    window.on_view_selected = [&](View* v) {
        picked = v;
        ++fired;
    };

    // Row 0 is the root, row 1 the knob.
    window.simulate_click(row_point(*tree, 1));
    REQUIRE(fired == 1);
    REQUIRE(picked == fx.knob);
    REQUIRE(tree->selected_node() == tree->find_node_by_user_data(fx.knob));
    REQUIRE(has_label_containing(window, "ID: gain"));

    window.simulate_click(row_point(*tree, 2));
    REQUIRE(picked == fx.label);
    REQUIRE(has_label_containing(window, "ID: status"));
}

TEST_CASE("InspectorWindow read-only mode shows properties without firing the callback",
          "[pulp-vellum-components][inspector-window]") {
    Fixture fx;
    InspectorWindow window(fx.root);
    window.set_bounds({0, 0, 320, 560});
    window.layout_children();
    REQUIRE_FALSE(window.selection_readonly());
    window.set_selection_readonly(true);
    REQUIRE(window.selection_readonly());

    bool fired = false;
    window.on_view_selected = [&](View*) { fired = true; };
    auto* tree = tree_of(window);
    window.simulate_click(row_point(*tree, 1));
    REQUIRE_FALSE(fired);
    REQUIRE(has_label_containing(window, "ID: gain"));

    window.reflect_selection(fx.label);
    REQUIRE_FALSE(fired);
}

TEST_CASE("InspectorWindow reflect_selection highlights the row without re-entering",
          "[pulp-vellum-components][inspector-window]") {
    Fixture fx;
    InspectorWindow window(fx.root);
    auto* tree = tree_of(window);
    int fired = 0;
    window.on_view_selected = [&](View*) { ++fired; };

    window.reflect_selection(fx.label);
    REQUIRE(fired == 0);
    auto* node = tree->find_node_by_user_data(fx.label);
    REQUIRE(node != nullptr);
    REQUIRE(tree->selected_node() == node);
    REQUIRE(has_label_containing(window, "ID: status"));

    // The next idle refresh keeps the highlighted row.
    window.refresh();
    REQUIRE(tree->selected_node() == node);

    // select_view is the write path and does fire.
    window.select_view(fx.label);
    REQUIRE(fired == 1);
}

TEST_CASE("InspectorWindow rebuilds only when the tree structure changes",
          "[pulp-vellum-components][inspector-window]") {
    Fixture fx;
    InspectorWindow window(fx.root);
    auto* tree = tree_of(window);
    auto* node = tree->find_node_by_user_data(fx.knob);
    REQUIRE(node != nullptr);

    // An idle tick on an unchanged tree leaves the nodes alone. A rebuild
    // would rename this node back to its type label.
    node->label = "kept";
    window.refresh();
    REQUIRE(tree->find_node_by_user_data(fx.knob) != nullptr);
    REQUIRE(tree->find_node_by_user_data(fx.knob)->label == "kept");

    // Adding a view changes the structure and rebuilds.
    auto extra = std::make_unique<View>();
    extra->set_id("extra");
    auto* extra_ptr = extra.get();
    fx.root.add_child(std::move(extra));
    REQUIRE(tree->find_node_by_user_data(extra_ptr) == nullptr);
    window.refresh();
    REQUIRE(tree->find_node_by_user_data(extra_ptr) != nullptr);
    REQUIRE(tree->find_node_by_user_data(fx.knob)->label == "Knob #gain");
}

TEST_CASE("InspectorWindow follows a new inspected root",
          "[pulp-vellum-components][inspector-window]") {
    Fixture fx;
    InspectorWindow window(fx.root);
    auto* tree = tree_of(window);
    REQUIRE(tree->find_node_by_user_data(&fx.root) != nullptr);

    View second;
    second.set_id("second");
    auto child = std::make_unique<Label>("Second");
    child->set_id("second-label");
    auto* child_ptr = child.get();
    second.add_child(std::move(child));

    window.set_inspected_root(&second);
    REQUIRE(tree->find_node_by_user_data(&second) != nullptr);
    REQUIRE(tree->find_node_by_user_data(child_ptr) != nullptr);
    REQUIRE(tree->find_node_by_user_data(&fx.root) == nullptr);
    REQUIRE(tree->find_node_by_user_data(fx.knob) == nullptr);
}

TEST_CASE("InspectorWindow shows an empty inspector when the root is cleared",
          "[pulp-vellum-components][inspector-window]") {
    Fixture fx;
    InspectorWindow window(fx.root);
    auto* tree = tree_of(window);
    window.select_view(fx.knob);
    REQUIRE(tree->find_node_by_user_data(fx.knob) != nullptr);
    REQUIRE(has_label_containing(window, "ID: gain"));
    REQUIRE(has_label_containing(window, "accent.primary"));

    window.set_inspected_root(nullptr);
    REQUIRE(tree->find_node_by_user_data(fx.knob) == nullptr);
    REQUIRE(tree->find_node_by_user_data(&fx.root) == nullptr);
    REQUIRE(tree->selected_node() == nullptr);
    REQUIRE_FALSE(has_label_containing(window, "ID: gain"));
    REQUIRE_FALSE(has_label_containing(window, "accent.primary"));
    REQUIRE(has_label_containing(window, "Type: \xE2\x80\x94"));

    // A periodic refresh with no root stays empty, and a new root repopulates.
    REQUIRE_NOTHROW(window.refresh());
    REQUIRE(tree->find_node_by_user_data(fx.knob) == nullptr);
    window.set_inspected_root(&fx.root);
    REQUIRE(tree->find_node_by_user_data(fx.knob) != nullptr);
    REQUIRE(has_label_containing(window, "accent.primary"));
}

TEST_CASE("InspectorWindow drops a selection whose view is gone after a rebuild",
          "[pulp-vellum-components][inspector-window]") {
    View root;
    root.set_id("root");
    auto knob = std::make_unique<Knob>();
    knob->set_id("gain");
    auto* knob_ptr = knob.get();
    root.add_child(std::move(knob));

    InspectorWindow window(root);
    auto* tree = tree_of(window);
    window.select_view(knob_ptr);
    REQUIRE(has_label_containing(window, "ID: gain"));

    // Keep the removed view alive until the replacement is allocated so the
    // allocator cannot recycle the address for the new child.
    auto removed = root.remove_child(knob_ptr);
    auto fresh = std::make_unique<Label>("fresh");
    fresh->set_id("fresh");
    root.add_child(std::move(fresh));
    removed.reset();

    REQUIRE_NOTHROW(window.refresh());
    REQUIRE(tree->find_node_by_user_data(knob_ptr) == nullptr);
    REQUIRE_NOTHROW(window.refresh());
}

TEST_CASE("InspectorWindow tool strip round-trips the active tool and picks by click",
          "[pulp-vellum-components][inspector-window]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorWindow window(root);

    REQUIRE(window.active_tool() == 0);
    window.set_active_tool(1);
    REQUIRE(window.active_tool() == 1);
    window.set_active_tool(42);
    REQUIRE(window.active_tool() == 0);
    window.set_active_tool(-7);
    REQUIRE(window.active_tool() == 0);

    int picked = -1;
    window.on_tool_picked = [&](int idx) { picked = idx; };
    window.set_bounds({0, 0, 400, 600});
    window.layout_children();
    View* strip = window.child_at(0);
    REQUIRE(strip != nullptr);

    // Second button centre: x = 6 + (40 + 4) + 20 = 70.
    window.simulate_click({70.0f, strip->bounds().y + strip->bounds().height * 0.5f});
    REQUIRE(picked == 1);
    window.simulate_click({26.0f, strip->bounds().y + strip->bounds().height * 0.5f});
    REQUIRE(picked == 0);
}

TEST_CASE("InspectorWindow tool strip shows the hovered button's tooltip",
          "[pulp-vellum-components][inspector-window]") {
    const auto tip_drawn = [](InspectorWindow& window, const char* tip) {
        RecordingCanvas canvas;
        window.paint_all(canvas);
        for (const auto& c : canvas.commands())
            if (c.type == DrawCommand::Type::fill_text && c.text == tip)
                return true;
        return false;
    };
    View root;
    root.set_bounds({0, 0, 400, 300});
    InspectorWindow window(root);
    window.set_bounds({0, 0, 400, 600});
    window.layout_children();
    View* strip = window.child_at(0);
    const float y = strip->bounds().y + strip->bounds().height * 0.5f;

    REQUIRE_FALSE(tip_drawn(window, "Select (V)"));
    window.simulate_hover({26.0f, y});
    REQUIRE(tip_drawn(window, "Select (V)"));
    REQUIRE_FALSE(tip_drawn(window, "Text (T)"));
    window.simulate_hover({70.0f, y});
    REQUIRE(tip_drawn(window, "Text (T)"));
    REQUIRE_FALSE(tip_drawn(window, "Select (V)"));
}

TEST_CASE("InspectorWindow paints the chrome in real colours, not saturated ones",
          "[pulp-vellum-components][inspector-window]") {
    Fixture fx;
    InspectorWindow window(fx.root);
    window.set_bounds({0, 0, 320, 800});
    window.layout_children();
    window.simulate_click(row_point(*tree_of(window), 1)); // select the knob
    const auto frame = render(window, 320, 800);
    // The window background is rgb(24, 24, 32). A 0-255 value written straight
    // into a float channel would saturate to white.
    const auto corner = frame.at(310, 790);
    REQUIRE(near(corner, 24, 24, 32, 6));
    // The tool strip's active button is the accent rgb(80, 160, 255).
    REQUIRE(near(frame.at(20, 14), 80, 160, 255, 6));
    maybe_write_png(window, 320, 800, "inspector-window.png");
}

TEST_CASE("CollapsableSection paints its title and toggles from a real header click",
          "[pulp-vellum-components][inspector-window]") {
    Fixture fx;
    InspectorWindow window(fx.root);
    window.set_bounds({0, 0, 320, 560});
    window.layout_children();

    std::vector<CollapsableSection*> sections;
    collect(window, sections);
    REQUIRE(sections.size() == 5);
    CollapsableSection* identity = sections.front();
    REQUIRE(identity->is_expanded());

    // The title is drawn over the header background: some header pixel is a
    // bright glyph pixel, not the header's own dark fill.
    const auto abs = absolute_bounds(*identity);
    const auto frame = render(window, 320, 560);
    bool glyph_ink = false;
    for (int y = static_cast<int>(abs.y); y < static_cast<int>(abs.y) + 22; ++y)
        for (int x = static_cast<int>(abs.x) + 20; x < static_cast<int>(abs.x) + 80; ++x)
            if (frame.at(x, y).r > 150)
                glyph_ink = true;
    REQUIRE(glyph_ink);

    // A click on the header, as the platform host delivers it, collapses it.
    window.simulate_click({abs.x + 30.0f, abs.y + 8.0f});
    REQUIRE_FALSE(identity->is_expanded());
    window.simulate_click({abs.x + 30.0f, abs.y + 8.0f});
    REQUIRE(identity->is_expanded());
}
