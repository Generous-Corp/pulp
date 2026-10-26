#include <catch2/catch_test_macros.hpp>

#include <pulp/state/store.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace pulp::state;
using namespace pulp::view;

namespace {

std::vector<std::string> child_ids(const View& parent) {
    std::vector<std::string> ids;
    for (size_t i = 0; i < parent.child_count(); ++i)
        ids.push_back(parent.child_at(i)->id());
    return ids;
}

} // namespace

TEST_CASE("insertChild positions a widget the factory could only append",
          "[view][bridge][child-order]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // A panel whose middle row arrives after both of its siblings: the shape a
    // re-opened dropdown produces when it remounts its content into a parent
    // that kept the rest of its children.
    bridge.load_script(R"JS(
        createCol('panel', '');
        createCol('caption', 'panel');
        createCol('footer', 'panel');
        createCol('description', 'panel');
        var moved = insertChild('panel', 'description', 1);
    )JS");

    auto* panel = bridge.widget("panel");
    REQUIRE(panel != nullptr);
    REQUIRE(engine.evaluate("moved").getWithDefault<bool>(false));
    CHECK(child_ids(*panel)
          == std::vector<std::string>{"caption", "description", "footer"});
}

TEST_CASE("a repositioned widget keeps its identity rather than being rebuilt",
          "[view][bridge][child-order]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        createCol('panel', '');
        createCol('first', 'panel');
        createCol('second', 'panel');
    )JS");

    auto* panel = bridge.widget("panel");
    REQUIRE(panel != nullptr);
    const View* second_before = bridge.widget("second");
    REQUIRE(second_before != nullptr);
    const auto lifetime = bridge.widget("second")->import_binding_lifetime_token();

    bridge.load_script("insertChild('panel', 'second', 0);");

    // Same view, still alive: a reorder must not tear the widget down and
    // rebuild it, which is what would drop its focus, drag and popup state.
    CHECK(child_ids(*panel) == std::vector<std::string>{"second", "first"});
    CHECK(bridge.widget("second") == second_before);
    CHECK_FALSE(lifetime.expired());
}

TEST_CASE("insertChild fails closed instead of reparenting or guessing",
          "[view][bridge][child-order]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        createCol('panel', '');
        createCol('other', '');
        createCol('first', 'panel');
        createCol('second', 'panel');
        var foreign_parent = insertChild('other', 'second', 0);
        var unknown_child = insertChild('panel', 'nobody', 0);
        var no_index = insertChild('panel', 'second');
        var negative_index = insertChild('panel', 'second', -1);
        var clamped = insertChild('panel', 'first', 99);
    )JS");

    auto* panel = bridge.widget("panel");
    auto* other = bridge.widget("other");
    REQUIRE(panel != nullptr);
    REQUIRE(other != nullptr);

    CHECK_FALSE(engine.evaluate("foreign_parent").getWithDefault<bool>(true));
    CHECK_FALSE(engine.evaluate("unknown_child").getWithDefault<bool>(true));
    CHECK_FALSE(engine.evaluate("no_index").getWithDefault<bool>(true));
    CHECK_FALSE(engine.evaluate("negative_index").getWithDefault<bool>(true));
    // Naming a parent that does not own the child never moves it across trees.
    CHECK(other->child_count() == 0);
    // An index past the last slot is the one out-of-range value with an
    // unambiguous meaning: put it last.
    CHECK(engine.evaluate("clamped").getWithDefault<bool>(false));
    CHECK(child_ids(*panel) == std::vector<std::string>{"second", "first"});
}

TEST_CASE("insertChild positions the child through an intervening wrapper",
          "[view][bridge][child-order]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        createCol('panel', '');
        createCol('caption', 'panel');
        createCol('wrapper', 'panel');
        createCol('content', 'wrapper');
    )JS");

    auto* panel = bridge.widget("panel");
    REQUIRE(panel != nullptr);

    // The bridge inserts layout wrappers of its own (an overflow box becomes a
    // retained ScrollView around the authored widget), so the view occupying a
    // parent's slot is not always the widget the renderer named. Addressing the
    // inner widget moves the slot it lives in.
    bridge.load_script("var moved = insertChild('panel', 'content', 0);");
    CHECK(engine.evaluate("moved").getWithDefault<bool>(false));
    CHECK(child_ids(*panel) == std::vector<std::string>{"wrapper", "caption"});
}

