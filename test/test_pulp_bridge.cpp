#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// Intentionally include the bridge implementation so this test can seed
// the anonymous-namespace fallback StateStore without changing production code.
#include "../apple/Sources/PulpSwift/PulpBridge.cpp"

#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

constexpr PulpParamID kBridgeParamId = 101;
constexpr PulpParamID kSecondBridgeParamId = 202;

class TestBridgeProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpBridgeTest",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.bridge",
            .version = "1.2.3",
            .category = pulp::format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Main Out", 2}},
            .accepts_midi = true,
        };
    }

    void define_parameters(pulp::state::StateStore&) override {}

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
};

class EffectBridgeProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpBridgeEffect",
            .manufacturer = "Pulp Labs",
            .bundle_id = "com.pulp.test.bridge-effect",
            .version = "2.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Main In", 2}},
            .output_buses = {{"Main Out", 2}},
            .accepts_midi = false,
            .produces_midi = true,
        };
    }

    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
};

std::unique_ptr<pulp::format::Processor> create_test_processor() {
    return std::make_unique<TestBridgeProcessor>();
}

std::unique_ptr<pulp::format::Processor> create_effect_processor() {
    return std::make_unique<EffectBridgeProcessor>();
}

std::unique_ptr<pulp::format::Processor> create_null_processor() {
    return nullptr;
}

struct ScopedFactoryRegistration {
    explicit ScopedFactoryRegistration(pulp::format::ProcessorFactory factory)
        : previous(pulp::format::registered_factory()) {
        pulp::format::register_plugin(factory);
    }

    ~ScopedFactoryRegistration() {
        pulp::format::register_plugin(previous);
    }

    pulp::format::ProcessorFactory previous;
};

pulp::state::StateStore& bridge_store() {
    auto& store = *active_store();
    static bool initialized = false;
    if (!initialized) {
        store.add_parameter({
            .id = kBridgeParamId,
            .name = "Bridge Gain",
            .unit = "dB",
            .range = {-60.0f, 24.0f, 0.0f, 0.1f},
        });
        store.add_parameter({
            .id = kSecondBridgeParamId,
            .name = "Bridge Mix",
            .unit = "%",
            .range = {0.0f, 100.0f, 50.0f, 1.0f},
        });
        initialized = true;
    }
    return store;
}

void reset_bridge_store() {
    auto& store = bridge_store();
    store.reset_all_mod();
    store.reset_all_to_defaults();
    store.set_gesture_callbacks({}, {});
}

} // namespace

TEST_CASE("PulpBridge fallback store parameter APIs are deterministic",
          "[apple][bridge][params]") {
    reset_bridge_store();
    auto& store = bridge_store();
    const auto* param = store.info(kBridgeParamId);
    REQUIRE(param != nullptr);

    REQUIRE(pulp_param_count() == 2);

    PulpParamInfo info{};
    REQUIRE_FALSE(pulp_param_info(-1, &info));
    REQUIRE_FALSE(pulp_param_info(2, &info));
    REQUIRE_FALSE(pulp_param_info(0, nullptr));
    REQUIRE(pulp_param_info(0, &info));
    REQUIRE(info.id == kBridgeParamId);
    REQUIRE(std::string(info.name) == "Bridge Gain");
    REQUIRE(std::string(info.unit) == "dB");
    REQUIRE_THAT(info.min_value, WithinAbs(-60.0f, 0.0001f));
    REQUIRE_THAT(info.max_value, WithinAbs(24.0f, 0.0001f));
    REQUIRE_THAT(info.default_value, WithinAbs(0.0f, 0.0001f));
    REQUIRE_THAT(info.step, WithinAbs(0.1f, 0.0001f));

    REQUIRE(pulp_param_info(1, &info));
    REQUIRE(info.id == kSecondBridgeParamId);
    REQUIRE(std::string(info.name) == "Bridge Mix");
    REQUIRE(std::string(info.unit) == "%");
    REQUIRE_THAT(info.min_value, WithinAbs(0.0f, 0.0001f));
    REQUIRE_THAT(info.max_value, WithinAbs(100.0f, 0.0001f));
    REQUIRE_THAT(info.default_value, WithinAbs(50.0f, 0.0001f));
    REQUIRE_THAT(info.step, WithinAbs(1.0f, 0.0001f));

    REQUIRE_THAT(pulp_param_get(kBridgeParamId), WithinAbs(0.0f, 0.0001f));

    pulp_param_set(kBridgeParamId, 6.0f);
    REQUIRE_THAT(pulp_param_get(kBridgeParamId), WithinAbs(6.0f, 0.0001f));
    REQUIRE_THAT(pulp_param_get_normalized(kBridgeParamId),
                 WithinAbs(param->range.normalize(6.0f), 0.0001f));

    pulp_param_set_normalized(kBridgeParamId, 0.5f);
    REQUIRE_THAT(pulp_param_get(kBridgeParamId), WithinAbs(-18.0f, 0.0001f));
    REQUIRE_THAT(pulp_param_get_normalized(kBridgeParamId), WithinAbs(0.5f, 0.0001f));

    int begin_calls = 0;
    int end_calls = 0;
    PulpParamID began = 0;
    PulpParamID ended = 0;
    store.set_gesture_callbacks(
        [&](pulp::state::ParamID id) {
            ++begin_calls;
            began = id;
        },
        [&](pulp::state::ParamID id) {
            ++end_calls;
            ended = id;
        });

    pulp_param_begin_gesture(kBridgeParamId);
    pulp_param_end_gesture(kBridgeParamId);
    REQUIRE(begin_calls == 1);
    REQUIRE(end_calls == 1);
    REQUIRE(began == kBridgeParamId);
    REQUIRE(ended == kBridgeParamId);

    pulp_param_reset(kBridgeParamId);
    REQUIRE_THAT(pulp_param_get(kBridgeParamId), WithinAbs(0.0f, 0.0001f));
}

