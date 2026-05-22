#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <choc/memory/choc_Endianness.h>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/state_migration.hpp>

#include <array>
#include <cstdint>
#include <span>
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

std::string plugin_payload_string(std::span<const uint8_t> envelope) {
    if (envelope.size() < 20
        || envelope[0] != 'P'
        || envelope[1] != 'L'
        || envelope[2] != 'S'
        || envelope[3] != 'T') {
        return {};
    }

    const std::size_t store_size =
        choc::memory::readLittleEndian<uint32_t>(envelope.data() + 8);
    const std::size_t plugin_size =
        choc::memory::readLittleEndian<uint32_t>(envelope.data() + 12);
    const auto plugin_offset = 16 + store_size;
    const auto plugin_end = plugin_offset + plugin_size;
    if (plugin_offset < 16
        || plugin_end < plugin_offset
        || plugin_end > envelope.size() - 4) {
        return {};
    }

    const auto plugin_blob = envelope.subspan(plugin_offset, plugin_size);
    return std::string(plugin_blob.begin(), plugin_blob.end());
}

void rewrite_envelope_version_and_crc(std::vector<uint8_t>& bytes, uint32_t version) {
    if (bytes.size() < 20) {
        return;
    }
    choc::memory::writeLittleEndian(bytes.data() + 4, version);
    const auto crc_offset = bytes.size() - 4;
    choc::memory::writeLittleEndian(bytes.data() + crc_offset,
                                    crc32_simple(bytes.data(), crc_offset));
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

TEST_CASE("plugin_state_io envelope encodes payload sizes and CRC",
          "[format][plugin-state][coverage][phase3]") {
    TestRig source;
    source.store.set_value(1, -2.5f);
    source.processor.plugin_state = "layout=full";

    const auto store_blob = source.store.serialize();
    const auto plugin_blob = source.processor.serialize_plugin_state();
    const auto blob = pulp::format::plugin_state_io::serialize(source.store,
                                                               source.processor);

    REQUIRE(blob.size() == 16 + store_blob.size() + plugin_blob.size() + 4);
    REQUIRE(blob[0] == 'P');
    REQUIRE(blob[1] == 'L');
    REQUIRE(blob[2] == 'S');
    REQUIRE(blob[3] == 'T');
    REQUIRE(choc::memory::readLittleEndian<uint32_t>(blob.data() + 4)
            == pulp::format::plugin_state_io::current_envelope_version());
    REQUIRE(choc::memory::readLittleEndian<uint32_t>(blob.data() + 8)
            == static_cast<uint32_t>(store_blob.size()));
    REQUIRE(choc::memory::readLittleEndian<uint32_t>(blob.data() + 12)
            == static_cast<uint32_t>(plugin_blob.size()));

    const std::vector<uint8_t> encoded_store(blob.begin() + 16,
                                             blob.begin() + 16 + store_blob.size());
    const std::vector<uint8_t> encoded_plugin(blob.begin() + 16 + store_blob.size(),
                                              blob.end() - 4);
    REQUIRE(encoded_store == store_blob);
    REQUIRE(encoded_plugin == plugin_blob);

    const auto crc_offset = blob.size() - 4;
    REQUIRE(choc::memory::readLittleEndian<uint32_t>(blob.data() + crc_offset)
            == crc32_simple(blob.data(), crc_offset));
}

TEST_CASE("plugin_state_io preserves binary plugin-owned payload bytes",
          "[format][plugin-state][coverage][phase3]") {
    TestRig source;
    source.store.set_value(1, -3.0f);
    source.processor.plugin_state.assign(
        std::string({'A', '\0', static_cast<char>(0x7F), static_cast<char>(0xFF), 'Z'}));

    auto blob = pulp::format::plugin_state_io::serialize(source.store, source.processor);

    TestRig restored;
    restored.processor.plugin_state = "stale";
    REQUIRE(pulp::format::plugin_state_io::deserialize(blob,
                                                       restored.store,
                                                       restored.processor));

    const std::vector<uint8_t> expected = {
        'A', 0x00, 0x7F, 0xFF, 'Z',
    };
    REQUIRE(restored.processor.last_payload == expected);
    REQUIRE(restored.processor.serialize_plugin_state() == expected);
    REQUIRE_THAT(restored.store.get_value(1), WithinAbs(-3.0, 0.01));
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

TEST_CASE("plugin_state_io migrates versioned StateStore payloads on read",
          "[format][plugin-state][migration]") {
    TestRig source;
    source.store.set_state_version(1);
    source.store.set_value(1, -6.0f);
    source.processor.plugin_state = "view=compact";
    auto store_blob = source.store.serialize();
    auto plugin_blob = source.processor.serialize_plugin_state();
    auto envelope = make_envelope(store_blob, plugin_blob);

    TestRig restored;
    restored.store.set_state_version(2);
    restored.store.set_value(1, 9.0f);
    restored.processor.plugin_state = "stale";

    REQUIRE(restored.store.register_state_migration(
        1, 2,
        [](std::span<const uint8_t> source_blob, std::vector<uint8_t>& migrated) {
            migrated.assign(source_blob.begin(), source_blob.end());
            write_u32(migrated, 4, 2);
            const auto crc_offset = migrated.size() - 4;
            write_u32(migrated, crc_offset,
                      crc32_simple(migrated.data(), crc_offset));
            return true;
        }));

    REQUIRE(pulp::format::plugin_state_io::deserialize(envelope,
                                                       restored.store,
                                                       restored.processor));
    REQUIRE_THAT(restored.store.get_value(1), WithinAbs(-6.0, 0.01));
    REQUIRE(restored.processor.plugin_state == "view=compact");
}

TEST_CASE("plugin_state_io migrates old envelopes before parsing payloads",
          "[format][plugin-state][migration]") {
    using pulp::format::plugin_state_io::current_envelope_version;
    using pulp::format::plugin_state_io::deserialize;
    using pulp::format::plugin_state_io::register_envelope_migration;

    const auto current_version = current_envelope_version();
    REQUIRE(current_version == 1);

    TestRig source;
    source.store.set_value(1, -8.0f);
    source.store.set_value(2, 61.0f);
    auto make_legacy_envelope = [&source](const std::string& plugin_payload) {
        source.processor.plugin_state = plugin_payload;
        return make_envelope(source.store.serialize(),
                             source.processor.serialize_plugin_state(),
                             0);
    };

    int migration_calls = 0;
    auto migration = [&migration_calls](std::span<const uint8_t> source_blob,
                                        std::vector<uint8_t>& migrated) {
        ++migration_calls;
        const auto marker = plugin_payload_string(source_blob);

        if (marker == "migration=false") {
            return false;
        }
        if (marker == "migration=empty") {
            return true;
        }
        if (marker == "migration=garbage") {
            migrated = {'N', 'O', 'P', 'E'};
            return true;
        }

        migrated.assign(source_blob.begin(), source_blob.end());
        if (marker == "migration=wrong-version") {
            rewrite_envelope_version_and_crc(migrated, 0);
            return true;
        }

        rewrite_envelope_version_and_crc(
            migrated, pulp::format::plugin_state_io::current_envelope_version());
        if (marker == "migration=bad-crc") {
            migrated.back() ^= 0x44u;
        }
        if (marker == "migration=size-mismatch") {
            migrated.insert(migrated.end() - 4, 0xAAu);
        }
        return true;
    };
    REQUIRE(register_envelope_migration(0, current_version, migration));
    REQUIRE_FALSE(register_envelope_migration(0, current_version, migration));

    auto envelope = make_legacy_envelope("view=legacy");
    TestRig restored;
    restored.store.set_value(1, 3.0f);
    restored.store.set_value(2, 17.0f);
    restored.processor.plugin_state = "stale";

    REQUIRE(deserialize(envelope, restored.store, restored.processor));
    REQUIRE_THAT(restored.store.get_value(1), WithinAbs(-8.0, 0.01));
    REQUIRE_THAT(restored.store.get_value(2), WithinAbs(61.0, 0.01));
    REQUIRE(restored.processor.plugin_state == "view=legacy");
    REQUIRE(restored.processor.deserialize_calls == 1);
    REQUIRE(restored.processor.last_payload ==
            std::vector<uint8_t>({'v', 'i', 'e', 'w', '=', 'l', 'e', 'g', 'a', 'c', 'y'}));
    REQUIRE(migration_calls == 1);

    source.processor.plugin_state = "view=current";
    auto current_envelope = make_envelope(source.store.serialize(),
                                          source.processor.serialize_plugin_state(),
                                          current_version);
    const auto calls_before_current = migration_calls;
    TestRig current_restored;
    current_restored.processor.plugin_state = "stale-current";

    REQUIRE(deserialize(current_envelope, current_restored.store,
                        current_restored.processor));
    REQUIRE(current_restored.processor.plugin_state == "view=current");
    REQUIRE(migration_calls == calls_before_current);

    auto corrupt_legacy_envelope = make_legacy_envelope("view=corrupt");
    corrupt_legacy_envelope.back() ^= 0x55u;
    const auto calls_before_corrupt = migration_calls;

    TestRig untouched;
    untouched.store.set_value(1, 4.0f);
    untouched.store.set_value(2, 23.0f);
    untouched.processor.plugin_state = "keep";

    REQUIRE_FALSE(deserialize(corrupt_legacy_envelope, untouched.store,
                              untouched.processor));
    REQUIRE_THAT(untouched.store.get_value(1), WithinAbs(4.0, 0.01));
    REQUIRE_THAT(untouched.store.get_value(2), WithinAbs(23.0, 0.01));
    REQUIRE(untouched.processor.plugin_state == "keep");
    REQUIRE(untouched.processor.deserialize_calls == 0);
    REQUIRE(migration_calls == calls_before_corrupt);

    auto expect_migration_failure = [&](const std::string& marker) {
        auto blob = make_legacy_envelope(marker);
        const auto calls_before = migration_calls;

        TestRig failed;
        failed.store.set_value(1, 7.0f);
        failed.store.set_value(2, 29.0f);
        failed.processor.plugin_state = "keep";

        REQUIRE_FALSE(deserialize(blob, failed.store, failed.processor));
        REQUIRE_THAT(failed.store.get_value(1), WithinAbs(7.0, 0.01));
        REQUIRE_THAT(failed.store.get_value(2), WithinAbs(29.0, 0.01));
        REQUIRE(failed.processor.plugin_state == "keep");
        REQUIRE(failed.processor.deserialize_calls == 0);
        REQUIRE(migration_calls == calls_before + 1);
    };

    expect_migration_failure("migration=false");
    expect_migration_failure("migration=empty");
    expect_migration_failure("migration=garbage");
    expect_migration_failure("migration=wrong-version");
    expect_migration_failure("migration=bad-crc");
    expect_migration_failure("migration=size-mismatch");
}

TEST_CASE("plugin_state_io rejects invalid envelope migration registrations",
          "[format][plugin-state][coverage][phase3]") {
    using pulp::format::plugin_state_io::current_envelope_version;
    using pulp::format::plugin_state_io::register_envelope_migration;

    const auto current = current_envelope_version();
    auto migration = [](std::span<const uint8_t> source,
                        std::vector<uint8_t>& migrated) {
        migrated.assign(source.begin(), source.end());
        return true;
    };

    REQUIRE_FALSE(register_envelope_migration(current, current, migration));
    REQUIRE_FALSE(register_envelope_migration(current, 0, migration));
    REQUIRE_FALSE(register_envelope_migration(0, current + 1, migration));
    REQUIRE_FALSE(register_envelope_migration(0, current, {}));
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

    SECTION("declared plugin payload size exceeds bytes available") {
        auto blob = make_envelope(legacy_blob);
        write_u32(blob, 12, 1024);

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
    REQUIRE(restored.processor.last_payload ==
            std::vector<uint8_t>({'k', 'e', 'e', 'p'}));
    REQUIRE(restored.processor.deserialize_calls == 2);
}