TEST_CASE("DOM insertBefore and replaceChild keep native order in step with the DOM",
          "[view][bridge][child-order][web-compat]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        var list = document.createElement('div');
        document.body.appendChild(list);
        function row(name) {
            var el = document.createElement('div');
            el.setAttribute('data-row', name);
            return el;
        }
        var a = row('a'), b = row('b'), c = row('c');
        list.appendChild(a);
        list.appendChild(b);
        list.appendChild(c);
        // A new row in the middle, an existing row moved to the front, and a
        // replacement in the middle: the three ways a keyed list changes.
        var d = row('d');
        list.insertBefore(d, b);
        list.insertBefore(c, a);
        var e = row('e');
        list.replaceChild(e, d);
        var names = {};
        [a, b, c, d, e].forEach(function(el) {
            names[el._id] = el.getAttribute('data-row');
        });
        var listId = list._id;
        var domOrder = list._children.map(function(el) {
            return el.getAttribute('data-row');
        }).join(',');
    )JS");

    auto* list = bridge.widget(engine.evaluate("listId").toString());
    REQUIRE(list != nullptr);
    REQUIRE(engine.evaluate("domOrder").toString() == "c,a,e,b");
    std::vector<std::string> native_order;
    for (const auto& id : child_ids(*list))
        native_order.push_back(engine.evaluate("names['" + id + "']").toString());
    CHECK(native_order == std::vector<std::string>{"c", "a", "e", "b"});
}

TEST_CASE("DOM reorder of a keyed row keeps its widget, focus, and applied styles",
          "[view][bridge][child-order][web-compat]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"JS(
        var list = document.createElement('div');
        document.body.appendChild(list);
        var first = document.createElement('div');
        var second = document.createElement('div');
        first.style.background = 'rgb(10,20,30)';
        second.style.background = 'rgb(40,50,60)';
        var field = document.createElement('input');
        second.appendChild(field);
        list.appendChild(first);
        list.appendChild(second);
        field.focus();
        var backgroundWrites = 0;
        var realSetBackground = setBackground;
        setBackground = function(id, value) {
            backgroundWrites++;
            return realSetBackground(id, value);
        };
        var listId = list._id, firstId = first._id, secondId = second._id;
        var fieldId = field._id;
    )JS");

    auto* list = bridge.widget(engine.evaluate("listId").toString());
    const auto first_id = engine.evaluate("firstId").toString();
    const auto second_id = engine.evaluate("secondId").toString();
    auto* field = bridge.widget(engine.evaluate("fieldId").toString());
    REQUIRE(list != nullptr);
    REQUIRE(field != nullptr);
    REQUIRE(field->has_focus());
    const View* second_before = bridge.widget(second_id);
    const auto lifetime = second_before->import_binding_lifetime_token();

    // Swap the rows the way a keyed reconciler does, then put them back.
    engine.evaluate("list.insertBefore(second, first)");
    CHECK(child_ids(*list) == std::vector<std::string>{second_id, first_id});
    CHECK(bridge.widget(second_id) == second_before);
    CHECK_FALSE(lifetime.expired());
    // A detach and re-attach would blur the editor inside the moved row.
    CHECK(field->has_focus());
    // The row's styles are already on its widget; a move does not resend them.
    CHECK(engine.evaluate("backgroundWrites").getWithDefault<int>(-1) == 0);

    engine.evaluate("list.insertBefore(first, second)");
    CHECK(child_ids(*list) == std::vector<std::string>{first_id, second_id});
    CHECK(field->has_focus());

    // Inserting a row before its own next sibling is no move at all.
    engine.evaluate("list.insertBefore(first, second)");
    CHECK(child_ids(*list) == std::vector<std::string>{first_id, second_id});
    CHECK(engine.evaluate("backgroundWrites").getWithDefault<int>(-1) == 0);
}

TEST_CASE("View reorders a child without running the attach lifecycle",
          "[view][child-order]") {
    View parent;
    auto owned_first = std::make_unique<View>();
    auto owned_second = std::make_unique<View>();
    auto owned_third = std::make_unique<View>();
    owned_first->set_id("first");
    owned_second->set_id("second");
    owned_third->set_id("third");
    View* third = owned_third.get();
    parent.add_child(std::move(owned_first));
    parent.add_child(std::move(owned_second));
    parent.add_child(std::move(owned_third));

    REQUIRE(parent.move_child_to_index(third, 0));
    CHECK(child_ids(parent) == std::vector<std::string>{"third", "first", "second"});

    // Moving forwards leaves every other child in its relative order.
    REQUIRE(parent.move_child_to_index(third, 1));
    CHECK(child_ids(parent) == std::vector<std::string>{"first", "third", "second"});

    // Past the end means last; a no-op move still reports success.
    REQUIRE(parent.move_child_to_index(third, 12));
    CHECK(child_ids(parent) == std::vector<std::string>{"first", "second", "third"});
    REQUIRE(parent.move_child_to_index(third, 2));
    CHECK(child_ids(parent) == std::vector<std::string>{"first", "second", "third"});

    View stranger;
    CHECK_FALSE(parent.move_child_to_index(&stranger, 0));
    CHECK_FALSE(parent.move_child_to_index(nullptr, 0));
    CHECK(parent.child_count() == 3);
}
