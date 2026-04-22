#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/processor.hpp>

#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

class TestPluginStateProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "PluginStateIOTest",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.plugin-state-io",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = 1,
            .name = "Gain",
            .unit = "dB",
            .range = {-60.0f, 24.0f, 0.0f, 0.1f},
        });
        store.add_parameter({
            .id = 2,
            .name = "Mix",
            .unit = "%",
            .range = {0.0f, 100.0f, 100.0f, 0.1f},
        });
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}

    std::vector<uint8_t> serialize_plugin_state() const override {
        return std::vector<uint8_t>(plugin_state.begin(), plugin_state.end());
    }

    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        ++deserialize_calls;
        last_payload.assign(data.begin(), data.end());
        const std::string payload(data.begin(), data.end());
        if (!rejected_payload.empty() && payload == rejected_payload) {
            return false;
        }

        plugin_state = payload;
        return true;
    }

    std::string plugin_state;
    std::string rejected_payload;
    int deserialize_calls = 0;
    std::vector<uint8_t> last_payload;
};

struct TestRig {
    pulp::state::StateStore store;
    TestPluginStateProcessor processor;

    TestRig() {
        processor.set_state_store(&store);
        processor.define_parameters(store);
    }
};

} // namespace

TEST_CASE("plugin_state_io round-trips parameter and plugin-owned state",
          "[format][plugin-state]") {
    TestRig source;
    source.store.set_value(1, -12.5f);
    source.store.set_value(2, 42.0f);
    source.processor.plugin_state = "bands=48;view=60-12000";

    auto blob = pulp::format::plugin_state_io::serialize(source.store, source.processor);
    REQUIRE(blob.size() >= 4);
    REQUIRE(blob[0] == 'P');
    REQUIRE(blob[1] == 'L');
    REQUIRE(blob[2] == 'S');
    REQUIRE(blob[3] == 'T');

    TestRig restored;
    restored.store.set_value(1, 6.0f);
    restored.processor.plugin_state = "stale";

    REQUIRE(pulp::format::plugin_state_io::deserialize(blob,
                                                       restored.store,
                                                       restored.processor));
    REQUIRE_THAT(restored.store.get_value(1), WithinAbs(-12.5, 0.01));
    REQUIRE_THAT(restored.store.get_value(2), WithinAbs(42.0, 0.01));
    REQUIRE(restored.processor.plugin_state == "bands=48;view=60-12000");
}

TEST_CASE("plugin_state_io preserves legacy raw StateStore blobs and resets plugin payload",
          "[format][plugin-state]") {
    TestRig source;
    source.store.set_value(1, -9.0f);
    auto legacy_blob = source.store.serialize();
    REQUIRE(legacy_blob.size() >= 4);
    REQUIRE(legacy_blob[0] == 'P');
    REQUIRE(legacy_blob[1] == 'U');
    REQUIRE(legacy_blob[2] == 'L');
    REQUIRE(legacy_blob[3] == 'P');

    TestRig restored;
    restored.processor.plugin_state = "stale";

    REQUIRE(pulp::format::plugin_state_io::deserialize(legacy_blob,
                                                       restored.store,
                                                       restored.processor));
    REQUIRE_THAT(restored.store.get_value(1), WithinAbs(-9.0, 0.01));
    REQUIRE(restored.processor.plugin_state.empty());
    REQUIRE(restored.processor.deserialize_calls == 1);
    REQUIRE(restored.processor.last_payload.empty());
}

TEST_CASE("plugin_state_io rejects corrupt envelopes without touching live state",
          "[format][plugin-state]") {
    TestRig source;
    source.store.set_value(1, -18.0f);
    source.processor.plugin_state = "snapshot-a";
    auto blob = pulp::format::plugin_state_io::serialize(source.store, source.processor);
    blob.back() ^= 0xFF;

    TestRig restored;
    restored.store.set_value(1, 3.0f);
    restored.store.set_value(2, 77.0f);
    restored.processor.plugin_state = "keep";

    REQUIRE_FALSE(pulp::format::plugin_state_io::deserialize(blob,
                                                             restored.store,
                                                             restored.processor));
    REQUIRE_THAT(restored.store.get_value(1), WithinAbs(3.0, 0.01));
    REQUIRE_THAT(restored.store.get_value(2), WithinAbs(77.0, 0.01));
    REQUIRE(restored.processor.plugin_state == "keep");
    REQUIRE(restored.processor.deserialize_calls == 0);
}

TEST_CASE("plugin_state_io rolls back StateStore when plugin payload restore fails",
          "[format][plugin-state]") {
    TestRig source;
    source.store.set_value(1, -24.0f);
    source.processor.plugin_state = "snapshot-b";
    auto blob = pulp::format::plugin_state_io::serialize(source.store, source.processor);

    TestRig restored;
    restored.store.set_value(1, 5.0f);
    restored.store.set_value(2, 12.0f);
    restored.processor.plugin_state = "keep";
    restored.processor.rejected_payload = "snapshot-b";

    REQUIRE_FALSE(pulp::format::plugin_state_io::deserialize(blob,
                                                             restored.store,
                                                             restored.processor));
    REQUIRE_THAT(restored.store.get_value(1), WithinAbs(5.0, 0.01));
    REQUIRE_THAT(restored.store.get_value(2), WithinAbs(12.0, 0.01));
    REQUIRE(restored.processor.plugin_state == "keep");
    REQUIRE(restored.processor.deserialize_calls == 2);
}
