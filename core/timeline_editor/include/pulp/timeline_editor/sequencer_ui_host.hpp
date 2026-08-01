#pragma once

#include <cstdint>
#include <type_traits>

#include <pulp/timebase/tick.hpp>
#include <pulp/timeline/item_id.hpp>

/// SequencerUiHost is the whole coupling a timeline editor view has toward
/// playback. A view reads where the playhead is, asks to hear something, and
/// hands out the intents a gesture produced; it never links, includes, or names
/// a playback type to do any of it. The interface is declared here, beside the
/// editor, and implemented by whoever owns audio — the product shell, or a
/// plugin that drives its own engine and wants a piano roll over it.
///
/// Two properties carry the contract, and both are mechanically checked:
///
///   - EVERYTHING CROSSES BY VALUE. A playhead reading is a self-contained
///     value with no pointer into engine-owned storage, so a program swap on
///     the audio side cannot dangle or tear a reading the view already holds.
///     `TransportSnapshot` is the counterexample this exists to avoid: it
///     carries a `const CompiledTempoMap*` borrowed from the compiled program,
///     which is correct for a block renderer and fatal for a view that keeps a
///     copy across a frame.
///   - THE VOCABULARY IS DOCUMENT-SIDE. Positions are `timebase` ticks and
///     subjects are `timeline::ItemId`s — the same identities the document
///     model and the edit commands already use. Nothing here describes how
///     audio is produced.
///
/// Structure, following the StepGridViewBase/StepGridViewT split:
///   - SequencerUiHost — non-template. Playhead snapshots and audition, both
///     independent of how edits are expressed.
///   - SequencerUiHostT<Intent> — thin templated shim adding intent submission
///     for one intent vocabulary. Edit intents are the editor's own type; the
///     host is only their destination, so parameterizing here keeps the intent
///     vocabulary and the playback seam free to evolve apart.
///
/// A host is driven from the UI/message thread. Implementations bridge to the
/// audio thread themselves (SeqLock for the playhead, an SPSC queue for
/// audition and intents is the shape that already works elsewhere); this
/// interface deliberately says nothing about that bridge, so a host backed by a
/// real transport and a host backed by a scripted value are interchangeable.
namespace pulp::timeline_editor {

/// Whether the playhead is moving, and why. A scrub moves the playhead while
/// the musical transport is stopped, so a view that only asks "does the ruler
/// need repainting" reads `moving()` rather than branching on the state.
enum class UiTransportState : std::uint8_t {
    Stopped,
    Playing,
    Scrubbing,
};

/// The loop the transport is honouring, in document ticks. Disabled loops keep
/// their bounds so a view can keep drawing the region a user has set up.
struct UiLoopRegion {
    bool enabled = false;
    timebase::TickPosition start{};
    timebase::TickPosition end{};

    constexpr bool operator==(const UiLoopRegion&) const = default;
};

/// One complete reading of where playback is.
///
/// Every field is a value. Holding a UiPlayhead across a program swap, a seek,
/// or the destruction of the host's engine is defined: the reading goes stale,
/// never invalid. `program_generation` is how a view tells stale from live
/// without holding anything a swap can invalidate — it changes on every program
/// adoption, so a view comparing two readings knows whether they describe the
/// same compiled world.
struct UiPlayhead {
    /// Identity of the compiled program this reading came from. Monotonic; a
    /// change means the engine adopted a different program between readings.
    std::uint64_t program_generation = 0;
    /// Monotonic per publish. Two readings with the same value are the same
    /// reading, which is how a view skips redundant repaints.
    std::uint64_t sequence = 0;
    /// Document position. This is the value a ruler and a playhead line draw.
    timebase::TickPosition position{};
    UiLoopRegion loop{};
    UiTransportState state = UiTransportState::Stopped;
    double tempo_bpm = 120.0;

    /// Whether the position advances on its own. True while scrubbing, because
    /// a scrub moves the playhead even though the musical transport is stopped.
    constexpr bool moving() const noexcept {
        return state == UiTransportState::Playing || state == UiTransportState::Scrubbing;
    }

