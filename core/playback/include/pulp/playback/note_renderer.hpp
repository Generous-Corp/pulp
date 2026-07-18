#pragma once

#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>
#include <pulp/playback/stable_renderer_shell.hpp>
#include <pulp/playback/transport.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace pulp::playback {

namespace detail {

/// Computes a block-local offset without signed overflow. The subtraction is
/// only performed after establishing event >= range_start, so unsigned
/// subtraction is the exact non-negative distance across the full int64 domain.
constexpr bool note_event_offset_in_range(timebase::SamplePosition event,
                                          timebase::SamplePosition range_start,
                                          std::uint32_t frame_count,
                                          std::uint32_t& offset) noexcept {
    if (event < range_start) return false;
    const auto delta = static_cast<std::uint64_t>(event.value) -
                       static_cast<std::uint64_t>(range_start.value);
    if (delta >= frame_count) return false;
    offset = static_cast<std::uint32_t>(delta);
    return true;
}

} // namespace detail

enum class NoteRenderCode : std::uint8_t {
    Ok,
    NotPrepared,
    MissingProgram,
    AdoptionRejected,
    InvalidTransport,
    TempoMapMismatch,
    ActiveStateOverflow,
    OutputOverflow,
};

struct NoteRenderResult {
    NoteRenderCode code = NoteRenderCode::Ok;
    ShellAdoptionResult adoption = ShellAdoptionResult::Missing;
    std::uint32_t emitted_events = 0;
    std::uint32_t dropped_events = 0;
};

/// Phase-1 arrangement-note scheduler. This is an engine transport-tick lane,
/// not an audio CustomNode: it resolves immutable TrackProgram events against
/// the transport's one or two monotonic ranges and produces block-relative
/// MIDI for the graph/embedded adapter to consume.
///
/// prepare() is control-thread work. process() owns all mutable execution state
/// on one audio thread and is allocation-free after prepare. Phase 1 resets and
/// releases active notes on adoption, seek, loop wrap, and stop; it deliberately
/// does not chase notes whose onset precedes the new range.
class ArrangementNoteRenderer {
  public:
    /// Bounded logical overlap depth per MIDI channel/pitch. Exceeding it is a
    /// malformed/pathological stream and fails closed with an immediate flush.
    static constexpr std::uint16_t maximum_logical_overlap = 255;
    static constexpr audio::RtSafetyClass process_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;

    explicit ArrangementNoteRenderer(timeline::ItemId track_id) noexcept
        : shell_(track_id) {
        output_.attach_ump(&ump_output_);
    }

    bool prepare(std::size_t maximum_events_per_block);
    NoteRenderResult process(const PlaybackProgramBlock& block,
                             const TransportSnapshot& transport) noexcept;

    const midi::MidiBuffer& events() const noexcept { return output_; }
    RendererProgramKey active_key() const noexcept { return shell_.active_key(); }
    bool has_active_notes() const noexcept;

  private:
    static constexpr std::size_t kMidiKeyCount = 16u * 128u;

    bool emit(const NoteProgramEvent& event, std::uint32_t sample_offset) noexcept;
    bool append_physical(midi::MidiEvent midi1, midi::UmpPacket midi2,
                         std::uint32_t sample_offset) noexcept;
    bool flush(std::uint32_t sample_offset) noexcept;
    void record_output_drop() noexcept;
    void update_carry_state(const TransportSnapshot& transport,
                            std::int64_t event_cursor) noexcept;

    StableRendererShell shell_;
    midi::MidiBuffer output_;
    midi::UmpBuffer ump_output_;
    std::array<std::uint16_t, kMidiKeyCount> active_counts_{};
    bool prepared_ = false;
    bool pending_flush_ = false;
    bool state_overflow_ = false;
    bool has_block_index_ = false;
    std::uint64_t last_block_index_ = 0;
    std::uint32_t dropped_events_ = 0;
};

} // namespace pulp::playback
