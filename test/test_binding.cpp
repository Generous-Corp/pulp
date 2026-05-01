#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/state/binding.hpp>

using namespace pulp::state;
using Catch::Matchers::WithinAbs;

static std::unique_ptr<StateStore> make_store() {
    auto store = std::make_unique<StateStore>();
    store->add_parameter({.id = 1, .name = "Gain", .unit = "dB",
                          .range = {-60.0f, 24.0f, 0.0f, 0.1f}});
    store->add_parameter({.id = 2, .name = "Mix", .unit = "%",
                          .range = {0.0f, 100.0f, 50.0f, 0.1f}});
    return store;
}

TEST_CASE("Binding get/set", "[binding]") {
    auto store = make_store();
    Binding b(*store,1);

    REQUIRE(b.is_bound());
    REQUIRE_THAT(b.get(), WithinAbs(0.0, 0.01)); // default
    b.set(-12.0f);
    REQUIRE_THAT(b.get(), WithinAbs(-12.0, 0.01));
}

TEST_CASE("Binding unbound operations are inert", "[binding]") {
    Binding b;
    int call_count = 0;
    b.on_change([&](float) { ++call_count; });

    REQUIRE_FALSE(b.is_bound());
    REQUIRE(b.id() == 0);
    REQUIRE(b.info() == nullptr);
    REQUIRE(b.edit_history() == nullptr);
    REQUIRE_THAT(b.get(), WithinAbs(0.0, 0.01));
    REQUIRE_THAT(b.get_normalized(), WithinAbs(0.0, 0.01));

    EditHistory history;
    b.set_edit_history(&history);
    REQUIRE(b.edit_history() == &history);

    b.set(10.0f);
    b.set_normalized(0.5f);
    b.begin_gesture();
    b.end_gesture();
    b.reset();

    REQUIRE_FALSE(b.poll());
    REQUIRE(call_count == 0);
    REQUIRE_FALSE(history.can_undo());
}

TEST_CASE("Binding normalized", "[binding]") {
    auto store = make_store();
    Binding b(*store,1); // range -60..24

    b.set_normalized(0.0f);
    REQUIRE_THAT(b.get(), WithinAbs(-60.0, 0.1));
    b.set_normalized(1.0f);
    REQUIRE_THAT(b.get(), WithinAbs(24.0, 0.1));
}

TEST_CASE("Binding set_normalized only notifies when value changes", "[binding]") {
    auto store = make_store();
    Binding b(*store,2); // range 0..100, default 50

    int call_count = 0;
    b.on_change([&](float) { ++call_count; });

    b.set_normalized(0.5f);
    REQUIRE(call_count == 0);

    b.set_normalized(0.75f);
    REQUIRE(call_count == 1);
    REQUIRE_THAT(b.get(), WithinAbs(75.0, 0.01));
}

TEST_CASE("Binding on_change fires", "[binding]") {
    auto store = make_store();
    Binding b(*store,1);

    float received = -999.0f;
    b.on_change([&](float v) { received = v; });

    b.set(6.0f);
    REQUIRE_THAT(received, WithinAbs(6.0, 0.01));
}

TEST_CASE("Binding on_change does not fire for same value", "[binding]") {
    auto store = make_store();
    Binding b(*store,1);

    int call_count = 0;
    b.on_change([&](float) { call_count++; });

    b.set(0.0f); // same as default
    REQUIRE(call_count == 0);

    b.set(5.0f);
    REQUIRE(call_count == 1);
    b.set(5.0f); // same again
    REQUIRE(call_count == 1);
}

TEST_CASE("Binding poll detects external changes", "[binding]") {
    auto store = make_store();
    Binding b(*store,1);

    // External change (simulating host automation)
    store->set_value(1, 12.0f);

    float received = -999.0f;
    b.on_change([&](float v) { received = v; });

    REQUIRE(b.poll());
    REQUIRE_THAT(received, WithinAbs(12.0, 0.01));

    // No change
    REQUIRE_FALSE(b.poll());
}

