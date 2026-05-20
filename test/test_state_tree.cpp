#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/state/cached_property.hpp>
#include <pulp/state/state_tree.hpp>
#include <pulp/state/state_tree_sync.hpp>

using namespace pulp::state;
using Catch::Matchers::WithinAbs;

// ── Basic property operations ───────────────────────────────────────────

TEST_CASE("StateTree create and type name", "[state][tree]") {
    auto tree = StateTree::create("oscillator");
    REQUIRE(tree->type_name() == "oscillator");
}

TEST_CASE("StateTree set and get properties", "[state][tree]") {
    auto tree = StateTree::create("params");

    tree->set("name", std::string("Gain"));
    tree->set("value", 0.75);
    tree->set("enabled", true);
    tree->set("index", int64_t(3));

    REQUIRE(tree->get_string("name") == "Gain");
    REQUIRE_THAT(tree->get_double("value"), WithinAbs(0.75, 1e-5));
    REQUIRE(tree->get_bool("enabled") == true);
    REQUIRE(tree->get_int("index") == 3);
}

TEST_CASE("StateTree get with defaults", "[state][tree]") {
    auto tree = StateTree::create("test");

    REQUIRE(tree->get_string("missing", "default") == "default");
    REQUIRE(tree->get_int("missing", 42) == 42);
    REQUIRE_THAT(tree->get_double("missing", 3.14), WithinAbs(3.14, 1e-5));
    REQUIRE(tree->get_bool("missing", true) == true);
}

TEST_CASE("StateTree typed getters keep defaults for mismatched property types",
          "[state][tree][issue-641]") {
    auto tree = StateTree::create("test");
    tree->set("as_string", std::string("123"));
    tree->set("as_bool", true);
    tree->set("as_int", int64_t(9));
    tree->set("as_double", 1.25);

    REQUIRE(tree->get_int("as_string", 77) == 77);
    REQUIRE(tree->get_bool("as_string", true));
    REQUIRE(tree->get_string("as_int", "fallback") == "fallback");
    REQUIRE_THAT(tree->get_double("as_int", 0.0), WithinAbs(9.0, 1e-9));
    REQUIRE(tree->get_int("as_double", 44) == 44);
    REQUIRE(tree->get_bool("as_int", false) == false);
    REQUIRE(tree->get_bool("as_bool", false) == true);
}

TEST_CASE("StateTree typed getters fall back on mismatched property types",
          "[state][tree][codecov]") {
    auto tree = StateTree::create("test");
    tree->set("bool_value", true);
    tree->set("int_value", int64_t(9));
    tree->set("double_value", 2.5);
    tree->set("string_value", std::string("text"));

    REQUIRE(tree->get_bool("string_value", true));
    REQUIRE(tree->get_int("double_value", 42) == 42);
    REQUIRE_THAT(tree->get_double("int_value", -1.0), WithinAbs(9.0, 1e-5));
    REQUIRE(tree->get_string("bool_value", "fallback") == "fallback");
}

TEST_CASE("StateTree has and remove", "[state][tree]") {
    auto tree = StateTree::create("test");
    tree->set("key", std::string("value"));

    REQUIRE(tree->has("key"));
    REQUIRE_FALSE(tree->has("other"));

    tree->remove("key");
    REQUIRE_FALSE(tree->has("key"));
}

TEST_CASE("StateTree property names", "[state][tree]") {
    auto tree = StateTree::create("test");
    tree->set("b", int64_t(2));
    tree->set("a", int64_t(1));

    auto names = tree->property_names();
    REQUIRE(names.size() == 2);
    // std::map orders alphabetically
    REQUIRE(names[0] == "a");
    REQUIRE(names[1] == "b");
}

// ── Children ────────────────────────────────────────────────────────────

TEST_CASE("StateTree add and access children", "[state][tree]") {
    auto root = StateTree::create("synth");
    auto osc = StateTree::create("oscillator");
    auto filter = StateTree::create("filter");

    root->add_child(osc);
    root->add_child(filter);

    REQUIRE(root->child_count() == 2);
    REQUIRE(root->child(0)->type_name() == "oscillator");
    REQUIRE(root->child(1)->type_name() == "filter");
    REQUIRE(osc->parent() == root.get());
}

TEST_CASE("StateTree find children by type", "[state][tree]") {
    auto root = StateTree::create("plugin");
    root->add_child(StateTree::create("param"));
    root->add_child(StateTree::create("param"));
    root->add_child(StateTree::create("bus"));

    auto params = root->find_children("param");
    REQUIRE(params.size() == 2);

    auto bus = root->find_child("bus");
    REQUIRE(bus != nullptr);
    REQUIRE(bus->type_name() == "bus");

    REQUIRE(root->find_child("nonexistent") == nullptr);
}

TEST_CASE("StateTree remove child by index", "[state][tree]") {
    auto root = StateTree::create("root");
    root->add_child(StateTree::create("a"));
    root->add_child(StateTree::create("b"));
    root->add_child(StateTree::create("c"));

    root->remove_child(1);
    REQUIRE(root->child_count() == 2);
    REQUIRE(root->child(0)->type_name() == "a");
    REQUIRE(root->child(1)->type_name() == "c");
}

TEST_CASE("StateTree child access and removal ignore invalid indexes",
          "[state][tree][issue-641]") {
    auto root = StateTree::create("root");
    root->add_child(StateTree::create("a"));

    int removed_count = 0;
    root->add_child_removed_listener(
        [&](StateTree&, StateTree&, int) { removed_count++; });

    REQUIRE(root->child(-1) == nullptr);
    REQUIRE(root->child(1) == nullptr);
    root->remove_child(-1);
    root->remove_child(99);
    REQUIRE(root->child_count() == 1);
    REQUIRE(removed_count == 0);
}

