#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/state/state_tree.hpp>

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

TEST_CASE("StateTree insert child at index", "[state][tree]") {
    auto root = StateTree::create("root");
    root->add_child(StateTree::create("a"));
    root->add_child(StateTree::create("c"));

    root->insert_child(1, StateTree::create("b"));
    REQUIRE(root->child_count() == 3);
    REQUIRE(root->child(1)->type_name() == "b");
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

TEST_CASE("StateTree from_json with invalid JSON returns nullptr", "[state][tree][json]") {
    auto result = StateTree::from_json("not json at all {{{");
    REQUIRE(result == nullptr);
}

TEST_CASE("StateTree from_json with empty object", "[state][tree][json]") {
    auto result = StateTree::from_json("{}");
    REQUIRE(result != nullptr);
    REQUIRE(result->type_name() == "node");  // Default type
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