TEST_CASE("PulpBridge state serialize and deserialize use the fallback store",
          "[apple][bridge][state]") {
    reset_bridge_store();
    pulp_param_set(kBridgeParamId, -12.5f);
    pulp_param_set(kSecondBridgeParamId, 75.0f);

    REQUIRE(pulp_state_serialize(nullptr) == nullptr);

    int size = -1;
    uint8_t* bytes = pulp_state_serialize(&size);
    REQUIRE(bytes != nullptr);
    REQUIRE(size >= 16);
    REQUIRE(std::memcmp(bytes, "PULP", 4) == 0);

    pulp_param_set(kBridgeParamId, 9.0f);
    pulp_param_set(kSecondBridgeParamId, 12.0f);
    REQUIRE_FALSE(pulp_state_deserialize(nullptr, size));
    REQUIRE_FALSE(pulp_state_deserialize(bytes, 0));
    REQUIRE(pulp_state_deserialize(bytes, size));
    REQUIRE_THAT(pulp_param_get(kBridgeParamId), WithinAbs(-12.5f, 0.0001f));
    REQUIRE_THAT(pulp_param_get(kSecondBridgeParamId), WithinAbs(75.0f, 0.0001f));

    pulp_free(bytes);
    pulp_free(nullptr);
}

TEST_CASE("PulpBridge parameter calls fail closed for unknown ids",
          "[apple][bridge][params][coverage]") {
    reset_bridge_store();
    constexpr PulpParamID kMissingParamId = 90909;

    REQUIRE_THAT(pulp_param_get(kMissingParamId), WithinAbs(0.0f, 0.0001f));
    REQUIRE_THAT(pulp_param_get_normalized(kMissingParamId), WithinAbs(0.0f, 0.0001f));

    pulp_param_set(kMissingParamId, 19.0f);
    REQUIRE_THAT(pulp_param_get(kBridgeParamId), WithinAbs(0.0f, 0.0001f));
    REQUIRE_THAT(pulp_param_get(kMissingParamId), WithinAbs(0.0f, 0.0001f));

    pulp_param_set_normalized(kMissingParamId, 0.75f);
    REQUIRE_THAT(pulp_param_get(kBridgeParamId), WithinAbs(0.0f, 0.0001f));
    REQUIRE_THAT(pulp_param_get_normalized(kMissingParamId), WithinAbs(0.0f, 0.0001f));
    REQUIRE(pulp_param_count() == 2);

    PulpParamInfo info{};
    REQUIRE(pulp_param_info(0, &info));
    REQUIRE(info.id == kBridgeParamId);
    REQUIRE(std::string(info.name) == "Bridge Gain");

    int begin_calls = 0;
    int end_calls = 0;
    bridge_store().set_gesture_callbacks(
        [&](pulp::state::ParamID id) {
            if (id == kMissingParamId) ++begin_calls;
        },
        [&](pulp::state::ParamID id) {
            if (id == kMissingParamId) ++end_calls;
        });
    pulp_param_begin_gesture(kMissingParamId);
    pulp_param_end_gesture(kMissingParamId);
    REQUIRE(begin_calls == 1);
    REQUIRE(end_calls == 1);

    pulp_param_reset(kMissingParamId);
    REQUIRE_THAT(pulp_param_get(kBridgeParamId), WithinAbs(0.0f, 0.0001f));
}