TEST_CASE("Binding poll reports non-zero default once", "[binding]") {
    auto store = make_store();
    Binding b(*store,2); // default 50, last_polled_ starts at 0

    float received = -1.0f;
    b.on_change([&](float v) { received = v; });

    REQUIRE(b.poll());
    REQUIRE_THAT(received, WithinAbs(50.0, 0.01));
    REQUIRE_FALSE(b.poll());
}

TEST_CASE("Binding reset to default", "[binding]") {
    auto store = make_store();
    Binding b(*store,1);

    b.set(15.0f);
    REQUIRE_THAT(b.get(), WithinAbs(15.0, 0.01));

    b.reset();
    REQUIRE_THAT(b.get(), WithinAbs(0.0, 0.01)); // default is 0.0
}

TEST_CASE("Binding reset always notifies with current default", "[binding]") {
    auto store = make_store();
    Binding b(*store,2);

    int call_count = 0;
    float last_value = -1.0f;
    b.on_change([&](float v) {
        ++call_count;
        last_value = v;
    });

    b.reset();
    REQUIRE(call_count == 1);
    REQUIRE_THAT(last_value, WithinAbs(50.0, 0.01));

    b.set(25.0f);
    b.reset();
    REQUIRE(call_count == 3); // set + reset
    REQUIRE_THAT(last_value, WithinAbs(50.0, 0.01));
}

TEST_CASE("Binding gestures forward callbacks and create undo entries",
          "[binding]") {
    auto store = make_store();
    Binding b(*store,1);
    EditHistory history;

    int begin_count = 0;
    int end_count = 0;
    ParamID begin_id = 0;
    ParamID end_id = 0;
    store->set_gesture_callbacks(
        [&](ParamID id) {
            ++begin_count;
            begin_id = id;
        },
        [&](ParamID id) {
            ++end_count;
            end_id = id;
        });

    b.set_edit_history(&history);
    b.begin_gesture();
    b.set(-12.0f);
    b.end_gesture();

    REQUIRE(begin_count == 1);
    REQUIRE(end_count == 1);
    REQUIRE(begin_id == 1);
    REQUIRE(end_id == 1);
    REQUIRE(history.can_undo());
    REQUIRE(history.undo_count() == 1);
    REQUIRE(history.undo_description() == "Gain");
    REQUIRE_THAT(b.get(), WithinAbs(-12.0, 0.01));

    REQUIRE(history.undo());
    REQUIRE_THAT(b.get(), WithinAbs(0.0, 0.01));
    REQUIRE(history.redo());
    REQUIRE_THAT(b.get(), WithinAbs(-12.0, 0.01));
}

TEST_CASE("Binding gestures skip undo history when value is unchanged",
          "[binding]") {
    auto store = make_store();
    Binding b(*store,1);
    EditHistory history;
    b.set_edit_history(&history);

    b.begin_gesture();
    b.set(0.0f);
    b.end_gesture();

    REQUIRE_FALSE(history.can_undo());
    REQUIRE(history.undo_count() == 0);
}

TEST_CASE("Binding clamps to range", "[binding]") {
    auto store = make_store();
    Binding b(*store,1); // range -60..24

    b.set(100.0f);
    REQUIRE_THAT(b.get(), WithinAbs(24.0, 0.01));
    b.set(-200.0f);
    REQUIRE_THAT(b.get(), WithinAbs(-60.0, 0.01));
}

TEST_CASE("create_bindings for all params", "[binding]") {
    auto store = make_store();
    auto bindings = create_bindings(*store);

    REQUIRE(bindings.size() == 2);
    REQUIRE(bindings[0].id() == 1);
    REQUIRE(bindings[1].id() == 2);
    REQUIRE(bindings[0].info()->name == "Gain");
    REQUIRE(bindings[1].info()->name == "Mix");
}
