#include <catch2/catch_test_macros.hpp>
#include <pulp/vellum_components/view_inspector.hpp>

#include <vellum/view/inspector_facts.hpp>
#include <vellum/view/ui_components.hpp>
#include <vellum/view/widgets.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace pulp::vellum_components;
using ::vellum::view::Fader;
using ::vellum::view::InspectorFacts;
using ::vellum::view::Knob;
using ::vellum::view::PropertyList;
using ::vellum::view::ScrollView;
using ::vellum::view::Toggle;
using ::vellum::view::TreeNode;
using ::vellum::view::view_type_name;
using ::vellum::view::XYPad;

namespace {

class DescribedProbeView : public View, public InspectorFacts {
  public:
    std::string_view inspector_type_name() const override {
        return "DescribedProbe";
    }
    void inspector_facts(std::vector<Fact>& out) const override {
        out.push_back({"gain", "Gain", 0.5, false, true});
        out.push_back({"count", "Count", 3.0, true, false});
    }
};

const PropertyList::Property* find_prop(const std::vector<PropertyList::Property>& props,
                                        const std::string& key) {
    for (const auto& p : props)
        if (p.key == key)
            return &p;
    return nullptr;
}

} // namespace

TEST_CASE("ViewInspector names the built-in widgets and shares view-core's classifier",
          "[pulp-vellum-components][inspector]") {
    View v;
    Knob k;
    Fader f;
    Toggle t;
    Label l;
    XYPad xy;
    DescribedProbeView described;

    REQUIRE(ViewInspector::type_name(v) == "View");
    REQUIRE(ViewInspector::type_name(k) == "Knob");
    REQUIRE(ViewInspector::type_name(f) == "Fader");
    REQUIRE(ViewInspector::type_name(t) == "Toggle");
    REQUIRE(ViewInspector::type_name(l) == "Label");
    REQUIRE(ViewInspector::type_name(xy) == "XYPad");
    REQUIRE(ViewInspector::type_name(described) == "DescribedProbe");
    for (const View* view : {static_cast<const View*>(&v), static_cast<const View*>(&k),
                             static_cast<const View*>(&described)})
        REQUIRE(ViewInspector::type_name(*view) == view_type_name(*view));
}

TEST_CASE("ViewInspector counts and finds views by id and anchor",
          "[pulp-vellum-components][inspector]") {
    View root;
    root.set_id("root");
    auto knob = std::make_unique<Knob>();
    knob->set_id("gain");
    knob->set_anchor_id("figma:1:2");
    auto* knob_ptr = knob.get();
    root.add_child(std::move(knob));
    auto panel = std::make_unique<View>();
    panel->add_child(std::make_unique<Label>("test"));
    root.add_child(std::move(panel));

    REQUIRE(ViewInspector::count_views(root) == 4); // root, knob, panel, label

    REQUIRE(ViewInspector::find_by_id(root, "root") == &root);
    REQUIRE(ViewInspector::find_by_id(root, "gain") == knob_ptr);
    REQUIRE(ViewInspector::find_by_id(root, "missing") == nullptr);

    REQUIRE(ViewInspector::find_by_anchor(root, "figma:1:2") == knob_ptr);
    REQUIRE(ViewInspector::find_by_anchor(root, "figma:9:9") == nullptr);
    // Most views carry no anchor; "" must not resolve to the first of them.
    REQUIRE(ViewInspector::find_by_anchor(root, "") == nullptr);
}

TEST_CASE("ViewInspector serializes the tree and widget values to JSON",
          "[pulp-vellum-components][inspector]") {
    View root;
    root.set_id("root");
    root.set_bounds({0, 0, 400, 300});

    auto knob = std::make_unique<Knob>();
    knob->set_id("gain");
    knob->set_bounds({10, 10, 48, 48});
    knob->set_value(0.75f);
    knob->set_label("Gain");
    root.add_child(std::move(knob));

    auto fader = std::make_unique<Fader>();
    fader->set_orientation(Fader::Orientation::horizontal);
    root.add_child(std::move(fader));
    root.add_child(std::make_unique<Label>("Hello"));
    root.add_child(std::make_unique<DescribedProbeView>());

    const auto json = ViewInspector::to_json(root);
    for (const char* needle : {"\"root\"", "\"Knob\"", "\"gain\"", "\"Gain\"", "0.75",
                               "\"horizontal\"", "\"Hello\"", "children", "\"DescribedProbe\""})
        REQUIRE(json.find(needle) != std::string::npos);
    // InspectorFacts members keep their declared JSON key, integral as an integer.
    REQUIRE(json.find("\"gain\": 0.5") != std::string::npos);
    REQUIRE(json.find("\"count\": 3") != std::string::npos);
    REQUIRE(json.find("\"count\": 3.") == std::string::npos);
}