TEST_CASE("PulpBridge parameters clamp raw and normalized values",
          "[apple][bridge][params][coverage]") {
    reset_bridge_store();

    pulp_param_set(kBridgeParamId, -999.0f);
    REQUIRE_THAT(pulp_param_get(kBridgeParamId), WithinAbs(-60.0f, 0.0001f));
    REQUIRE_THAT(pulp_param_get_normalized(kBridgeParamId), WithinAbs(0.0f, 0.0001f));

    pulp_param_set(kBridgeParamId, 999.0f);
    REQUIRE_THAT(pulp_param_get(kBridgeParamId), WithinAbs(24.0f, 0.0001f));
    REQUIRE_THAT(pulp_param_get_normalized(kBridgeParamId), WithinAbs(1.0f, 0.0001f));

    pulp_param_set_normalized(kSecondBridgeParamId, -1.0f);
    REQUIRE_THAT(pulp_param_get(kSecondBridgeParamId), WithinAbs(0.0f, 0.0001f));

    pulp_param_set_normalized(kSecondBridgeParamId, 2.0f);
    REQUIRE_THAT(pulp_param_get(kSecondBridgeParamId), WithinAbs(100.0f, 0.0001f));

    pulp_param_set(kSecondBridgeParamId, 25.0f);
    pulp_param_reset(999999);
    REQUIRE_THAT(pulp_param_get(kSecondBridgeParamId), WithinAbs(25.0f, 0.0001f));
    REQUIRE_THAT(pulp_param_get_normalized(kSecondBridgeParamId), WithinAbs(0.25f, 0.0001f));
}

TEST_CASE("PulpBridge deserialize rejects malformed state without clobbering values",
          "[apple][bridge][state][coverage]") {
    reset_bridge_store();
    pulp_param_set(kBridgeParamId, 7.25f);
    pulp_param_set(kSecondBridgeParamId, 33.0f);

    std::vector<uint8_t> empty;
    std::vector<uint8_t> bad_magic = {'N', 'O', 'P', 'E'};
    std::vector<uint8_t> truncated = {'P', 'U', 'L', 'P', 0, 0, 0, 1};

    REQUIRE_FALSE(pulp_state_deserialize(empty.data(), 0));
    REQUIRE_THAT(pulp_param_get(kBridgeParamId), WithinAbs(7.25f, 0.0001f));

    REQUIRE_FALSE(pulp_state_deserialize(bad_magic.data(), static_cast<int>(bad_magic.size())));
    REQUIRE_THAT(pulp_param_get(kBridgeParamId), WithinAbs(7.25f, 0.0001f));

    REQUIRE_FALSE(pulp_state_deserialize(truncated.data(), static_cast<int>(truncated.size())));
    REQUIRE_THAT(pulp_param_get(kBridgeParamId), WithinAbs(7.25f, 0.0001f));
    REQUIRE_THAT(pulp_param_get(kSecondBridgeParamId), WithinAbs(33.0f, 0.0001f));

    int size = 0;
    uint8_t* bytes = pulp_state_serialize(&size);
    REQUIRE(bytes != nullptr);
    REQUIRE(size > 0);
    REQUIRE(size >= 16);
    REQUIRE(std::memcmp(bytes, "PULP", 4) == 0);
    REQUIRE(pulp_state_deserialize(bytes, size));
    REQUIRE_THAT(pulp_param_get(kBridgeParamId), WithinAbs(7.25f, 0.0001f));
    REQUIRE_THAT(pulp_param_get(kSecondBridgeParamId), WithinAbs(33.0f, 0.0001f));
    pulp_free(bytes);
}

TEST_CASE("PulpBridge state serialization snapshots current parameter values",
          "[apple][bridge][state][coverage]") {
    reset_bridge_store();

    pulp_param_set(kBridgeParamId, -6.0f);
    int first_size = 0;
    uint8_t* first = pulp_state_serialize(&first_size);
    REQUIRE(first != nullptr);
    REQUIRE(first_size > 0);

    pulp_param_set(kBridgeParamId, 18.0f);
    int second_size = 0;
    uint8_t* second = pulp_state_serialize(&second_size);
    REQUIRE(second != nullptr);
    REQUIRE(second_size == first_size);

    REQUIRE(pulp_state_deserialize(first, first_size));
    REQUIRE_THAT(pulp_param_get(kBridgeParamId), WithinAbs(-6.0f, 0.0001f));

    REQUIRE(pulp_state_deserialize(second, second_size));
    REQUIRE_THAT(pulp_param_get(kBridgeParamId), WithinAbs(18.0f, 0.0001f));

    pulp_free(first);
    pulp_free(second);
}

