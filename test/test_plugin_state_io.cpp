#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <choc/memory/choc_Endianness.h>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/processor.hpp>

#include <array>
#include <cstdint>
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
        store.add_group({
            .id = 1,
            .name = "Main",
        });
        store.add_parameter({
            .id = 1,
            .name = "Gain",
            .unit = "dB",
            .range = {-60.0f, 24.0f, 0.0f, 0.1f},
            .group_id = 1,
        });
        store.add_parameter({
            .id = 2,
            .name = "Mix",
            .unit = "%",
            .range = {0.0f, 100.0f, 100.0f, 0.1f},
            .group_id = 1,
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

class DefaultPluginStateProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "DefaultPluginStateProcessor",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.default-plugin-state-io",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
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

struct TestRig {
    pulp::state::StateStore store;
    TestPluginStateProcessor processor;

    TestRig() {
        processor.set_state_store(&store);
        processor.define_parameters(store);
    }
};

constexpr uint8_t kEnvelopeMagic[4] = {'P', 'L', 'S', 'T'};

uint32_t crc32_simple(const uint8_t* data, std::size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            const uint32_t mask = (crc & 1u) ? 0xFFFFFFFFu : 0u;
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void append_u32(std::vector<uint8_t>& bytes, uint32_t value) {
    uint8_t out[4];
    choc::memory::writeLittleEndian(out, value);
    bytes.insert(bytes.end(), out, out + sizeof(out));
}

void write_u32(std::vector<uint8_t>& bytes, std::size_t offset, uint32_t value) {
    REQUIRE(offset + 4 <= bytes.size());
    choc::memory::writeLittleEndian(bytes.data() + offset, value);
}

std::vector<uint8_t> make_envelope(std::span<const uint8_t> store_blob,
                                   std::span<const uint8_t> plugin_blob = {},
                                   uint32_t version = 1) {
    std::vector<uint8_t> bytes;
    bytes.insert(bytes.end(), kEnvelopeMagic, kEnvelopeMagic + 4);
    append_u32(bytes, version);
    append_u32(bytes, static_cast<uint32_t>(store_blob.size()));
    append_u32(bytes, static_cast<uint32_t>(plugin_blob.size()));
    bytes.insert(bytes.end(), store_blob.begin(), store_blob.end());
    bytes.insert(bytes.end(), plugin_blob.begin(), plugin_blob.end());
    append_u32(bytes, crc32_simple(bytes.data(), bytes.size()));
    return bytes;
}

} // namespace

TEST_CASE("Processor default plugin-state hooks are no-ops", "[format][plugin-state]") {
    DefaultPluginStateProcessor processor;
    REQUIRE(processor.serialize_plugin_state().empty());

    REQUIRE(processor.deserialize_plugin_state({}));

    const std::array<uint8_t, 4> payload = {'N', 'O', 'P', 'E'};
    REQUIRE(processor.deserialize_plugin_state(payload));
}

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

TEST_CASE("plugin_state_io envelope with empty plugin payload resets plugin state",
          "[format][plugin-state][coverage][issue-647]") {
    TestRig source;
    source.store.set_value(1, -15.0f);
    auto store_blob = source.store.serialize();
    auto envelope = make_envelope(store_blob, {});

    TestRig restored;
    restored.store.set_value(1, 6.0f);
    restored.processor.plugin_state = "stale";

    REQUIRE(pulp::format::plugin_state_io::deserialize(envelope,
                                                       restored.store,
                                                       restored.processor));
    REQUIRE_THAT(restored.store.get_value(1), WithinAbs(-15.0, 0.01));
    REQUIRE(restored.processor.plugin_state.empty());
    REQUIRE(restored.processor.deserialize_calls == 1);
    REQUIRE(restored.processor.last_payload.empty());
}

TEST_CASE("plugin_state_io serialize falls back to raw StateStore blobs when plugin payload is empty",
          "[format][plugin-state]") {
    TestRig source;
    source.store.set_value(1, -7.5f);
    source.processor.plugin_state.clear();

    auto blob = pulp::format::plugin_state_io::serialize(source.store, source.processor);
    REQUIRE(blob.size() >= 4);
    REQUIRE(blob[0] == 'P');
    REQUIRE(blob[1] == 'U');
    REQUIRE(blob[2] == 'L');
    REQUIRE(blob[3] == 'P');
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

TEST_CASE("plugin_state_io rejects malformed blobs without touching live state",
          "[format][plugin-state]") {
    TestRig reference;
    reference.store.set_value(1, -18.0f);
    reference.processor.plugin_state = "snapshot-a";
    auto legacy_blob = reference.store.serialize();

    std::vector<uint8_t> bad_store = {'N', 'O', 'P', 'E'};

    SECTION("non-envelope garbage") {
        std::vector<uint8_t> blob = {'N', 'O', 'P', 'E'};

        TestRig restored;
        restored.store.set_value(1, 3.0f);
        restored.processor.plugin_state = "keep";

        REQUIRE_FALSE(pulp::format::plugin_state_io::deserialize(blob,
                                                                 restored.store,
                                                                 restored.processor));
        REQUIRE_THAT(restored.store.get_value(1), WithinAbs(3.0, 0.01));
        REQUIRE(restored.processor.plugin_state == "keep");
    }

    SECTION("truncated envelope") {
        std::vector<uint8_t> blob = {'P', 'L', 'S', 'T'};

        TestRig restored;
        restored.store.set_value(1, 3.0f);
        restored.processor.plugin_state = "keep";

        REQUIRE_FALSE(pulp::format::plugin_state_io::deserialize(blob,
                                                                 restored.store,
                                                                 restored.processor));
        REQUIRE_THAT(restored.store.get_value(1), WithinAbs(3.0, 0.01));
        REQUIRE(restored.processor.plugin_state == "keep");
    }

    SECTION("unsupported version") {
        auto blob = make_envelope(legacy_blob);
        write_u32(blob, 4, 2);

        TestRig restored;
        restored.store.set_value(1, 3.0f);
        restored.processor.plugin_state = "keep";

        REQUIRE_FALSE(pulp::format::plugin_state_io::deserialize(blob,
                                                                 restored.store,
                                                                 restored.processor));
        REQUIRE_THAT(restored.store.get_value(1), WithinAbs(3.0, 0.01));
        REQUIRE(restored.processor.plugin_state == "keep");
    }

    SECTION("payload size exceeds bytes available") {
        auto blob = make_envelope(legacy_blob);
        write_u32(blob, 8, static_cast<uint32_t>(legacy_blob.size() + 1));

        TestRig restored;
        restored.store.set_value(1, 3.0f);
        restored.processor.plugin_state = "keep";

        REQUIRE_FALSE(pulp::format::plugin_state_io::deserialize(blob,
                                                                 restored.store,
                                                                 restored.processor));
        REQUIRE_THAT(restored.store.get_value(1), WithinAbs(3.0, 0.01));
        REQUIRE(restored.processor.plugin_state == "keep");
    }

    SECTION("size mismatch with trailing bytes") {
        auto blob = make_envelope(legacy_blob);
        blob.push_back(0x7F);

        TestRig restored;
        restored.store.set_value(1, 3.0f);
        restored.processor.plugin_state = "keep";

        REQUIRE_FALSE(pulp::format::plugin_state_io::deserialize(blob,
                                                                 restored.store,
                                                                 restored.processor));
        REQUIRE_THAT(restored.store.get_value(1), WithinAbs(3.0, 0.01));
        REQUIRE(restored.processor.plugin_state == "keep");
    }

    SECTION("invalid inner StateStore payload") {
        auto blob = make_envelope(bad_store);

        TestRig restored;
        restored.store.set_value(1, 3.0f);
        restored.processor.plugin_state = "keep";

        REQUIRE_FALSE(pulp::format::plugin_state_io::deserialize(blob,
                                                                 restored.store,
                                                                 restored.processor));
        REQUIRE_THAT(restored.store.get_value(1), WithinAbs(3.0, 0.01));
        REQUIRE(restored.processor.plugin_state == "keep");
        REQUIRE(restored.processor.deserialize_calls == 0);
    }

    SECTION("empty inner StateStore payload") {
        const std::array<uint8_t, 4> plugin_blob = {'K', 'E', 'E', 'P'};
        auto blob = make_envelope({}, plugin_blob);

        TestRig restored;
        restored.store.set_value(1, 3.0f);
        restored.processor.plugin_state = "keep";

        REQUIRE_FALSE(pulp::format::plugin_state_io::deserialize(blob,
                                                                 restored.store,
                                                                 restored.processor));
        REQUIRE_THAT(restored.store.get_value(1), WithinAbs(3.0, 0.01));
        REQUIRE(restored.processor.plugin_state == "keep");
        REQUIRE(restored.processor.deserialize_calls == 0);
    }
}

TEST_CASE("plugin_state_io rejects envelope CRC mismatch without plugin callback",
          "[format][plugin-state][coverage][issue-647]") {
    TestRig source;
    source.store.set_value(1, -21.0f);
    source.processor.plugin_state = "snapshot-c";
    auto store_blob = source.store.serialize();
    auto plugin_blob = source.processor.serialize_plugin_state();
    auto blob = make_envelope(store_blob, plugin_blob);
    blob[8] ^= 0x01; // mutate store size after CRC calculation

    TestRig restored;
    restored.store.set_value(1, 2.0f);
    restored.processor.plugin_state = "keep";

    REQUIRE_FALSE(pulp::format::plugin_state_io::deserialize(blob,
                                                             restored.store,
                                                             restored.processor));
    REQUIRE_THAT(restored.store.get_value(1), WithinAbs(2.0, 0.01));
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
