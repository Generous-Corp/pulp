#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace pulp::state {

/// Offline migration support for serialized StateStore payloads.
///
/// Migration callbacks run on load/save infrastructure, never on the audio
/// thread. A callback receives the complete source blob and must write a
/// complete destination blob, including any format header/footer checksums.
class StateMigrationRegistry {
public:
    using MigrationFn =
        std::function<bool(std::span<const uint8_t> source,
                           std::vector<uint8_t>& destination)>;

    bool register_migration(uint32_t from_version,
                            uint32_t to_version,
                            MigrationFn migration);

    [[nodiscard]] bool has_migration_from(uint32_t from_version) const;

    [[nodiscard]] std::optional<std::vector<uint8_t>>
    migrate(std::span<const uint8_t> source, uint32_t target_version) const;

private:
    struct Entry {
        uint32_t from_version = 0;
        uint32_t to_version = 0;
        MigrationFn migration;
    };

    const Entry* find(uint32_t from_version) const;

    std::vector<Entry> migrations_;
};

[[nodiscard]] std::optional<uint32_t>
serialized_state_version(std::span<const uint8_t> source);

} // namespace pulp::state
