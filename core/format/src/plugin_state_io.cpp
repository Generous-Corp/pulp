#include <pulp/format/plugin_state_io.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/runtime/assert.hpp>
#include <pulp/state/store.hpp>
#include <choc/memory/choc_Endianness.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace pulp::format::plugin_state_io {
namespace {

constexpr uint8_t kStateStoreMagic[4] = {'P', 'U', 'L', 'P'};
constexpr uint8_t kEnvelopeMagic[4] = {'P', 'L', 'S', 'T'};
constexpr uint32_t kEnvelopeVersion = 1;
constexpr std::size_t kEnvelopeHeaderSize = 16;
constexpr std::size_t kEnvelopeFooterSize = 4;

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

void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    uint8_t bytes[4];
    choc::memory::writeLittleEndian(bytes, value);
    out.insert(out.end(), bytes, bytes + sizeof(bytes));
}

bool has_magic(std::span<const uint8_t> data, const uint8_t (&magic)[4]) {
    return data.size() >= 4
        && data[0] == magic[0]
        && data[1] == magic[1]
        && data[2] == magic[2]
        && data[3] == magic[3];
}

void clone_schema(const state::StateStore& source, state::StateStore& dest) {
    dest.set_state_version(source.state_version());
    for (const auto& group : source.all_groups()) {
        dest.add_group(group);
    }
    for (const auto& param : source.all_params()) {
        dest.add_parameter(param);
    }
    dest.copy_state_migrations_from(source);
}

struct ParsedBlob {
    std::span<const uint8_t> store_blob;
    std::span<const uint8_t> plugin_blob;
};

struct EnvelopeMigrationEntry {
    uint32_t from_version = 0;
    uint32_t to_version = 0;
    EnvelopeMigrationFn migration;
};

std::vector<EnvelopeMigrationEntry>& envelope_migrations() {
    static std::vector<EnvelopeMigrationEntry> migrations;
    return migrations;
}

const EnvelopeMigrationEntry* find_envelope_migration(uint32_t from_version) {
    for (const auto& entry : envelope_migrations()) {
        if (entry.from_version == from_version) {
            return &entry;
        }
    }
    return nullptr;
}

std::optional<uint32_t> envelope_version(std::span<const uint8_t> bytes) {
    if (!has_magic(bytes, kEnvelopeMagic)
        || bytes.size() < (kEnvelopeHeaderSize + kEnvelopeFooterSize)) {
        return std::nullopt;
    }
    return choc::memory::readLittleEndian<uint32_t>(bytes.data() + 4);
}

bool has_valid_envelope_crc(std::span<const uint8_t> bytes) {
    if (!has_magic(bytes, kEnvelopeMagic)
        || bytes.size() < (kEnvelopeHeaderSize + kEnvelopeFooterSize)) {
        return false;
    }

    const uint32_t store_size =
        choc::memory::readLittleEndian<uint32_t>(bytes.data() + 8);
    const uint32_t plugin_size =
        choc::memory::readLittleEndian<uint32_t>(bytes.data() + 12);
    const std::size_t payload_size =
        static_cast<std::size_t>(store_size) + static_cast<std::size_t>(plugin_size);

    if (payload_size > (bytes.size() - kEnvelopeHeaderSize - kEnvelopeFooterSize)) {
        return false;
    }

    const std::size_t expected_size =
        kEnvelopeHeaderSize + payload_size + kEnvelopeFooterSize;
    if (bytes.size() != expected_size) {
        return false;
    }

    const std::size_t crc_offset = kEnvelopeHeaderSize + payload_size;
    const uint32_t stored_crc =
        choc::memory::readLittleEndian<uint32_t>(bytes.data() + crc_offset);
    const uint32_t computed_crc = crc32_simple(bytes.data(), crc_offset);
    return stored_crc == computed_crc;
}

bool migrate_envelope_to_current(std::span<const uint8_t> bytes,
                                 std::vector<uint8_t>& migrated_storage) {
    auto version = envelope_version(bytes);
    if (!version.has_value() || *version > kEnvelopeVersion) {
        return false;
    }
    if (!has_valid_envelope_crc(bytes)) {
        return false;
    }
    if (*version == kEnvelopeVersion) {
        migrated_storage.assign(bytes.begin(), bytes.end());
        return true;
    }

    std::vector<uint8_t> current(bytes.begin(), bytes.end());
    while (*version != kEnvelopeVersion) {
        const auto* migration = find_envelope_migration(*version);
        if (migration == nullptr
            || migration->to_version <= *version
            || migration->to_version > kEnvelopeVersion) {
            return false;
        }

        std::vector<uint8_t> next;
        if (!migration->migration(current, next) || next.empty()) {
            return false;
        }

        auto next_version = envelope_version(next);
        if (!next_version.has_value() || *next_version != migration->to_version) {
            return false;
        }

        current = std::move(next);
        version = next_version;
    }

    migrated_storage = std::move(current);
    return true;
}