TEST_CASE("StateTree child removal ignores invalid inputs and clears parent",
          "[state][tree][codecov]") {
    auto root = StateTree::create("root");
    auto child = StateTree::create("child");
    auto stranger = StateTree::create("stranger");

    root->add_child(child);
    REQUIRE(child->parent() == root.get());

    root->remove_child(-1);
    root->remove_child(99);
    root->remove_child(stranger.get());
    root->remove_child(static_cast<StateTree*>(nullptr));

    REQUIRE(root->child_count() == 1);
    REQUIRE(child->parent() == root.get());

    root->remove_child(child.get());
    REQUIRE(root->child_count() == 0);
    REQUIRE(child->parent() == nullptr);
}

TEST_CASE("StateTree insert child at index", "[state][tree]") {
    auto root = StateTree::create("root");
    root->add_child(StateTree::create("a"));
    root->add_child(StateTree::create("c"));

    root->insert_child(1, StateTree::create("b"));
    REQUIRE(root->child_count() == 3);
    REQUIRE(root->child(1)->type_name() == "b");
}

TEST_CASE("StateTree insert at end and missing child pointer removal are stable",
          "[state][tree][issue-641]") {
    auto root = StateTree::create("root");
    auto missing = StateTree::create("missing");

    root->add_child(StateTree::create("a"));
    root->insert_child(root->child_count(), StateTree::create("b"));
    root->remove_child(missing.get());

    REQUIRE(root->child_count() == 2);
    REQUIRE(root->child(0)->type_name() == "a");
    REQUIRE(root->child(1)->type_name() == "b");
    REQUIRE(missing->parent() == nullptr);
}

TEST_CASE("StateTree child insertion clamps invalid indexes and skips null children",
          "[state][tree][codecov]") {
    auto root = StateTree::create("root");
    root->add_child(nullptr);
    root->insert_child(10, nullptr);
    REQUIRE(root->child_count() == 0);

    root->insert_child(-10, StateTree::create("first"));
    root->insert_child(99, StateTree::create("last"));

    REQUIRE(root->child_count() == 2);
    REQUIRE(root->child(0)->type_name() == "first");
    REQUIRE(root->child(1)->type_name() == "last");
}

TEST_CASE("StateTree reparenting detaches children from the old parent",
          "[state][tree][codecov]") {
    auto old_parent = StateTree::create("old");
    auto new_parent = StateTree::create("new");
    auto sibling = StateTree::create("sibling");
    auto child = StateTree::create("child");

    int old_removed_count = 0;
    old_parent->add_child_removed_listener(
        [&](StateTree&, StateTree& removed, int index) {
            REQUIRE(&removed == child.get());
            REQUIRE(index == 0);
            ++old_removed_count;
        });

    old_parent->add_child(child);
    old_parent->add_child(sibling);
    REQUIRE(old_parent->child_count() == 2);

    new_parent->add_child(child);

    REQUIRE(old_removed_count == 1);
    REQUIRE(old_parent->child_count() == 1);
    REQUIRE(old_parent->child(0) == sibling);
    REQUIRE(new_parent->child_count() == 1);
    REQUIRE(new_parent->child(0) == child);
    REQUIRE(child->parent() == new_parent.get());
}

TEST_CASE("StateTree insert reparents children at the requested new index",
          "[state][tree][codecov]") {
    auto old_parent = StateTree::create("old");
    auto new_parent = StateTree::create("new");
    auto first = StateTree::create("first");
    auto moved = StateTree::create("moved");
    auto last = StateTree::create("last");

    old_parent->add_child(moved);
    new_parent->add_child(first);
    new_parent->add_child(last);

    new_parent->insert_child(1, moved);

    REQUIRE(old_parent->child_count() == 0);
    REQUIRE(new_parent->child_count() == 3);
    REQUIRE(new_parent->child(0) == first);
    REQUIRE(new_parent->child(1) == moved);
    REQUIRE(new_parent->child(2) == last);
    REQUIRE(moved->parent() == new_parent.get());
}

// ── Listeners ───────────────────────────────────────────────────────────

TEST_CASE("StateTree property change listener", "[state][tree]") {
    auto tree = StateTree::create("test");

    std::string changed_prop;
    tree->add_listener([&](StateTree&, std::string_view prop, const PropertyValue&, const PropertyValue&) {
        changed_prop = std::string(prop);
    });

    tree->set("volume", 0.5);
    REQUIRE(changed_prop == "volume");
}

TEST_CASE("StateTree property listener receives old and new values",
          "[state][tree][issue-641]") {
    auto tree = StateTree::create("test");
    std::vector<PropertyValue> old_values;
    std::vector<PropertyValue> new_values;

    tree->add_listener(
        [&](StateTree&, std::string_view, const PropertyValue& old_val,
            const PropertyValue& new_val) {
            old_values.push_back(old_val);
            new_values.push_back(new_val);
        });

    tree->set("gain", 0.25);
    tree->set("gain", 0.5);

    REQUIRE(old_values.size() == 2);
    REQUIRE(std::holds_alternative<std::monostate>(old_values[0]));
    REQUIRE_THAT(std::get<double>(new_values[0]), WithinAbs(0.25, 1e-9));
    REQUIRE_THAT(std::get<double>(old_values[1]), WithinAbs(0.25, 1e-9));
    REQUIRE_THAT(std::get<double>(new_values[1]), WithinAbs(0.5, 1e-9));
}