TEST_CASE("ViewInspector builds the tree nodes and property rows",
          "[pulp-vellum-components][inspector]") {
    View root;
    auto knob = std::make_unique<Knob>();
    knob->set_id("gain");
    knob->set_bounds({12, 18, 48, 48});
    knob->set_value(0.5f);
    knob->set_opacity(0.5f);
    auto* knob_ptr = knob.get();
    root.add_child(std::move(knob));

    TreeNode top;
    ViewInspector::populate_tree(top, root);
    REQUIRE(top.children.size() == 1);
    const TreeNode& root_node = *top.children[0];
    REQUIRE(root_node.label == "View");
    REQUIRE(root_node.user_data == &root);
    REQUIRE(root_node.expanded);
    REQUIRE(root_node.children.size() == 1);
    REQUIRE(root_node.children[0]->label == "Knob #gain");
    REQUIRE(root_node.children[0]->user_data == knob_ptr);

    const auto props = ViewInspector::view_properties(*knob_ptr);
    const auto* type = find_prop(props, "type");
    REQUIRE(type != nullptr);
    REQUIRE(std::get<std::string>(type->value) == "Knob");
    REQUIRE(type->category == "Identity");
    REQUIRE(std::get<float>(find_prop(props, "x")->value) == 12.0f);
    REQUIRE(std::get<float>(find_prop(props, "width")->value) == 48.0f);
    REQUIRE(find_prop(props, "width")->category == "Layout");
    REQUIRE(std::get<float>(find_prop(props, "opacity")->value) == 0.5f);
    REQUIRE(find_prop(props, "opacity")->category == "Visual");
    REQUIRE(std::get<float>(find_prop(props, "value")->value) == 0.5f);
    REQUIRE(find_prop(props, "value")->category == "Widget");
    // A view without an id shows a placeholder, not an empty cell.
    REQUIRE(std::get<std::string>(find_prop(ViewInspector::view_properties(root), "id")->value) ==
            "(none)");
}

TEST_CASE("ViewInspector view_properties lists only the facts a widget marks visible",
          "[pulp-vellum-components][inspector]") {
    DescribedProbeView described;
    const auto props = ViewInspector::view_properties(described);
    const auto* gain = find_prop(props, "gain");
    REQUIRE(gain != nullptr);
    REQUIRE(gain->label == "Gain");
    REQUIRE(std::get<float>(gain->value) == 0.5f);
    REQUIRE(find_prop(props, "count") == nullptr);
}

TEST_CASE("ViewInspector absolute_bounds adds every ancestor offset and the scroll shift",
          "[pulp-vellum-components][inspector]") {
    View root;
    root.set_bounds({5, 7, 400, 300});
    auto outer = std::make_unique<View>();
    outer->set_bounds({10, 20, 200, 200});
    auto inner = std::make_unique<View>();
    inner->set_bounds({3, 4, 50, 60});
    auto* inner_ptr = inner.get();
    outer->add_child(std::move(inner));
    root.add_child(std::move(outer));

    // The root's own origin is part of the sum (the walk starts at the view
    // and adds each parent's bounds up to and including the root).
    const auto abs = ViewInspector::absolute_bounds(*inner_ptr);
    REQUIRE(abs.x == 3 + 10 + 5);
    REQUIRE(abs.y == 4 + 20 + 7);
    REQUIRE(abs.width == 50);
    REQUIRE(abs.height == 60);

    // A scrolled ScrollView shifts its children's paint position.
    View frame;
    auto scroll = std::make_unique<ScrollView>();
    scroll->set_bounds({0, 0, 100, 100});
    scroll->set_content_size({100, 400});
    auto row = std::make_unique<View>();
    row->set_bounds({0, 150, 100, 20});
    auto* row_ptr = row.get();
    scroll->add_child(std::move(row));
    auto* scroll_ptr = scroll.get();
    frame.add_child(std::move(scroll));
    const float unscrolled = ViewInspector::absolute_bounds(*row_ptr).y;
    scroll_ptr->set_scroll(0.0f, 60.0f);
    REQUIRE(ViewInspector::absolute_bounds(*row_ptr).y == unscrolled - 60.0f);
}