    constexpr bool operator==(const UiPlayhead&) const = default;
};

static_assert(std::is_trivially_copyable_v<UiPlayhead>,
              "UiPlayhead must be trivially copyable: a reading a view retains "
              "must survive the engine that produced it");

/// What a view asks to hear, in document terms only.
///
/// This is the piano-roll click, the keyboard press, the drag onto a new pitch.
/// It says which note, how hard, and on whose behalf — never which device,
/// voice, or graph node sounds it. Resolving `track` to something that makes
/// noise is the host's whole job, and the reason the view does not need one.
struct AuditionRequest {
    /// Track whose device chain should sound this. A null id asks the host to
    /// use its own default routing, which is what a host with a single
    /// instrument and no track model wants.
    timeline::ItemId track{};
    /// Content item the request came from, when there is one. Carried so a host
    /// can apply the item's own transposition or channel mapping; a null id is
    /// a request that belongs to no item, such as a keyboard press.
    timeline::ItemId item{};
    /// Pitch, velocity, and channel exactly as timeline::NoteEvent spells them,
    /// so a request built from a note in the document needs no conversion and
    /// no second velocity scale. Velocity is full-range: 0xffff is full scale.
    std::uint8_t pitch = 60;
    std::uint16_t velocity = 0xffff;
    std::uint8_t channel = 0;
    /// Zero requests a sustained audition, ended by end_audition(). Nonzero
    /// requests a one-shot of that musical length, which the host ends itself.
    timebase::TickDuration duration{};

    constexpr bool operator==(const AuditionRequest&) const = default;
};

static_assert(std::is_trivially_copyable_v<AuditionRequest>,
              "AuditionRequest must be trivially copyable: it crosses to the "
              "host by value and may be queued to an audio thread");

/// Names a sustained audition so it can be ended. Zero is the null sentinel,
/// which is what a host that did not start anything returns.
struct AuditionHandle {
    std::uint64_t value = 0;

    constexpr bool valid() const noexcept { return value != 0; }
    constexpr bool operator==(const AuditionHandle&) const = default;
};

enum class AuditionStatus : std::uint8_t {
    /// The host is sounding it. A sustained request also yields a valid handle.
    Started,
    /// This host does not audition at all. A view must stay fully usable.
    Unsupported,
    /// The host is at its audition capacity and dropped this one.
    Busy,
    /// The request named something the host could not route or sound.
    Rejected,
};

/// Returned by value so a view driven by a host that cannot audition is
/// observable in a test without a mock — the same reason StepGridViewBase
/// returns PumpResult rather than relying on an unobservable side effect.
struct AuditionResult {
    AuditionStatus status = AuditionStatus::Unsupported;
    AuditionHandle handle{};

    constexpr bool operator==(const AuditionResult&) const = default;
};

enum class IntentStatus : std::uint8_t {
    /// The host took the intent and will apply it.
    Accepted,
    /// The host is not accepting intents right now — mid-adoption, or read
    /// only. A view retries or drops; it never treats this as an error.
    Deferred,
    /// The host understood the intent and refused it.
    Rejected,
};

struct IntentResult {
    IntentStatus status = IntentStatus::Deferred;
    /// Host-assigned identity of an accepted intent, so a view can correlate
    /// what it emitted with what later comes back as applied. Zero when the
    /// intent was not accepted.
    std::uint64_t sequence = 0;

    constexpr bool operator==(const IntentResult&) const = default;
};

/// The vocabulary-independent half of the seam: where playback is, and asking
/// to hear something.
///
/// Every method is noexcept: a host is called from inside a gesture and from
/// inside a paint, and the modules on either side of it (timeline, timebase)
/// compile without exceptions. A host that needs to allocate or fail does it
/// off this path and reports the outcome in the returned status.
class SequencerUiHost {
  public:
    virtual ~SequencerUiHost() = default;

    /// The latest playhead reading, by value. Cheap enough to call once per
    /// frame; implementations publish through a lock-free primitive rather than
    /// blocking the caller.
    virtual UiPlayhead playhead() const noexcept = 0;

    /// Ask to hear something. A host that does not audition returns
    /// Unsupported and does nothing else, which is a complete implementation.
    virtual AuditionResult begin_audition(const AuditionRequest& request) noexcept = 0;

    /// End a sustained audition. Ending an already-ended or never-started
    /// handle is a no-op, so a view may end unconditionally on mouse-up.
    virtual void end_audition(AuditionHandle handle) noexcept = 0;
};

/// Adds intent submission for one intent vocabulary. Intents are the editor's
/// type, not the host's; the host is only where they go.
template <class Intent>
class SequencerUiHostT : public SequencerUiHost {
  public:
    using IntentType = Intent;

    /// Hand over one intent a gesture produced. The host routes it to whatever
    /// owns the document; the view never learns what that is.
    virtual IntentResult submit_intent(const Intent& intent) noexcept = 0;
};

} // namespace pulp::timeline_editor