TEST_CASE("StateTree remove emits property callbacks with a cleared value",
          "[state][tree][issue-641]") {
    auto tree = StateTree::create("test");
    tree->set("name", std::string("before"));

    std::string changed_prop;
    PropertyValue old_value;
    PropertyValue new_value;
    int count = 0;
    tree->add_listener([&](StateTree&, std::string_view prop,
                           const PropertyValue& old_val,
                           const PropertyValue& next_val) {
        changed_prop = std::string(prop);
        old_value = old_val;
        new_value = next_val;
        count++;
    });

    tree->remove("name");
    tree->remove("missing");
    REQUIRE(count == 1);
    REQUIRE(changed_prop == "name");
    REQUIRE(std::get<std::string>(old_value) == "before");
    REQUIRE(std::holds_alternative<std::monostate>(new_value));
    REQUIRE_FALSE(tree->has("name"));
}

TEST_CASE("StateTree child added listener", "[state][tree]") {
    auto root = StateTree::create("root");

    int added_count = 0;
    root->add_child_added_listener([&](StateTree&, StateTree&, int) {
        added_count++;
    });

    root->add_child(StateTree::create("a"));
    root->add_child(StateTree::create("b"));
    REQUIRE(added_count == 2);
}

TEST_CASE("StateTree child listener removal stops callbacks", "[state][tree]") {
    auto root = StateTree::create("root");
    auto first = StateTree::create("first");
    root->add_child(first);

    int added_count = 0;
    int removed_count = 0;
    int added_id = root->add_child_added_listener(
        [&](StateTree&, StateTree&, int) { added_count++; });
    int removed_id = root->add_child_removed_listener(
        [&](StateTree&, StateTree&, int) { removed_count++; });

    root->add_child(StateTree::create("second"));
    root->remove_child(first.get());
    REQUIRE(added_count == 1);
    REQUIRE(removed_count == 1);

    root->remove_child_added_listener(added_id);
    root->remove_child_removed_listener(removed_id);
    root->add_child(StateTree::create("third"));
    root->remove_child(0);

    REQUIRE(added_count == 1);
    REQUIRE(removed_count == 1);
}

TEST_CASE("StateTree removing unknown listener ids is stable",
          "[state][tree][coverage]") {
    auto root = StateTree::create("root");
    int property_count = 0;
    int added_count = 0;
    int removed_count = 0;

    root->remove_listener(1234);
    root->remove_child_added_listener(1234);
    root->remove_child_removed_listener(1234);

    auto property_id = root->add_listener(
        [&](StateTree&, std::string_view, const PropertyValue&, const PropertyValue&) {
            ++property_count;
        });
    auto added_id = root->add_child_added_listener(
        [&](StateTree&, StateTree&, int) { ++added_count; });
    auto removed_id = root->add_child_removed_listener(
        [&](StateTree&, StateTree&, int) { ++removed_count; });

    root->remove_listener(property_id + 100);
    root->remove_child_added_listener(added_id + 100);
    root->remove_child_removed_listener(removed_id + 100);

    root->set("name", std::string("kept"));
    root->add_child(StateTree::create("child"));
    root->remove_child(0);

    REQUIRE(property_count == 1);
    REQUIRE(added_count == 1);
    REQUIRE(removed_count == 1);
}

TEST_CASE("StateTree remove listener", "[state][tree]") {
    auto tree = StateTree::create("test");

    int count = 0;
    int id = tree->add_listener([&](StateTree&, std::string_view, const PropertyValue&, const PropertyValue&) {
        count++;
    });

    tree->set("a", int64_t(1));
    REQUIRE(count == 1);

    tree->remove_listener(id);
    tree->set("b", int64_t(2));
    REQUIRE(count == 1);  // Listener removed, count unchanged
}

TEST_CASE("StateTree remove notifies listeners with cleared value",
          "[state][tree][codecov]") {
    auto tree = StateTree::create("test");
    tree->set("gain", 0.75);

    int count = 0;
    PropertyValue old_value;
    PropertyValue new_value;
    tree->add_listener([&](StateTree&, std::string_view prop,
                           const PropertyValue& old_val,
                           const PropertyValue& new_val) {
        REQUIRE(prop == "gain");
        old_value = old_val;
        new_value = new_val;
        ++count;
    });

    tree->remove("gain");
    tree->remove("gain");

    REQUIRE(count == 1);
    REQUIRE_THAT(std::get<double>(old_value), WithinAbs(0.75, 1e-5));
    REQUIRE(std::holds_alternative<std::monostate>(new_value));
}

TEST_CASE("StateTree skips empty listeners",
          "[state][tree][codecov]") {
    auto root = StateTree::create("root");
    int property_count = 0;
    int added_count = 0;
    int removed_count = 0;

    root->add_listener({});
    root->add_listener([&](StateTree&, std::string_view prop,
                           const PropertyValue&, const PropertyValue&) {
        REQUIRE(prop == "gain");
        ++property_count;
    });

    root->add_child_added_listener({});
    root->add_child_added_listener(
        [&](StateTree&, StateTree& child, int index) {
            REQUIRE(child.type_name() == "child");
            REQUIRE(index == 0);
            ++added_count;
        });

    root->add_child_removed_listener({});
    root->add_child_removed_listener(
        [&](StateTree&, StateTree& child, int index) {
            REQUIRE(child.type_name() == "child");
            REQUIRE(index == 0);
            ++removed_count;
        });

    root->set("gain", 0.5);
    root->add_child(StateTree::create("child"));
    root->remove_child(0);

    REQUIRE(property_count == 1);
    REQUIRE(added_count == 1);
    REQUIRE(removed_count == 1);
}

// ── JSON serialization ──────────────────────────────────────────────────

TEST_CASE("StateTree to_json produces valid JSON", "[state][tree][json]") {
    auto root = StateTree::create("preset");
    root->set("name", std::string("Default"));
    root->set("version", int64_t(1));

    auto osc = StateTree::create("oscillator");
    osc->set("waveform", std::string("saw"));
    osc->set("frequency", 440.0);
    root->add_child(osc);

    std::string json = root->to_json();
    REQUIRE(json.find("preset") != std::string::npos);
    REQUIRE(json.find("Default") != std::string::npos);
    REQUIRE(json.find("oscillator") != std::string::npos);
    REQUIRE(json.find("saw") != std::string::npos);
}

