#pragma once

#include <pulp/playback/compile_context_registry.hpp>
#include <pulp/timeline/schema_registry.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace pulp::playback {

inline constexpr const char* kChordPatternContentType = "pulp.playback.chord_pattern";
inline constexpr std::uint32_t kChordPatternContentSchemaVersion = 1;
inline constexpr std::size_t kMaximumChordPatternNotes = 4096;

/// Immutable authored settings for the deterministic chord-pattern compiler.
/// Octaves use MIDI naming: C-1 is note zero and C4 is note 60.
struct ChordPatternContent {
    std::uint64_t seed = 0;
    timebase::TickDuration step;
    timebase::TickDuration gate;
    std::int8_t octave = 4;
    std::uint16_t velocity = 0xffff;

    constexpr auto operator<=>(const ChordPatternContent&) const = default;
};

/// Registers the exact versioned Content schema and codec.
runtime::Result<timeline::SchemaRegistration, timeline::SchemaError>
register_chord_pattern_content_schema(timeline::SchemaRegistryBuilder& builder);

/// Creates schema-backed chord-pattern content after strict payload validation.
runtime::Result<timeline::RegisteredContent, timeline::PersistenceError>
create_chord_pattern_content(const ChordPatternContent& content,
                             const timeline::SchemaRegistry& schemas,
                             std::size_t maximum_json_bytes = 1024);

/// Declares the trusted deterministic note compiler and its ChordScale read.
std::optional<ContextRegistrationError>
declare_chord_pattern_renderer(CompileContextRegistry& registry,
                               const timeline::SchemaRegistry& schemas);

} // namespace pulp::playback
