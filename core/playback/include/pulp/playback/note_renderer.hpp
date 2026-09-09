#pragma once

#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>
#include <pulp/playback/event_compensation.hpp>
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
    /// A compensating shift met a range that locates events by host beat. The
    /// shift is a document-sample quantity and that range has no document-sample
    /// origin to add it to, so the block fails closed rather than scheduling
    /// against a position nothing defined.
    CompensationUnsupported,
    /// A compensating shift would read past an enabled loop's end. What belongs
    /// in that window is the content after the wrap, not the document positions
    /// past the loop point, and wrap-aware read-ahead is not implemented. The
    /// block fails closed rather than playing events the pass will never reach.
    CompensationLoopWrapUnsupported,
};

struct NoteRenderResult {
    NoteRenderCode code = NoteRenderCode::Ok;
    ShellAdoptionResult adoption = ShellAdoptionResult::Missing;
    std::uint32_t emitted_events = 0;
    std::uint32_t dropped_events = 0;
    /// The shift this block actually scheduled against, which is the latched
    /// value and not necessarily the one the caller requested.
    EventCompensationShift applied_shift{};
    /// True while a requested shift differs from the latched one and playback
    /// is holding the older alignment until the transport stops.
    bool shift_relatch_pending = false;
};

/// Arrangement-note scheduler for the engine transport-tick lane. It is
/// not an audio CustomNode: it resolves immutable TrackProgram events against
/// the transport's one or two monotonic ranges and produces block-relative
/// MIDI for the graph/embedded adapter to consume.
///
/// prepare() is control-thread work. process() owns all mutable execution state
/// on one audio thread and is allocation-free after prepare. Adoption, seek,
/// loop wrap, and stop reset and release active notes; notes whose onset
/// precedes the new range are deliberately not chased. With a compensating
/// shift the non-chase rule applies to the SHIFTED range, so reading ahead does
/// not turn a seek into a chase.
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
    /// Schedules against `requested_shift` samples ahead of each transport
    /// range, so a chain that delays the events themselves still lands them on
    /// the authored sample.
    ///
    /// The value is prepared by the caller on the control thread and is
    /// constant for the block. A CHANGED value is adopted only while the
    /// transport is stopped: re-aligning mid-playback would move every
    /// subsequent event by the delta, which is audible as a jump in a stream
    /// the musician is listening to. Until then the older alignment is held and
    /// the result reports the relatch as pending.
    NoteRenderResult process(const PlaybackProgramBlock& block,
                             const TransportSnapshot& transport,
                             EventCompensationShift requested_shift) noexcept;
    /// The shift currently latched for scheduling.
    EventCompensationShift applied_shift() const noexcept {
        return latched_shift_;
    }

    const midi::MidiBuffer& events() const noexcept { return output_; }
    RendererProgramKey active_key() const noexcept { return shell_.active_key(); }
    bool has_active_notes() const noexcept;
    void reset() noexcept;

  private:
    static constexpr std::size_t kMidiKeyCount = 16u * 128u;

    NoteRenderResult process_shifted(const PlaybackProgramBlock& block,
                                     const TransportSnapshot& transport,
                                     EventCompensationShift shift) noexcept;
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
    bool has_latched_shift_ = false;
    EventCompensationShift latched_shift_{};
    std::uint64_t last_block_index_ = 0;
    std::uint32_t dropped_events_ = 0;
};

} // namespace pulp::playback