TEST_CASE("StateTree JSON round-trip", "[state][tree][json]") {
    // Build a tree
    auto root = StateTree::create("synth");
    root->set("name", std::string("MySynth"));
    root->set("polyphony", int64_t(8));
    root->set("master_volume", 0.85);
    root->set("active", true);

    auto osc1 = StateTree::create("oscillator");
    osc1->set("type", std::string("sine"));
    osc1->set("detune", 0.0);
    root->add_child(osc1);

    auto osc2 = StateTree::create("oscillator");
    osc2->set("type", std::string("square"));
    osc2->set("detune", 7.0);
    root->add_child(osc2);

    auto filter = StateTree::create("filter");
    filter->set("cutoff", 1200.5);
    filter->set("resonance", 0.5);
    root->add_child(filter);

    // Serialize
    std::string json = root->to_json();
    REQUIRE_FALSE(json.empty());

    // Deserialize
    auto restored = StateTree::from_json(json);
    REQUIRE(restored != nullptr);
    REQUIRE(restored->type_name() == "synth");
    REQUIRE(restored->get_string("name") == "MySynth");
    REQUIRE(restored->get_int("polyphony") == 8);
    REQUIRE(restored->child_count() == 3);

    // Check children
    auto osc_restored = restored->child(0);
    REQUIRE(osc_restored->type_name() == "oscillator");
    REQUIRE(osc_restored->get_string("type") == "sine");

    auto filter_restored = restored->child(2);
    REQUIRE(filter_restored->type_name() == "filter");
    REQUIRE_THAT(filter_restored->get_double("cutoff"), WithinAbs(1200.5, 0.1));
}

TEST_CASE("StateTree JSON round-trip preserves escaped strings",
          "[state][tree][json][coverage][phase3]") {
    auto root = StateTree::create(R"(preset "A")");
    root->set(R"(display\name)", std::string("Line 1\nLine \"2\" \\ tail"));

    auto child = StateTree::create(R"(child\type)");
    child->set("label", std::string(R"(quoted "child")"));
    root->add_child(child);

    const auto json = root->to_json();
    REQUIRE(json.find("\\\"") != std::string::npos);
    REQUIRE(json.find("\\\\") != std::string::npos);
    REQUIRE(json.find("\\n") != std::string::npos);

    auto restored = StateTree::from_json(json);
    REQUIRE(restored != nullptr);
    REQUIRE(restored->type_name() == R"(preset "A")");
    REQUIRE(restored->get_string(R"(display\name)") == "Line 1\nLine \"2\" \\ tail");
    REQUIRE(restored->child_count() == 1);
    REQUIRE(restored->child(0)->type_name() == R"(child\type)");
    REQUIRE(restored->child(0)->get_string("label") == R"(quoted "child")");
}

TEST_CASE("StateTree from_json with invalid JSON returns nullptr", "[state][tree][json]") {
    auto result = StateTree::from_json("not json at all {{{");
    REQUIRE(result == nullptr);
}

TEST_CASE("StateTree from_json with empty object", "[state][tree][json]") {
    auto result = StateTree::from_json("{}");
    REQUIRE(result != nullptr);
    REQUIRE(result->type_name() == "node");  // Default type
}

TEST_CASE("StateTree from_json skips unsupported properties and child values",
          "[state][tree][json][issue-641]") {
    auto result = StateTree::from_json(R"JSON({
        "type": 123,
        "properties": {
            "name": "Patch",
            "enabled": false,
            "voice_count": 8,
            "gain": 0.25,
            "ignored_null": null,
            "ignored_array": [1, 2],
            "ignored_object": {"nested": true}
        },
        "children": [
            {"type": "osc", "properties": {"wave": "sine"}},
            null,
            42,
            {"properties": {"default_type": true}}
        ]
    })JSON");

    REQUIRE(result != nullptr);
    REQUIRE(result->type_name() == "node");
    REQUIRE(result->get_string("name") == "Patch");
    REQUIRE_FALSE(result->get_bool("enabled", true));
    REQUIRE(result->get_int("voice_count") == 8);
    REQUIRE_THAT(result->get_double("gain"), WithinAbs(0.25, 1e-9));
    REQUIRE_FALSE(result->has("ignored_null"));
    REQUIRE_FALSE(result->has("ignored_array"));
    REQUIRE_FALSE(result->has("ignored_object"));
    REQUIRE(result->child_count() == 2);
    REQUIRE(result->child(0)->type_name() == "osc");
    REQUIRE(result->child(1)->type_name() == "node");
    REQUIRE(result->child(1)->get_bool("default_type"));
}

TEST_CASE("StateTree from_json ignores malformed members and children",
          "[state][tree][json][codecov]") {
    auto non_object = StateTree::from_json("[]");
    REQUIRE(non_object == nullptr);

    auto result = StateTree::from_json(R"({
        "type": 123,
        "properties": {
            "ok_bool": true,
            "ok_int32": 7,
            "skip_array": [1, 2, 3],
            "skip_null": null
        },
        "children": [
            {"type": "kept", "properties": {"name": "child"}},
            42,
            {"type": false}
        ]
    })");

    REQUIRE(result != nullptr);
    REQUIRE(result->type_name() == "node");
    REQUIRE(result->get_bool("ok_bool"));
    REQUIRE(result->get_int("ok_int32") == 7);
    REQUIRE_FALSE(result->has("skip_array"));
    REQUIRE_FALSE(result->has("skip_null"));
    REQUIRE(result->child_count() == 2);
    REQUIRE(result->child(0)->type_name() == "kept");
    REQUIRE(result->child(0)->get_string("name") == "child");
    REQUIRE(result->child(1)->type_name() == "node");
}