TEST_CASE("PulpBridge plugin info rejects missing factories and null outputs",
          "[apple][bridge][plugin-info]") {
    PulpPluginInfo info{};

    {
        ScopedFactoryRegistration registration(create_test_processor);
        REQUIRE_FALSE(pulp_plugin_info(nullptr));
    }

    {
        ScopedFactoryRegistration registration(nullptr);
        REQUIRE_FALSE(pulp_plugin_info(&info));
    }

    {
        ScopedFactoryRegistration registration(create_null_processor);
        REQUIRE_FALSE(pulp_plugin_info(&info));
    }
}

TEST_CASE("PulpBridge plugin info exposes the registered descriptor",
          "[apple][bridge][plugin-info]") {
    ScopedFactoryRegistration registration(create_test_processor);

    PulpPluginInfo info{};
    REQUIRE(pulp_plugin_info(&info));
    REQUIRE(info.name != nullptr);
    REQUIRE(info.manufacturer != nullptr);
    REQUIRE(info.version != nullptr);
    REQUIRE(info.bundle_id != nullptr);
    REQUIRE(std::string(info.name) == "PulpBridgeTest");
    REQUIRE(std::string(info.manufacturer) == "Pulp");
    REQUIRE(std::string(info.version) == "1.2.3");
    REQUIRE(std::string(info.bundle_id) == "com.pulp.test.bridge");
    REQUIRE(info.category ==
            static_cast<int>(pulp::format::PluginCategory::Instrument));
    REQUIRE(info.accepts_midi);
    REQUIRE_FALSE(info.produces_midi);
}

TEST_CASE("PulpBridge plugin info string storage survives later descriptor calls",
          "[apple][bridge][plugin-info][coverage]") {
    ScopedFactoryRegistration registration(create_test_processor);

    PulpPluginInfo first{};
    REQUIRE(pulp_plugin_info(&first));
    REQUIRE(first.name != nullptr);
    REQUIRE(first.manufacturer != nullptr);
    REQUIRE(first.version != nullptr);
    REQUIRE(first.bundle_id != nullptr);
    REQUIRE(std::string(first.name) == "PulpBridgeTest");
    REQUIRE(std::string(first.manufacturer) == "Pulp");
    REQUIRE(std::string(first.version) == "1.2.3");
    REQUIRE(std::string(first.bundle_id) == "com.pulp.test.bridge");

    PulpPluginInfo second{};
    REQUIRE(pulp_plugin_info(&second));
    REQUIRE(second.name != nullptr);
    REQUIRE(second.manufacturer != nullptr);
    REQUIRE(second.version != nullptr);
    REQUIRE(second.bundle_id != nullptr);
    REQUIRE(std::string(second.name) == "PulpBridgeTest");
    REQUIRE(std::string(second.manufacturer) == "Pulp");
    REQUIRE(std::string(second.version) == "1.2.3");
    REQUIRE(std::string(second.bundle_id) == "com.pulp.test.bridge");
}

TEST_CASE("PulpBridge plugin info updates static string storage per descriptor",
          "[apple][bridge][plugin-info][coverage]") {
    PulpPluginInfo info{};

    {
        ScopedFactoryRegistration registration(create_test_processor);
        REQUIRE(pulp_plugin_info(&info));
        REQUIRE(std::string(info.name) == "PulpBridgeTest");
        REQUIRE(info.category ==
                static_cast<int>(pulp::format::PluginCategory::Instrument));
        REQUIRE(info.accepts_midi);
        REQUIRE_FALSE(info.produces_midi);
    }

    {
        ScopedFactoryRegistration registration(create_effect_processor);
        REQUIRE(pulp_plugin_info(&info));
        REQUIRE(std::string(info.name) == "PulpBridgeEffect");
        REQUIRE(std::string(info.manufacturer) == "Pulp Labs");
        REQUIRE(std::string(info.version) == "2.0.0");
        REQUIRE(std::string(info.bundle_id) == "com.pulp.test.bridge-effect");
        REQUIRE(info.category ==
                static_cast<int>(pulp::format::PluginCategory::Effect));
        REQUIRE_FALSE(info.accepts_midi);
        REQUIRE(info.produces_midi);
    }
}