bool parse_blob(std::span<const uint8_t> bytes,
                ParsedBlob& parsed,
                std::vector<uint8_t>& migrated_storage) {
    if (has_magic(bytes, kStateStoreMagic)) {
        parsed.store_blob = bytes;
        parsed.plugin_blob = {};
        return true;
    }

    if (!has_magic(bytes, kEnvelopeMagic)) {
        return false;
    }

    if (bytes.size() < (kEnvelopeHeaderSize + kEnvelopeFooterSize)) {
        return false;
    }

    const uint32_t version =
        choc::memory::readLittleEndian<uint32_t>(bytes.data() + 4);
    if (version > kEnvelopeVersion) {
        return false;
    }
    if (version < kEnvelopeVersion) {
        if (!migrate_envelope_to_current(bytes, migrated_storage)) {
            return false;
        }
        bytes = migrated_storage;
    }

    const uint32_t readable_version =
        choc::memory::readLittleEndian<uint32_t>(bytes.data() + 4);
    if (readable_version != kEnvelopeVersion) {
        return false;
    }

    const uint32_t store_size =
        choc::memory::readLittleEndian<uint32_t>(bytes.data() + 8);
    const uint32_t plugin_size =
        choc::memory::readLittleEndian<uint32_t>(bytes.data() + 12);
    const std::size_t payload_size =
        static_cast<std::size_t>(store_size) + static_cast<std::size_t>(plugin_size);

    if (payload_size > (bytes.size() - kEnvelopeHeaderSize - kEnvelopeFooterSize)) {
        return false;
    }

    const std::size_t expected_size =
        kEnvelopeHeaderSize + payload_size + kEnvelopeFooterSize;
    if (bytes.size() != expected_size) {
        return false;
    }

    const std::size_t crc_offset = kEnvelopeHeaderSize + payload_size;
    const uint32_t stored_crc =
        choc::memory::readLittleEndian<uint32_t>(bytes.data() + crc_offset);
    const uint32_t computed_crc = crc32_simple(bytes.data(), crc_offset);
    if (stored_crc != computed_crc) {
        return false;
    }

    parsed.store_blob = bytes.subspan(kEnvelopeHeaderSize, store_size);
    parsed.plugin_blob = bytes.subspan(kEnvelopeHeaderSize + store_size, plugin_size);
    return true;
}

bool restore_previous_state(const std::vector<uint8_t>& store_blob,
                            const std::vector<uint8_t>& plugin_blob,
                            state::StateStore& store,
                            Processor& processor) {
    const bool store_restored = store.deserialize(store_blob);
    PULP_ASSERT(store_restored, "StateStore rollback must succeed");
    if (!store_restored) {
        return false;
    }

    const bool plugin_restored = processor.deserialize_plugin_state(plugin_blob);
    PULP_ASSERT(plugin_restored, "Processor plugin-state rollback must succeed");
    return plugin_restored;
}

} // namespace

uint32_t current_envelope_version() {
    return kEnvelopeVersion;
}

bool register_envelope_migration(uint32_t from_version,
                                 uint32_t to_version,
                                 EnvelopeMigrationFn migration) {
    if (from_version >= to_version
        || to_version > kEnvelopeVersion
        || !migration) {
        return false;
    }
    if (find_envelope_migration(from_version) != nullptr) {
        return false;
    }

    envelope_migrations().push_back({from_version, to_version, std::move(migration)});
    return true;
}

std::vector<uint8_t> serialize(const state::StateStore& store,
                               const Processor& processor) {
    auto store_blob = store.serialize();
    auto plugin_blob = processor.serialize_plugin_state();
    if (plugin_blob.empty()) {
        return store_blob;
    }

    std::vector<uint8_t> out;
    out.reserve(kEnvelopeHeaderSize + store_blob.size()
                + plugin_blob.size() + kEnvelopeFooterSize);

    out.insert(out.end(), kEnvelopeMagic, kEnvelopeMagic + 4);
    append_u32(out, kEnvelopeVersion);
    append_u32(out, static_cast<uint32_t>(store_blob.size()));
    append_u32(out, static_cast<uint32_t>(plugin_blob.size()));
    out.insert(out.end(), store_blob.begin(), store_blob.end());
    out.insert(out.end(), plugin_blob.begin(), plugin_blob.end());
    append_u32(out, crc32_simple(out.data(), out.size()));
    return out;
}

bool deserialize(std::span<const uint8_t> bytes,
                 state::StateStore& store,
                 Processor& processor) {
    ParsedBlob parsed{};
    std::vector<uint8_t> migrated_storage;
    if (!parse_blob(bytes, parsed, migrated_storage)) {
        return false;
    }

    state::StateStore probe;
    clone_schema(store, probe);
    if (!probe.deserialize(parsed.store_blob)) {
        return false;
    }

    const auto previous_store_blob = store.serialize();
    const auto previous_plugin_blob = processor.serialize_plugin_state();

    if (!store.deserialize(parsed.store_blob)) {
        restore_previous_state(previous_store_blob, previous_plugin_blob,
                               store, processor);
        return false;
    }

    if (processor.deserialize_plugin_state(parsed.plugin_blob)) {
        return true;
    }

    restore_previous_state(previous_store_blob, previous_plugin_blob,
                           store, processor);
    return false;
}

} // namespace pulp::format::plugin_state_io