TEST_CASE("StateTree JSON keeps integer values available as doubles",
          "[state][tree][json][coverage]") {
    auto result = StateTree::from_json(R"({
        "type": "numbers",
        "properties": {
            "whole": 12,
            "fractional": 12.5
        }
    })");

    REQUIRE(result != nullptr);
    REQUIRE(result->type_name() == "numbers");
    REQUIRE(result->get_int("whole") == 12);
    REQUIRE_THAT(result->get_double("whole"), WithinAbs(12.0, 1e-9));
    REQUIRE(result->get_int("fractional", 99) == 99);
    REQUIRE_THAT(result->get_double("fractional"), WithinAbs(12.5, 1e-9));
}

// ── Deep copy ───────────────────────────────────────────────────────────

TEST_CASE("StateTree deep copy", "[state][tree]") {
    auto root = StateTree::create("root");
    root->set("name", std::string("original"));
    root->add_child(StateTree::create("child"));

    auto copy = root->deep_copy();
    REQUIRE(copy->type_name() == "root");
    REQUIRE(copy->get_string("name") == "original");
    REQUIRE(copy->child_count() == 1);

    // Modifying copy doesn't affect original
    copy->set("name", std::string("modified"));
    REQUIRE(root->get_string("name") == "original");
}

TEST_CASE("StateTree deep copy rewires child parent pointers",
          "[state][tree][issue-641]") {
    auto root = StateTree::create("root");
    auto child = StateTree::create("child");
    root->add_child(child);

    auto copy = root->deep_copy();
    REQUIRE(copy->child_count() == 1);
    REQUIRE(copy->child(0).get() != child.get());
    REQUIRE(copy->child(0)->parent() == copy.get());
    REQUIRE(child->parent() == root.get());
}

TEST_CASE("StateTree deep copy does not copy listeners",
          "[state][tree][coverage]") {
    auto root = StateTree::create("root");
    auto child = StateTree::create("child");
    root->add_child(child);

    int property_count = 0;
    int added_count = 0;
    int removed_count = 0;
    root->add_listener([&](StateTree&, std::string_view, const PropertyValue&, const PropertyValue&) {
        ++property_count;
    });
    root->add_child_added_listener([&](StateTree&, StateTree&, int) {
        ++added_count;
    });
    root->add_child_removed_listener([&](StateTree&, StateTree&, int) {
        ++removed_count;
    });

    auto copy = root->deep_copy();
    copy->set("name", std::string("copy"));
    copy->add_child(StateTree::create("new_child"));
    copy->remove_child(0);

    REQUIRE(property_count == 0);
    REQUIRE(added_count == 0);
    REQUIRE(removed_count == 0);
    REQUIRE(root->child_count() == 1);
    REQUIRE(copy->child_count() == 1);
}

// ── ObservableValue ─────────────────────────────────────────────────────

TEST_CASE("ObservableValue get and set", "[state][observable]") {
    ObservableValue<int> val(42);
    REQUIRE(val.get() == 42);

    val.set(100);
    REQUIRE(val.get() == 100);
}

TEST_CASE("ObservableValue listener fires on change", "[state][observable]") {
    ObservableValue<std::string> val("hello");

    std::string old_val, new_val;
    val.add_listener([&](const std::string& o, const std::string& n) {
        old_val = o;
        new_val = n;
    });

    val.set("world");
    REQUIRE(old_val == "hello");
    REQUIRE(new_val == "world");
}

TEST_CASE("ObservableValue no notification on same value", "[state][observable]") {
    ObservableValue<int> val(5);
    int change_count = 0;
    val.add_listener([&](const int&, const int&) { change_count++; });

    val.set(5);  // Same value
    REQUIRE(change_count == 0);

    val.set(10);  // Different value
    REQUIRE(change_count == 1);
}

TEST_CASE("ObservableValue remove listener", "[state][observable]") {
    ObservableValue<int> val(0);
    int count = 0;
    int id = val.add_listener([&](const int&, const int&) { count++; });

    val.set(1);
    REQUIRE(count == 1);

    val.remove_listener(id);
    val.set(2);
    REQUIRE(count == 1);  // Listener removed
}

TEST_CASE("ObservableValue assignment operator emits one change",
          "[state][observable][issue-641]") {
    ObservableValue<int> val(1);
    int old_value = 0;
    int new_value = 0;
    int count = 0;
    val.add_listener([&](const int& old_val, const int& next_val) {
        old_value = old_val;
        new_value = next_val;
        count++;
    });

    val = 5;
    val = 5;

    REQUIRE(val.get() == 5);
    REQUIRE(old_value == 1);
    REQUIRE(new_value == 5);
    REQUIRE(count == 1);
}

TEST_CASE("ObservableValue skips empty listeners", "[state][observable][codecov]") {
    ObservableValue<int> val(1);
    int count = 0;

    val.add_listener({});
    val.add_listener([&](const int& old_value, const int& new_value) {
        REQUIRE(old_value == 1);
        REQUIRE(new_value == 2);
        ++count;
    });

    val.set(2);
    REQUIRE(count == 1);
}

TEST_CASE("ObservableValue removing an unknown listener is a no-op",
          "[state][observable][coverage][phase3]") {
    ObservableValue<int> val(1);
    int count = 0;
    val.add_listener([&](const int&, const int&) { ++count; });

    val.remove_listener(999);
    val.set(2);

    REQUIRE(count == 1);
}

// ── CachedProperty ──────────────────────────────────────────────────────

