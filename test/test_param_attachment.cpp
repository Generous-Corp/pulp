#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/param_attachment.hpp>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

static void setup_store(StateStore& store) {
    store.add_parameter({.id = 1, .name = "Cutoff", .unit = "Hz",
        .range = {20, 20000, 1000, 0.01f}});
    store.add_parameter({.id = 2, .name = "Resonance", .unit = "",
        .range = {0, 1, 0.5f, 0.01f}});
    store.add_parameter({.id = 3, .name = "Type", .unit = "",
        .range = {0, 3, 0, 1}});
    store.add_parameter({.id = 4, .name = "Bypass", .unit = "",
        .range = {0, 1, 0, 1}});
}

TEST_CASE("attach_knob creates knob with correct label", "[view][attachment]") {
    StateStore store;
    setup_store(store);
    auto [knob, binding] = attach_knob(store, 1, 48.0f);
    REQUIRE(knob != nullptr);
    REQUIRE(knob->id() == "Cutoff");
}

TEST_CASE("attach_knob initial value matches param", "[view][attachment]") {
    StateStore store;
    setup_store(store);
    store.set_value(1, 5000.0f);
    auto [knob, binding] = attach_knob(store, 1);
    float expected_norm = store.get_normalized(1);
    REQUIRE_THAT(knob->value(), WithinAbs(expected_norm, 0.01));
}

TEST_CASE("attach_knob size is set", "[view][attachment]") {
    StateStore store;
    setup_store(store);
    auto [knob, binding] = attach_knob(store, 1, 80.0f);
    REQUIRE_THAT(knob->flex().preferred_width, WithinAbs(80.0, 0.1));
    REQUIRE_THAT(knob->flex().preferred_height, WithinAbs(80.0, 0.1));
}

TEST_CASE("attach_fader creates fader with label", "[view][attachment]") {
    StateStore store;
    setup_store(store);
    auto [fader, binding] = attach_fader(store, 2);
    REQUIRE(fader != nullptr);
    REQUIRE(fader->id() == "Resonance");
}

TEST_CASE("attach_fader initial value matches param", "[view][attachment]") {
    StateStore store;
    setup_store(store);
    store.set_value(2, 0.75f);
    auto [fader, binding] = attach_fader(store, 2);
    float expected_norm = store.get_normalized(2);
    REQUIRE_THAT(fader->value(), WithinAbs(expected_norm, 0.01));
}

TEST_CASE("attach_toggle creates toggle with label", "[view][attachment]") {
    StateStore store;
    setup_store(store);
    auto [toggle, binding] = attach_toggle(store, 4);
    REQUIRE(toggle != nullptr);
    REQUIRE(toggle->id() == "Bypass");
}

TEST_CASE("attach_toggle reflects boolean state", "[view][attachment]") {
    StateStore store;
    setup_store(store);
    store.set_value(4, 1.0f);
    auto [toggle, binding] = attach_toggle(store, 4);
    REQUIRE(toggle->is_on());

    StateStore store2;
    setup_store(store2);
    store2.set_value(4, 0.0f);
    auto [toggle2, binding2] = attach_toggle(store2, 4);
    REQUIRE_FALSE(toggle2->is_on());
}

TEST_CASE("attach_combo creates combobox with items", "[view][attachment]") {
    StateStore store;
    setup_store(store);
    store.set_value(3, 2.0f);
    auto [combo, binding] = attach_combo(store, 3, {"LP", "HP", "BP", "Notch"});
    REQUIRE(combo != nullptr);
    REQUIRE(combo->selected() == 2);
    REQUIRE(combo->selected_text() == "BP");
}

TEST_CASE("poll_bindings does not crash", "[view][attachment]") {
    StateStore store;
    setup_store(store);
    auto [k, b1] = attach_knob(store, 1);
    auto [f, b2] = attach_fader(store, 2);
    std::vector<Binding> bindings;
    bindings.push_back(std::move(b1));
    bindings.push_back(std::move(b2));
    poll_bindings(bindings); // should not crash
}

TEST_CASE("attach_knob on_change writes to store", "[view][attachment]") {
    StateStore store;
    setup_store(store);
    auto [knob, binding] = attach_knob(store, 1);
    // Simulate user dragging knob to 0.5 normalized
    if (knob->on_change) knob->on_change(0.5f);
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(0.5, 0.01));
}

