#include <pulp/state/state_migration.hpp>

#include <choc/memory/choc_Endianness.h>

#include <cstddef>
#include <utility>

namespace pulp::state {
namespace {

bool has_state_magic(std::span<const uint8_t> source) {
    return source.size() >= 4
        && source[0] == 'P'
        && source[1] == 'U'
        && source[2] == 'L'
        && source[3] == 'P';
}

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

bool has_valid_state_crc(std::span<const uint8_t> source) {
    if (source.size() < 16 || !has_state_magic(source)) {
        return false;
    }

    const auto crc_offset = source.size() - 4;
    const uint32_t stored_crc =
        choc::memory::readLittleEndian<uint32_t>(source.data() + crc_offset);
    const uint32_t computed_crc = crc32_simple(source.data(), crc_offset);
    return stored_crc == computed_crc;
}

} // namespace

bool StateMigrationRegistry::register_migration(uint32_t from_version,
                                                uint32_t to_version,
                                                MigrationFn migration) {
    if (from_version >= to_version || !migration) {
        return false;
    }

    if (find(from_version) != nullptr) {
        return false;
    }

    migrations_.push_back({from_version, to_version, std::move(migration)});
    return true;
}

bool StateMigrationRegistry::has_migration_from(uint32_t from_version) const {
    return find(from_version) != nullptr;
}

std::optional<std::vector<uint8_t>>
StateMigrationRegistry::migrate(std::span<const uint8_t> source,
                                uint32_t target_version) const {
    auto version = serialized_state_version(source);
    if (!version.has_value()) {
        return std::nullopt;
    }
    if (!has_valid_state_crc(source)) {
        return std::nullopt;
    }

    if (*version == target_version) {
        return std::vector<uint8_t>(source.begin(), source.end());
    }
    if (*version > target_version) {
        return std::nullopt;
    }

    std::vector<uint8_t> current(source.begin(), source.end());
    while (*version != target_version) {
        const Entry* entry = find(*version);
        if (entry == nullptr) {
            return std::nullopt;
        }

        if (entry->to_version <= *version || entry->to_version > target_version) {
            return std::nullopt;
        }

        std::vector<uint8_t> next;
        if (!entry->migration(current, next) || next.empty()) {
            return std::nullopt;
        }

        auto next_version = serialized_state_version(next);
        if (!next_version.has_value() || *next_version != entry->to_version) {
            return std::nullopt;
        }

        current = std::move(next);
        version = next_version;
    }

    return current;
}

const StateMigrationRegistry::Entry*
StateMigrationRegistry::find(uint32_t from_version) const {
    for (const auto& entry : migrations_) {
        if (entry.from_version == from_version) {
            return &entry;
        }
    }
    return nullptr;
}

std::optional<uint32_t>
serialized_state_version(std::span<const uint8_t> source) {
    if (source.size() < 12 || !has_state_magic(source)) {
        return std::nullopt;
    }

    return choc::memory::readLittleEndian<uint32_t>(source.data() + 4);
}

} // namespace pulp::state