TEST_CASE("CachedProperty binds default and follows tree updates", "[state][cached]") {
    auto tree = StateTree::create("params");
    CachedProperty<std::string> name(tree, "name", "default");

    REQUIRE(name.is_bound());
    REQUIRE(name.get() == "default");

    tree->set("name", std::string("Gain"));
    REQUIRE(name.get() == "Gain");

    name.set("Drive");
    REQUIRE(tree->get_string("name") == "Drive");
    REQUIRE(name.get() == "Drive");
}

TEST_CASE("CachedProperty refresh and numeric coercion", "[state][cached]") {
    auto tree = StateTree::create("params");
    tree->set("mix", int64_t(7));

    CachedProperty<double> mix(tree, "mix", 0.5);
    REQUIRE_THAT(mix.get(), WithinAbs(7.0, 1e-5));

    tree->set("mix", 2.5);
    REQUIRE_THAT(mix.get(), WithinAbs(2.5, 1e-5));

    tree->remove("mix");
    mix.refresh();
    REQUIRE_THAT(mix.get(), WithinAbs(2.5, 1e-5));
}

TEST_CASE("CachedProperty move transfers listener ownership", "[state][cached]") {
    auto tree = StateTree::create("params");
    tree->set("enabled", true);

    CachedProperty<bool> enabled(tree, "enabled", false);
    CachedProperty<bool> moved(std::move(enabled));

    REQUIRE(moved.is_bound());
    REQUIRE(moved.get());

    tree->set("enabled", false);
    REQUIRE_FALSE(moved.get());
}

TEST_CASE("CachedProperty unbound set only updates local cache", "[state][cached]") {
    CachedProperty<int64_t> size;

    REQUIRE_FALSE(size.is_bound());
    REQUIRE(size.get() == 0);

    size.set(512);
    REQUIRE(size.get() == 512);
}

TEST_CASE("CachedProperty move of unbound property preserves cached value",
          "[state][cached][coverage][phase3]") {
    CachedProperty<int64_t> size;
    size.set(256);

    CachedProperty<int64_t> moved(std::move(size));

    REQUIRE_FALSE(moved.is_bound());
    REQUIRE(moved.get() == 256);
}

TEST_CASE("CachedProperty ignores mismatched external updates",
          "[state][cached][issue-641]") {
    auto tree = StateTree::create("params");
    tree->set("name", std::string("Initial"));
    CachedProperty<std::string> name(tree, "name", "Default");

    tree->set("name", int64_t(123));
    REQUIRE(name.get() == "Initial");

    name.set("Restored");
    REQUIRE(tree->get_string("name") == "Restored");
}

TEST_CASE("CachedProperty destruction removes its StateTree listener",
          "[state][cached][coverage][phase3]") {
    auto tree = StateTree::create("params");
    tree->set("gain", 0.25);

    {
        CachedProperty<double> gain(tree, "gain", 0.0);
        REQUIRE_THAT(gain.get(), WithinAbs(0.25, 1e-5));
        tree->set("gain", 0.5);
        REQUIRE_THAT(gain.get(), WithinAbs(0.5, 1e-5));
    }

    int count = 0;
    tree->add_listener([&](StateTree&, std::string_view prop,
                           const PropertyValue&, const PropertyValue&) {
        REQUIRE(prop == "gain");
        ++count;
    });

    tree->set("gain", 0.75);
    REQUIRE(count == 1);
}

TEST_CASE("CachedProperty bool ignores mismatched refresh values",
          "[state][cached][coverage][phase3-large]") {
    auto tree = StateTree::create("params");
    tree->set("enabled", true);
    CachedProperty<bool> enabled(tree, "enabled", false);

    REQUIRE(enabled.get());

    tree->set("enabled", std::string("false"));
    enabled.refresh();

    REQUIRE(enabled.get());
    enabled = false;
    REQUIRE_FALSE(enabled.get());
    REQUIRE_FALSE(tree->get_bool("enabled", true));
}

TEST_CASE("CachedProperty int64 move tracks later tree updates",
          "[state][cached][coverage][phase3-large]") {
    auto tree = StateTree::create("params");
    tree->set("voices", int64_t(8));
    CachedProperty<int64_t> voices(tree, "voices", 1);
    CachedProperty<int64_t> moved(std::move(voices));

    tree->set("voices", int64_t(16));

    REQUIRE(moved.is_bound());
    REQUIRE(moved.get() == 16);
}

TEST_CASE("CachedProperty int refresh preserves cache on incompatible values",
          "[state][cached][coverage][phase3]") {
    auto tree = StateTree::create("params");
    CachedProperty<int64_t> count(tree, "count", 4);

    REQUIRE(count.get() == 4);

    tree->set("count", std::string("7"));
    count.refresh();
    REQUIRE(count.get() == 4);

    tree->set("count", int64_t(7));
    REQUIRE(count.get() == 7);
}

// ── StateTreeSynchroniser ───────────────────────────────────────────────

TEST_CASE("StateTreeSynchroniser records property and child deltas", "[state][sync]") {
    auto tree = StateTree::create("root");
    StateTreeSynchroniser sync;
    sync.attach(tree);

    tree->set("name", std::string("Lead"));
    tree->add_child(StateTree::create("osc"));
    tree->remove_child(0);

    auto deltas = sync.take_deltas();
    REQUIRE(deltas.size() == 3);

    REQUIRE(deltas[0].type == SyncDeltaType::PropertySet);
    REQUIRE(deltas[0].path == "root");
    REQUIRE(deltas[0].key == "name");
    REQUIRE(std::get<std::string>(deltas[0].value) == "Lead");

    REQUIRE(deltas[1].type == SyncDeltaType::ChildAdd);
    REQUIRE(deltas[1].path == "root");
    REQUIRE(deltas[1].key == "osc");
    REQUIRE(deltas[1].child_index == 0);

    REQUIRE(deltas[2].type == SyncDeltaType::ChildRemove);
    REQUIRE(deltas[2].path == "root");
    REQUIRE(deltas[2].child_index == 0);

    REQUIRE(sync.take_deltas().empty());
}