TEST_CASE("attach_knob formatter denormalizes values with units",
          "[view][attachment][coverage][phase3]") {
    StateStore store;
    setup_store(store);

    auto [knob, binding] = attach_knob(store, 1, 80.0f);
    knob->set_bounds({0, 0, 80, 80});
    knob->set_value(0.0f);

    pulp::canvas::RecordingCanvas canvas;
    knob->paint(canvas);

    bool saw_min_hz = false;
    for (const auto& command : canvas.commands()) {
        if (command.type == pulp::canvas::DrawCommand::Type::fill_text &&
            command.text == "20.0 Hz") {
            saw_min_hz = true;
        }
    }
    REQUIRE(saw_min_hz);
}

TEST_CASE("param attachments forward fader toggle and combo edits",
          "[view][attachment][issue-493]") {
    StateStore store;
    setup_store(store);

    auto [fader, fader_binding] = attach_fader(store, 2);
    REQUIRE(fader->on_change);
    fader->on_change(0.25f);
    REQUIRE_THAT(store.get_value(2), WithinAbs(0.25, 0.001));

    auto [toggle, toggle_binding] = attach_toggle(store, 4);
    REQUIRE(toggle->on_toggle);
    toggle->on_toggle(true);
    REQUIRE_THAT(store.get_value(4), WithinAbs(1.0, 0.001));
    toggle->on_toggle(false);
    REQUIRE_THAT(store.get_value(4), WithinAbs(0.0, 0.001));

    auto [combo, combo_binding] = attach_combo(store, 3, {"LP", "HP", "BP", "Notch"});
    combo->set_selected(3);
    REQUIRE(combo->selected() == 3);
    REQUIRE(combo->selected_text() == "Notch");
    REQUIRE_THAT(store.get_value(3), WithinAbs(3.0, 0.001));
}

TEST_CASE("param attachments tolerate missing parameter ids",
          "[view][attachment][issue-493]") {
    StateStore store;
    setup_store(store);

    auto [knob, knob_binding] = attach_knob(store, 999, 72.0f);
    REQUIRE(knob != nullptr);
    REQUIRE(knob->id().empty());
    REQUIRE_THAT(knob->value(), WithinAbs(0.0, 0.001));
    REQUIRE(knob->on_change);
    knob->on_change(1.0f);
    REQUIRE_THAT(knob_binding.get(), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(knob->flex().preferred_width, WithinAbs(72.0, 0.001));

    auto [fader, fader_binding] = attach_fader(store, 999);
    REQUIRE(fader != nullptr);
    REQUIRE(fader->id().empty());
    REQUIRE(fader->on_change);
    fader->on_change(0.5f);
    REQUIRE_THAT(fader_binding.get(), WithinAbs(0.0, 0.001));

    auto [toggle, toggle_binding] = attach_toggle(store, 999);
    REQUIRE(toggle != nullptr);
    REQUIRE_FALSE(toggle->is_on());
    REQUIRE(toggle->on_toggle);
    toggle->on_toggle(true);
    REQUIRE_THAT(toggle_binding.get(), WithinAbs(0.0, 0.001));

    auto [combo, combo_binding] = attach_combo(store, 999, {"A", "B"});
    REQUIRE(combo != nullptr);
    combo->set_selected(1);
    REQUIRE(combo->selected() == 1);
    REQUIRE_THAT(combo_binding.get(), WithinAbs(0.0, 0.001));
}

TEST_CASE("poll_bindings forwards external parameter changes",
          "[view][attachment][issue-493]") {
    StateStore store;
    setup_store(store);
    auto [knob, binding] = attach_knob(store, 1);

    int callback_count = 0;
    float last_value = 0.0f;
    binding.on_change([&](float value) {
        ++callback_count;
        last_value = value;
    });

    std::vector<Binding> bindings;
    bindings.push_back(std::move(binding));

    poll_bindings(bindings);
    callback_count = 0;

    store.set_value(1, 5000.0f);
    poll_bindings(bindings);

    REQUIRE(callback_count == 1);
    REQUIRE_THAT(last_value, WithinAbs(5000.0, 0.001));
}
