#include <pulp/format/plugin_state_io.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/runtime/assert.hpp>
#include <pulp/runtime/exceptions.hpp>
#include <pulp/state/store.hpp>
#include <choc/memory/choc_Endianness.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
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

std::vector<uint8_t> safe_serialize_plugin_state(const Processor& processor) noexcept {
    PULP_TRY {
        return processor.serialize_plugin_state();
    } PULP_CATCH_ALL {
        return {};
    }
}

bool safe_deserialize_plugin_state(Processor& processor,
                                   std::span<const uint8_t> bytes) noexcept {
    PULP_TRY {
        return processor.deserialize_plugin_state(bytes);
    } PULP_CATCH_ALL {
        return false;
    }
}

struct EnvelopeSizes {
    uint32_t store_size = 0;
    uint32_t plugin_size = 0;
    std::size_t payload_size = 0;
    std::size_t expected_size = 0;
    std::size_t crc_offset = 0;
};

bool read_envelope_sizes(std::span<const uint8_t> bytes,
                         EnvelopeSizes& sizes) noexcept {
    if (bytes.size() < (kEnvelopeHeaderSize + kEnvelopeFooterSize)) {
        return false;
    }

    sizes.store_size = choc::memory::readLittleEndian<uint32_t>(bytes.data() + 8);
    sizes.plugin_size = choc::memory::readLittleEndian<uint32_t>(bytes.data() + 12);

    const auto max_size = std::numeric_limits<std::size_t>::max();
    const auto store_size = static_cast<std::size_t>(sizes.store_size);
    const auto plugin_size = static_cast<std::size_t>(sizes.plugin_size);
    if (plugin_size > max_size - store_size) {
        return false;
    }

    sizes.payload_size = store_size + plugin_size;
    const auto available_payload = bytes.size() - kEnvelopeHeaderSize - kEnvelopeFooterSize;
    if (sizes.payload_size > available_payload) {
        return false;
    }

    if (sizes.payload_size > max_size - kEnvelopeHeaderSize - kEnvelopeFooterSize) {
        return false;
    }
    sizes.expected_size = kEnvelopeHeaderSize + sizes.payload_size + kEnvelopeFooterSize;
    if (bytes.size() != sizes.expected_size) {
        return false;
    }

    sizes.crc_offset = kEnvelopeHeaderSize + sizes.payload_size;
    return true;
}

struct EnvelopeMigrationEntry {
    uint32_t from_version = 0;
    uint32_t to_version = 0;
    EnvelopeMigrationFn migration;
};

std::vector<EnvelopeMigrationEntry>& envelope_migrations() {
    static std::vector<EnvelopeMigrationEntry> migrations;
    return migrations;
}