TEST_CASE("StateTreeSynchroniser attach replaces the previous tree",
          "[state][sync][issue-641]") {
    auto first = StateTree::create("first");
    auto second = StateTree::create("second");
    StateTreeSynchroniser sync;

    sync.attach(first);
    first->set("name", std::string("before"));
    sync.attach(second);
    first->set("name", std::string("ignored"));
    second->set("name", std::string("recorded"));

    auto deltas = sync.take_deltas();
    REQUIRE(deltas.size() == 1);
    REQUIRE(deltas[0].path == "second");
    REQUIRE(deltas[0].key == "name");
    REQUIRE(std::get<std::string>(deltas[0].value) == "recorded");
}

TEST_CASE("StateTreeSynchroniser records property removals", "[state][sync][codecov]") {
    auto tree = StateTree::create("root");
    tree->set("name", std::string("Lead"));

    StateTreeSynchroniser sync;
    sync.attach(tree);

    tree->remove("name");
    tree->remove("missing");

    auto deltas = sync.take_deltas();
    REQUIRE(deltas.size() == 1);
    REQUIRE(deltas[0].type == SyncDeltaType::PropertyRemove);
    REQUIRE(deltas[0].path == "root");
    REQUIRE(deltas[0].key == "name");
    REQUIRE(std::holds_alternative<std::monostate>(deltas[0].value));
}

TEST_CASE("StateTreeSynchroniser preserves explicit null property sets",
          "[state][sync][codecov]") {
    auto tree = StateTree::create("root");

    StateTreeSynchroniser sync;
    sync.attach(tree);

    tree->set("nullable", PropertyValue{});

    auto deltas = sync.take_deltas();
    REQUIRE(deltas.size() == 1);
    REQUIRE(deltas[0].type == SyncDeltaType::PropertySet);
    REQUIRE(deltas[0].key == "nullable");
    REQUIRE(std::holds_alternative<std::monostate>(deltas[0].value));

    auto mirror = StateTree::create("root");
    StateTreeSynchroniser::apply(*mirror, deltas);
    REQUIRE(mirror->has("nullable"));
    REQUIRE(std::holds_alternative<std::monostate>(mirror->get("nullable")));
}

TEST_CASE("StateTreeSynchroniser detach clears pending and stops recording", "[state][sync]") {
    auto tree = StateTree::create("root");
    StateTreeSynchroniser sync;
    sync.attach(tree);

    tree->set("name", std::string("before"));
    tree->add_child(StateTree::create("before_child"));
    sync.detach();
    REQUIRE(sync.take_deltas().empty());

    tree->set("name", std::string("after"));
    tree->add_child(StateTree::create("after_child"));
    tree->remove_child(0);
    REQUIRE(sync.take_deltas().empty());
}

TEST_CASE("StateTreeSynchroniser attach nullptr detaches existing listeners",
          "[state][sync][codecov]") {
    auto tree = StateTree::create("root");
    StateTreeSynchroniser sync;
    sync.attach(tree);

    tree->set("before", std::string("value"));
    REQUIRE(sync.take_deltas().size() == 1);

    sync.attach(nullptr);
    tree->set("after", std::string("ignored"));
    tree->add_child(StateTree::create("ignored_child"));

    REQUIRE(sync.take_deltas().empty());
}

TEST_CASE("StateTreeSynchroniser encode and decode round-trip", "[state][sync]") {
    std::vector<SyncDelta> deltas = {
        {SyncDeltaType::PropertySet, "root", "name", std::string("Lead"), 3},
        {SyncDeltaType::PropertySet, "root", "count", int64_t(42), 7},
        {SyncDeltaType::PropertySet, "root", "mix", 0.25, 1},
        {SyncDeltaType::PropertySet, "root", "enabled", true, 2},
        {SyncDeltaType::PropertyRemove, "root", "unused", {}, -1},
        {SyncDeltaType::ChildAdd, "root", "osc", {}, 1},
        {SyncDeltaType::ChildRemove, "root", "", {}, 0},
    };

    auto encoded = StateTreeSynchroniser::encode(deltas);
    auto decoded = StateTreeSynchroniser::decode(encoded.data(), encoded.size());

    REQUIRE(decoded.size() == deltas.size());
    REQUIRE(decoded[0].type == SyncDeltaType::PropertySet);
    REQUIRE(decoded[0].path == "root");
    REQUIRE(decoded[0].key == "name");
    REQUIRE(std::get<std::string>(decoded[0].value) == "Lead");

    REQUIRE(std::get<int64_t>(decoded[1].value) == 42);
    REQUIRE_THAT(std::get<double>(decoded[2].value), WithinAbs(0.25, 1e-8));
    REQUIRE(std::get<bool>(decoded[3].value));
    REQUIRE(decoded[4].type == SyncDeltaType::PropertyRemove);
    REQUIRE(decoded[4].value.index() == 0);
    REQUIRE(decoded[5].type == SyncDeltaType::ChildAdd);
    REQUIRE(decoded[5].child_index == 1);
    REQUIRE(decoded[6].type == SyncDeltaType::ChildRemove);
    REQUIRE(decoded[6].child_index == 0);
}

TEST_CASE("StateTreeSynchroniser decode rejects undersized buffers", "[state][sync]") {
    std::vector<uint8_t> encoded = {1, 0, static_cast<uint8_t>(SyncDeltaType::PropertySet)};
    auto decoded = StateTreeSynchroniser::decode(encoded.data(), encoded.size());
    REQUIRE(decoded.empty());

    decoded = StateTreeSynchroniser::decode(nullptr, 0);
    REQUIRE(decoded.empty());
}

