#pragma once

#include <pulp/playback/capture_engine.hpp>
#include <pulp/runtime/result.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/model.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace pulp::playback {

struct MidiCaptureMaterializationConfig {
    const timebase::CompiledTempoMap* tempo_map = nullptr;
    timebase::SamplePosition placement_start;
    std::uint64_t frame_count = 0;
    timebase::TickDuration quantize_grid;
    timebase::TickDuration minimum_note_duration{1};
    std::uint64_t next_item_id = 1;
};

struct MaterializedMidiCapture {
    timebase::TickPosition clip_start;
    timeline::NoteContent notes;
    std::vector<CapturedMidiEvent> mpe_expression;
    std::uint64_t next_item_id = 1;
};

enum class MidiCaptureMaterializationError : std::uint8_t {
    InvalidConfig,
    IdentityExhausted,
    InvalidNotes,
};

runtime::Result<MaterializedMidiCapture, MidiCaptureMaterializationError>
materialize_midi_capture(std::span<const CapturedMidiEvent> events,
                         MidiCaptureMaterializationConfig config);

} // namespace pulp::playback