std::mutex& envelope_migrations_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::optional<EnvelopeMigrationEntry> find_envelope_migration(uint32_t from_version) {
    std::lock_guard<std::mutex> lock(envelope_migrations_mutex());
    for (const auto& entry : envelope_migrations()) {
        if (entry.from_version == from_version) {
            return entry;
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> find_envelope_migration_target(uint32_t from_version) noexcept {
    PULP_TRY {
        std::lock_guard<std::mutex> lock(envelope_migrations_mutex());
        for (const auto& entry : envelope_migrations()) {
            if (entry.from_version == from_version) {
                return entry.to_version;
            }
        }
    } PULP_CATCH_ALL {
        return std::nullopt;
    }
    return std::nullopt;
}

bool envelope_migration_chain_available(uint32_t from_version) noexcept {
    auto version = from_version;
    while (version < kEnvelopeVersion) {
        auto to_version = find_envelope_migration_target(version);
        if (!to_version.has_value() ||
            *to_version <= version ||
            *to_version > kEnvelopeVersion) {
            return false;
        }
        version = *to_version;
    }
    return version == kEnvelopeVersion;
}

std::optional<uint32_t> envelope_version(std::span<const uint8_t> bytes) {
    if (!has_magic(bytes, kEnvelopeMagic)
        || bytes.size() < (kEnvelopeHeaderSize + kEnvelopeFooterSize)) {
        return std::nullopt;
    }
    return choc::memory::readLittleEndian<uint32_t>(bytes.data() + 4);
}

bool has_valid_envelope_crc(std::span<const uint8_t> bytes) {
    if (!has_magic(bytes, kEnvelopeMagic)) {
        return false;
    }

    EnvelopeSizes sizes;
    if (!read_envelope_sizes(bytes, sizes)) {
        return false;
    }

    const uint32_t stored_crc =
        choc::memory::readLittleEndian<uint32_t>(bytes.data() + sizes.crc_offset);
    const uint32_t computed_crc = crc32_simple(bytes.data(), sizes.crc_offset);
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
        auto migration = find_envelope_migration(*version);
        if (!migration.has_value()
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

    EnvelopeSizes sizes;
    if (!read_envelope_sizes(bytes, sizes)) {
        return false;
    }

    const uint32_t stored_crc =
        choc::memory::readLittleEndian<uint32_t>(bytes.data() + sizes.crc_offset);
    const uint32_t computed_crc = crc32_simple(bytes.data(), sizes.crc_offset);
    if (stored_crc != computed_crc) {
        return false;
    }

    parsed.store_blob = bytes.subspan(kEnvelopeHeaderSize, sizes.store_size);
    parsed.plugin_blob = bytes.subspan(kEnvelopeHeaderSize + sizes.store_size,
                                       sizes.plugin_size);
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

    const bool plugin_restored = safe_deserialize_plugin_state(processor, plugin_blob);
    // Author restore code may reject or throw for both the requested payload
    // and the rollback payload. Never turn that into a host-process abort; the
    // StateStore has still been restored and the adapter reports failure.
    return plugin_restored;
}

} // namespace

uint32_t current_envelope_version() {
    return kEnvelopeVersion;
}

const char* restore_diagnostic_status_name(RestoreDiagnosticStatus status) noexcept {
    switch (status) {
        case RestoreDiagnosticStatus::ok: return "ok";
        case RestoreDiagnosticStatus::empty_blob: return "empty_blob";
        case RestoreDiagnosticStatus::unknown_format: return "unknown_format";
        case RestoreDiagnosticStatus::truncated_envelope: return "truncated_envelope";
        case RestoreDiagnosticStatus::unsupported_envelope_version: return "unsupported_envelope_version";
        case RestoreDiagnosticStatus::envelope_migration_unavailable: return "envelope_migration_unavailable";
        case RestoreDiagnosticStatus::payload_size_mismatch: return "payload_size_mismatch";
        case RestoreDiagnosticStatus::crc_mismatch: return "crc_mismatch";
        case RestoreDiagnosticStatus::empty_store_payload: return "empty_store_payload";
        case RestoreDiagnosticStatus::total_size_budget_exceeded: return "total_size_budget_exceeded";
        case RestoreDiagnosticStatus::store_size_budget_exceeded: return "store_size_budget_exceeded";
        case RestoreDiagnosticStatus::plugin_size_budget_exceeded: return "plugin_size_budget_exceeded";
    }
    return "unknown";
}

RestoreDiagnostics inspect_restore_blob(
    std::span<const uint8_t> bytes,
    const RestoreDiagnosticPolicy& policy) noexcept {
    RestoreDiagnostics diagnostics;
    diagnostics.total_bytes = static_cast<uint64_t>(bytes.size());
    diagnostics.large_state = policy.large_state_threshold_bytes > 0 &&
        diagnostics.total_bytes > policy.large_state_threshold_bytes;

    if (bytes.empty()) {
        diagnostics.status = RestoreDiagnosticStatus::empty_blob;
        return diagnostics;
    }
    if (policy.max_total_bytes > 0 && diagnostics.total_bytes > policy.max_total_bytes) {
        diagnostics.status = RestoreDiagnosticStatus::total_size_budget_exceeded;
        return diagnostics;
    }

    auto apply_budget = [&]() noexcept {
        if (policy.max_total_bytes > 0 && diagnostics.total_bytes > policy.max_total_bytes) {
            diagnostics.status = RestoreDiagnosticStatus::total_size_budget_exceeded;
            return;
        }
        if (policy.max_store_bytes > 0 && diagnostics.store_bytes > policy.max_store_bytes) {
            diagnostics.status = RestoreDiagnosticStatus::store_size_budget_exceeded;
            return;
        }
        if (policy.max_plugin_bytes > 0 && diagnostics.plugin_bytes > policy.max_plugin_bytes) {
            diagnostics.status = RestoreDiagnosticStatus::plugin_size_budget_exceeded;
            return;
        }
        diagnostics.status = RestoreDiagnosticStatus::ok;
    };

    if (has_magic(bytes, kStateStoreMagic)) {
        diagnostics.format = RestoreBlobFormat::legacy_state_store;
        diagnostics.store_bytes = diagnostics.total_bytes;
        apply_budget();
        return diagnostics;
    }

    if (!has_magic(bytes, kEnvelopeMagic)) {
        diagnostics.status = RestoreDiagnosticStatus::unknown_format;
        return diagnostics;
    }

    diagnostics.format = RestoreBlobFormat::envelope;
    if (bytes.size() < (kEnvelopeHeaderSize + kEnvelopeFooterSize)) {
        diagnostics.status = RestoreDiagnosticStatus::truncated_envelope;
        return diagnostics;
    }

    diagnostics.envelope_version = choc::memory::readLittleEndian<uint32_t>(bytes.data() + 4);
    diagnostics.requires_envelope_migration = diagnostics.envelope_version < kEnvelopeVersion;
    if (diagnostics.envelope_version > kEnvelopeVersion) {
        diagnostics.status = RestoreDiagnosticStatus::unsupported_envelope_version;
        return diagnostics;
    }

    EnvelopeSizes sizes;
    if (!read_envelope_sizes(bytes, sizes)) {
        diagnostics.status = RestoreDiagnosticStatus::payload_size_mismatch;
        return diagnostics;
    }

    diagnostics.store_bytes = sizes.store_size;
    diagnostics.plugin_bytes = sizes.plugin_size;
    diagnostics.requires_plugin_restore = sizes.plugin_size > 0;
    diagnostics.may_require_resource_relink = sizes.plugin_size > 0;

    const uint32_t stored_crc =
        choc::memory::readLittleEndian<uint32_t>(bytes.data() + sizes.crc_offset);
    const uint32_t computed_crc = crc32_simple(bytes.data(), sizes.crc_offset);
    if (stored_crc != computed_crc) {
        diagnostics.status = RestoreDiagnosticStatus::crc_mismatch;
        return diagnostics;
    }

    if (diagnostics.store_bytes == 0) {
        diagnostics.status = RestoreDiagnosticStatus::empty_store_payload;
        return diagnostics;
    }

    if (diagnostics.requires_envelope_migration &&
        !envelope_migration_chain_available(diagnostics.envelope_version)) {
        diagnostics.status = RestoreDiagnosticStatus::envelope_migration_unavailable;
        return diagnostics;
    }

    apply_budget();
    return diagnostics;
}

bool register_envelope_migration(uint32_t from_version,
                                 uint32_t to_version,
                                 EnvelopeMigrationFn migration) {
    if (from_version >= to_version
        || to_version > kEnvelopeVersion
        || !migration) {
        return false;
    }

    std::lock_guard<std::mutex> lock(envelope_migrations_mutex());
    for (const auto& entry : envelope_migrations()) {
        if (entry.from_version == from_version) {
            return false;
        }
    }

    envelope_migrations().push_back({from_version, to_version, std::move(migration)});
    return true;
}

std::vector<uint8_t> serialize(const state::StateStore& store,
                               const Processor& processor) {
    auto store_blob = store.serialize();
    auto published = processor.published_plugin_state_snapshot();
    auto plugin_blob = published ? *published : safe_serialize_plugin_state(processor);
    if (plugin_blob.empty()) {
        return store_blob;
    }

    const auto max_u32 = static_cast<std::size_t>(std::numeric_limits<uint32_t>::max());
    if (store_blob.size() > max_u32 || plugin_blob.size() > max_u32) {
        return {};
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
    const auto previous_plugin_blob = safe_serialize_plugin_state(processor);

    if (!store.deserialize(parsed.store_blob)) {
        restore_previous_state(previous_store_blob, previous_plugin_blob,
                               store, processor);
        return false;
    }

    if (safe_deserialize_plugin_state(processor, parsed.plugin_blob)) {
        return true;
    }

    restore_previous_state(previous_store_blob, previous_plugin_blob,
                           store, processor);
    return false;
}

} // namespace pulp::format::plugin_state_io