TEST_CASE("StateTreeSynchroniser encodes and decodes empty delta batches",
          "[state][sync][issue-641]") {
    auto encoded = StateTreeSynchroniser::encode({});
    REQUIRE(encoded.size() == 2);

    auto decoded = StateTreeSynchroniser::decode(encoded.data(), encoded.size());
    REQUIRE(decoded.empty());
}

TEST_CASE("StateTreeSynchroniser decode keeps complete prefix on truncated batches",
          "[state][sync][issue-641]") {
    std::vector<SyncDelta> deltas = {
        {SyncDeltaType::PropertySet, "root", "name", std::string("Lead"), 0},
        {SyncDeltaType::PropertySet, "root", "count", int64_t(3), 0},
    };
    auto encoded = StateTreeSynchroniser::encode(deltas);
    auto first_only = StateTreeSynchroniser::encode({deltas[0]});
    encoded.resize(first_only.size() + 1);

    auto decoded = StateTreeSynchroniser::decode(encoded.data(), encoded.size());
    REQUIRE(decoded.size() == 1);
    REQUIRE(decoded[0].type == SyncDeltaType::PropertySet);
    REQUIRE(decoded[0].key == "name");
    REQUIRE(std::get<std::string>(decoded[0].value) == "Lead");
}

TEST_CASE("StateTreeSynchroniser preserves negative child indexes in encoding",
          "[state][sync][coverage][phase3]") {
    std::vector<SyncDelta> deltas = {
        {SyncDeltaType::ChildAdd, "root", "child", {}, -1},
        {SyncDeltaType::ChildRemove, "root", "", {}, -1},
    };

    auto encoded = StateTreeSynchroniser::encode(deltas);
    auto decoded = StateTreeSynchroniser::decode(encoded.data(), encoded.size());

    REQUIRE(decoded.size() == 2);
    REQUIRE(decoded[0].child_index == -1);
    REQUIRE(decoded[1].child_index == -1);
}

TEST_CASE("StateTreeSynchroniser decode treats unknown value types as null",
          "[state][sync][coverage][phase3]") {
    SyncDelta delta{SyncDeltaType::PropertySet, "root", "mystery", {}, -1};
    auto encoded = StateTreeSynchroniser::encode({delta});
    REQUIRE_FALSE(encoded.empty());
    encoded.back() = 99;

    auto decoded = StateTreeSynchroniser::decode(encoded.data(), encoded.size());
    REQUIRE(decoded.size() == 1);
    REQUIRE(decoded[0].type == SyncDeltaType::PropertySet);
    REQUIRE(decoded[0].key == "mystery");
    REQUIRE(std::holds_alternative<std::monostate>(decoded[0].value));
}

TEST_CASE("StateTreeSynchroniser decode rejects truncated typed values",
          "[state][sync][codecov]") {
    const std::vector<SyncDelta> cases = {
        {SyncDeltaType::PropertySet, "root", "name", std::string("Lead"), -1},
        {SyncDeltaType::PropertySet, "root", "count", int64_t(42), -1},
        {SyncDeltaType::PropertySet, "root", "mix", 0.25, -1},
        {SyncDeltaType::PropertySet, "root", "enabled", true, -1},
    };

    for (const auto& delta : cases) {
        auto encoded = StateTreeSynchroniser::encode({delta});
        REQUIRE_FALSE(encoded.empty());
        encoded.pop_back();

        auto decoded = StateTreeSynchroniser::decode(encoded.data(), encoded.size());
        REQUIRE(decoded.empty());
    }
}

TEST_CASE("StateTreeSynchroniser decode preserves negative child indexes",
          "[state][sync][coverage][phase3]") {
    std::vector<SyncDelta> deltas = {
        {SyncDeltaType::ChildRemove, "root", "", {}, -1},
        {SyncDeltaType::ChildAdd, "root", "tail", {}, -2},
    };

    auto encoded = StateTreeSynchroniser::encode(deltas);
    auto decoded = StateTreeSynchroniser::decode(encoded.data(), encoded.size());

    REQUIRE(decoded.size() == 2);
    REQUIRE(decoded[0].type == SyncDeltaType::ChildRemove);
    REQUIRE(decoded[0].child_index == -1);
    REQUIRE(decoded[1].type == SyncDeltaType::ChildAdd);
    REQUIRE(decoded[1].child_index == -2);
}

TEST_CASE("StateTreeSynchroniser apply mutates properties and children", "[state][sync]") {
    auto tree = StateTree::create("root");
    tree->set("remove_me", std::string("bye"));
    tree->add_child(StateTree::create("existing"));

    std::vector<SyncDelta> deltas = {
        {SyncDeltaType::PropertySet, "root", "name", std::string("Lead"), -1},
        {SyncDeltaType::PropertySet, "root", "count", int64_t(5), -1},
        {SyncDeltaType::PropertyRemove, "root", "remove_me", {}, -1},
        {SyncDeltaType::ChildAdd, "root", "inserted", {}, 0},
        {SyncDeltaType::ChildAdd, "root", "appended", {}, 99},
        {SyncDeltaType::ChildRemove, "root", "", {}, 1},
        {SyncDeltaType::ChildRemove, "root", "", {}, 99},
    };

    StateTreeSynchroniser::apply(*tree, deltas);

    REQUIRE(tree->get_string("name") == "Lead");
    REQUIRE(tree->get_int("count") == 5);
    REQUIRE_FALSE(tree->has("remove_me"));

    REQUIRE(tree->child_count() == 2);
    REQUIRE(tree->child(0)->type_name() == "inserted");
    REQUIRE(tree->child(1)->type_name() == "appended");
}
