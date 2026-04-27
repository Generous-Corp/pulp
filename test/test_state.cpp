#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/state/state.hpp>
#include <vector>

using namespace pulp::state;
using Catch::Matchers::WithinAbs;

static ParamInfo make_param_info(ParamID id, const char* name, const char* unit, ParamRange range) {
    ParamInfo info;
    info.id = id;
    info.name = name;
    info.unit = unit;
    info.range = range;
    return info;
}

TEST_CASE("ParamRange normalization", "[state][range]") {
    ParamRange range{0.0f, 100.0f, 50.0f, 0.0f};

    REQUIRE_THAT(range.normalize(0.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(range.normalize(50.0f), WithinAbs(0.5, 0.001));
    REQUIRE_THAT(range.normalize(100.0f), WithinAbs(1.0, 0.001));

    REQUIRE_THAT(range.denormalize(0.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(range.denormalize(0.5f), WithinAbs(50.0, 0.001));
    REQUIRE_THAT(range.denormalize(1.0f), WithinAbs(100.0, 0.001));
}

TEST_CASE("ParamRange with step", "[state][range]") {
    ParamRange range{0.0f, 10.0f, 5.0f, 1.0f}; // step = 1

    REQUIRE_THAT(range.denormalize(0.33f), WithinAbs(3.0, 0.5));
    REQUIRE_THAT(range.denormalize(0.77f), WithinAbs(8.0, 0.5));
}

TEST_CASE("StateStore basic operations", "[state][store]") {
    StateStore store;

    ParamInfo gain_info;
    gain_info.id = 1;
    gain_info.name = "Gain";
    gain_info.unit = "dB";
    gain_info.range = {-60.0f, 12.0f, 0.0f, 0.0f};

    ParamInfo mix_info;
    mix_info.id = 2;
    mix_info.name = "Mix";
    mix_info.unit = "%";
    mix_info.range = {0.0f, 100.0f, 100.0f, 0.0f};

    store.add_parameter(gain_info);
    store.add_parameter(mix_info);

    SECTION("Initial values are defaults") {
        REQUIRE_THAT(store.get_value(1), WithinAbs(0.0, 0.001));
        REQUIRE_THAT(store.get_value(2), WithinAbs(100.0, 0.001));
    }

    SECTION("Set and get") {
        store.set_value(1, -12.0f);
        REQUIRE_THAT(store.get_value(1), WithinAbs(-12.0, 0.001));
    }

    SECTION("Values clamped to range") {
        store.set_value(1, 999.0f);
        REQUIRE_THAT(store.get_value(1), WithinAbs(12.0, 0.001));

        store.set_value(1, -999.0f);
        REQUIRE_THAT(store.get_value(1), WithinAbs(-60.0, 0.001));
    }

    SECTION("Normalized access") {
        store.set_normalized(1, 0.5f);
        float expected = -60.0f + 0.5f * (12.0f - (-60.0f)); // -24 dB
        REQUIRE_THAT(store.get_value(1), WithinAbs(expected, 0.1));
    }

    SECTION("Reset to default") {
        store.set_value(1, -12.0f);
        store.reset_to_default(1);
        REQUIRE_THAT(store.get_value(1), WithinAbs(0.0, 0.001));
    }

    SECTION("Param info lookup") {
        auto* info = store.info(1);
        REQUIRE(info != nullptr);
        REQUIRE(info->name == "Gain");
        REQUIRE(info->unit == "dB");

        REQUIRE(store.info(999) == nullptr);
    }

    SECTION("Unknown param returns 0") {
        REQUIRE(store.get_value(999) == 0.0f);
    }

    SECTION("Param count") {
        REQUIRE(store.param_count() == 2);
    }
}

TEST_CASE("StateStore serialization", "[state][serialize]") {
    StateStore store;

    ParamInfo p1 = make_param_info(1, "A", "", {0.0f, 1.0f, 0.5f});
    ParamInfo p2 = make_param_info(2, "B", "", {-1.0f, 1.0f, 0.0f});
    store.add_parameter(p1);
    store.add_parameter(p2);

    store.set_value(1, 0.75f);
    store.set_value(2, -0.5f);

    auto data = store.serialize();
    REQUIRE_FALSE(data.empty());

    SECTION("Round-trip") {
        StateStore store2;
        store2.add_parameter(p1);
        store2.add_parameter(p2);

        REQUIRE(store2.deserialize(data));
        REQUIRE_THAT(store2.get_value(1), WithinAbs(0.75, 0.001));
        REQUIRE_THAT(store2.get_value(2), WithinAbs(-0.5, 0.001));
    }

    SECTION("Forward compatibility (unknown params ignored)") {
        StateStore store3;
        store3.add_parameter(p1); // Only knows param 1, not 2

        REQUIRE(store3.deserialize(data));
        REQUIRE_THAT(store3.get_value(1), WithinAbs(0.75, 0.001));
    }

    SECTION("Backward compatibility (missing params keep defaults)") {
        StateStore store_old;
        store_old.add_parameter(p1);
        store_old.set_value(1, 0.9f);
        auto old_data = store_old.serialize();

        StateStore store_new;
        store_new.add_parameter(p1);
        store_new.add_parameter(p2); // New param, not in old data

        REQUIRE(store_new.deserialize(old_data));
        REQUIRE_THAT(store_new.get_value(1), WithinAbs(0.9, 0.001));
        REQUIRE_THAT(store_new.get_value(2), WithinAbs(0.0, 0.001)); // Default
    }

    SECTION("Corrupted data rejected") {
        auto bad_data = data;
        bad_data[bad_data.size() - 1] ^= 0xFF; // Corrupt CRC
        StateStore store4;
        store4.add_parameter(p1);
        REQUIRE_FALSE(store4.deserialize(bad_data));
    }

    SECTION("Too-short data rejected") {
        REQUIRE_FALSE(store.deserialize(std::span<const uint8_t>{}));
    }
}

TEST_CASE("StateStore change listener", "[state][listener]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.5f}));

    ParamID changed_id = 0;
    float changed_value = 0.0f;
    store.add_listener([&](ParamID id, float val) {
        changed_id = id;
        changed_value = val;
    });

    store.set_value(1, 0.8f);
    REQUIRE(changed_id == 1);
    REQUIRE_THAT(changed_value, WithinAbs(0.8, 0.001));
}

TEST_CASE("StateStore modulation offsets and default resets", "[state][store]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Cutoff", "Hz", {20.0f, 20000.0f, 1000.0f}));
    store.add_parameter(make_param_info(2, "Resonance", "", {0.1f, 10.0f, 1.0f}));

    REQUIRE(store.get_modulated(999) == 0.0f);
    REQUIRE(store.get_normalized(999) == 0.0f);
    REQUIRE(store.get_default(999) == 0.0f);

    store.set_value(1, 2000.0f);
    store.set_mod_offset(1, 150.0f);
    REQUIRE_THAT(store.get_modulated(1), WithinAbs(2150.0, 0.001));

    store.add_mod_offset(1, -25.0f);
    REQUIRE_THAT(store.get_modulated(1), WithinAbs(2125.0, 0.001));

    store.set_mod_offset(2, 0.75f);
    store.set_mod_offset(999, 123.0f);
    store.add_mod_offset(999, 456.0f);

    store.reset_all_mod();
    REQUIRE_THAT(store.get_modulated(1), WithinAbs(2000.0, 0.001));
    REQUIRE_THAT(store.get_modulated(2), WithinAbs(1.0, 0.001));

    store.set_value(1, 4000.0f);
    store.set_value(2, 4.0f);
    store.reset_all_to_defaults();
    REQUIRE_THAT(store.get_value(1), WithinAbs(1000.0, 0.001));
    REQUIRE_THAT(store.get_value(2), WithinAbs(1.0, 0.001));
}

TEST_CASE("StateStore exposes registration spans and gesture callbacks", "[state][store]") {
    StateStore store;

    store.begin_gesture(1);
    store.end_gesture(1);

    store.add_group({10, "Main", 0});
    store.add_group({20, "Oscillator", 10});
    store.add_parameter(make_param_info(1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}));
    store.add_parameter(make_param_info(2, "Mix", "%", {0.0f, 100.0f, 50.0f}));

    auto params = store.all_params();
    REQUIRE(params.size() == 2);
    REQUIRE(params[0].id == 1);
    REQUIRE(params[0].name == "Gain");
    REQUIRE(params[1].unit == "%");

    auto groups = store.all_groups();
    REQUIRE(groups.size() == 2);
    REQUIRE(groups[0].name == "Main");
    REQUIRE(groups[1].parent_id == 10);

    std::vector<ParamID> began;
    std::vector<ParamID> ended;
    store.set_gesture_callbacks(
        [&](ParamID id) { began.push_back(id); },
        [&](ParamID id) { ended.push_back(id); });

    store.begin_gesture(2);
    store.end_gesture(2);

    REQUIRE(began == std::vector<ParamID>{2});
    REQUIRE(ended == std::vector<ParamID>{2});
}
