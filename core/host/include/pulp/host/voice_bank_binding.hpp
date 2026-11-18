#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/host/graph_event_contract.hpp>
#include <pulp/host/signal_graph_subgraph.hpp>
#include <pulp/host/voice_template.hpp>
#include <pulp/midi/mpe_synth_voice.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace pulp::host {

class VoiceBankOwner {
  public:
    virtual ~VoiceBankOwner() = default;
    virtual std::unique_ptr<format::Processor> make_processor(std::uint32_t slot) = 0;
    virtual void dispatch(const midi::MpeExpressionEvent& event) noexcept = 0;
    virtual bool live(std::uint32_t slot, midi::MpeNoteGeneration generation,
                      midi::MpeExpressionEvent::Kind kind,
                      const midi::MpeNoteState& state) const noexcept = 0;
    virtual void reset() noexcept = 0;
    virtual void set_steal_mode(midi::MpeVoiceStealMode mode) noexcept = 0;
    // The audio owner can inspect and retire one exact generation without
    // changing another slot. This is needed for scoped All Sound Off.
    virtual bool slot_note(std::uint32_t slot, midi::MpeNoteState& note,
                           bool& releasing) const noexcept = 0;
    virtual bool retire_slot(std::uint32_t slot, midi::MpeNoteGeneration generation) noexcept = 0;
    // Opaque identity of the mutable voice state borrowed by the Processor
    // factory. Preparation refuses null or repeated identities across slots.
    virtual const void* slot_state_identity(std::uint32_t slot) const noexcept = 0;
    enum class InputSemantic : std::uint8_t { Unknown, Note, Pitch, Pressure, Timbre };
    virtual InputSemantic resolve_input(const VoiceBindingSpec& binding) const noexcept = 0;
};

struct VoiceBankInputEndpoint {
    std::string target;
    std::uint32_t ordinal = 0;
};

// Number of voice slots in a bank.
inline constexpr std::uint32_t kVoiceBankSlots = 8;

// The developer supplies a Processor that borrows the corresponding voice.
// This adapter retains exactly one existing MPE allocator for all eight slots.
template <class Voice> class MpeVoiceBankOwner final : public VoiceBankOwner {
    static_assert(std::is_base_of_v<midi::MpeSynthVoice, Voice>);

  public:
    using ProcessorFactory =
        std::function<std::unique_ptr<format::Processor>(Voice&, std::uint32_t)>;
    explicit MpeVoiceBankOwner(ProcessorFactory factory,
                               std::array<VoiceBankInputEndpoint, 4> endpoints)
        : allocator_(kVoiceBankSlots), factory_(std::move(factory)),
          endpoints_(std::move(endpoints)) {}
    std::unique_ptr<format::Processor> make_processor(std::uint32_t slot) override {
        return slot < kVoiceBankSlots && factory_ ? factory_(allocator_.voice(slot), slot)
                                                  : nullptr;
    }
    void dispatch(const midi::MpeExpressionEvent& event) noexcept override {
        allocator_.dispatch(event);
    }
    bool live(std::uint32_t slot, midi::MpeNoteGeneration generation,
              midi::MpeExpressionEvent::Kind kind,
              const midi::MpeNoteState& state) const noexcept override {
        return slot < kVoiceBankSlots && generation != 0 && allocator_.voice(slot).active() &&
               allocator_.voice(slot).note_id() == generation && state.note_id == generation &&
               state.channel == allocator_.voice(slot).channel() &&
               state.note == allocator_.voice(slot).note_number() &&
               state.velocity == allocator_.voice(slot).velocity() &&
               kind != midi::MpeExpressionEvent::Kind::NoteOn &&
               (kind != midi::MpeExpressionEvent::Kind::NoteOff ||
                !allocator_.voice(slot).releasing());
    }
    void reset() noexcept override {
        allocator_.reset_all();
    }
    void set_steal_mode(midi::MpeVoiceStealMode mode) noexcept override {
        allocator_.set_steal_mode(mode);
    }
    bool slot_note(std::uint32_t slot, midi::MpeNoteState& note,
                   bool& releasing) const noexcept override {
        if (slot >= kVoiceBankSlots || !allocator_.voice(slot).active())
            return false;
        const auto& voice = allocator_.voice(slot);
        note = {};
        note.active = true;
        note.note_id = voice.note_id();
        note.channel = voice.channel();
        note.note = voice.note_number();
        note.velocity = voice.velocity();
        releasing = voice.releasing();
        return true;
    }
    bool retire_slot(std::uint32_t slot, midi::MpeNoteGeneration generation) noexcept override {
        return allocator_.retire_slot(slot, generation);
    }
    const void* slot_state_identity(std::uint32_t slot) const noexcept override {
        return slot < kVoiceBankSlots ? static_cast<const void*>(&allocator_.voice(slot)) : nullptr;
    }
    InputSemantic resolve_input(const VoiceBindingSpec& binding) const noexcept override {
        if (binding.target_kind != VoiceBindingTarget::Port ||
            binding.direction != VoiceBindingDirection::Input)
            return InputSemantic::Unknown;
        for (std::size_t i = 0; i < endpoints_.size(); ++i) {
            if (endpoints_[i].target != binding.target || endpoints_[i].ordinal != binding.ordinal)
                continue;
            const bool note = i == 0;
            if (binding.type != (note ? VoiceBindingType::Event : VoiceBindingType::Control) ||
                binding.shape != VoiceBindingShape::Scalar ||
                binding.rate != (note ? VoiceBindingRate::Event : VoiceBindingRate::Sample))
                return InputSemantic::Unknown;
            return static_cast<InputSemantic>(i + 1);
        }
        return InputSemantic::Unknown;
    }
    midi::MpeVoiceAllocator<Voice>& allocator() noexcept {
        return allocator_;
    }
    const midi::MpeVoiceAllocator<Voice>& allocator() const noexcept {
        return allocator_;
    }

  private:
    midi::MpeVoiceAllocator<Voice> allocator_;
    ProcessorFactory factory_;
    std::array<VoiceBankInputEndpoint, 4> endpoints_;
};

enum class VoiceBankBindingCode : std::uint8_t {
    Ok,
    InvalidManifest,
    FactoryFailure,
    GraphPrepareFailure,
    InvalidBlock,
    IngressFailure,
    GraphExecutionFailure,
    StaleEpoch,
    StaleGeneration,
    EpochExhausted,
    TrackerCapacity,
    OutOfZone,
    UnsupportedControl,
    UnsupportedExpression,
    InvalidState,
};

struct VoiceBankBindingResult {
    VoiceBankBindingCode code = VoiceBankBindingCode::Ok;
    std::uint32_t failing_slot = 0;
    std::string failing_path;
    std::string detail;
    explicit operator bool() const noexcept {
        return code == VoiceBankBindingCode::Ok;
    }
};

// Optional audio-thread saving. With SkipInactiveSlots, a voice slot whose
// voice is not active is not rendered once it has already produced a block of
// exact silence (all sample bits zero) for at least its declared latency plus
// tail. Skipping gives the same samples as rendering, provided the voice meets
// this contract: at every note start, the voice's output must not depend on how
// many blocks it was rendered while inactive. A voice with free-running state
// (an LFO or noise source that keeps running, a parameter smoother that keeps
// moving, anything that follows the transport while silent) breaks it and must
// use Off. prepare() refuses the option with InvalidManifest when a voice
// declares an audio input, audio-rate modulation or an unbounded tail.
enum class VoiceBankIdleSkip : std::uint8_t { Off, SkipInactiveSlots };

// What happens to sounding voices when the bank moves to a new epoch
// (activate() and commit_event_reset()).
//
// HardCut (the default): every voice is cut on the reset, through the owner's
// reset(). Output from the next frame contains only the new epoch's notes.
//
// DrainRetired (opt-in): each sounding voice is retired instead of cut. A held
// or pedal-held voice is released, an already releasing voice keeps releasing,
// and each retired voice renders for at most `tail_frames` further frames,
// after which it is cut on that exact frame whatever the block partition. At
// most `capacity` voices drain at once, counting drains still running from an
// earlier reset (each keeps its original deadline); every further sounding
// voice is refused a drain and cut on the reset, scanning slots in ordinal
// order. A retired voice belongs to no epoch: deferred tokens for it are
// refused, sustain and CC121 of the new epoch do not touch it, and an All
// Sound Off covering its channel, or a steal by a new note, ends it early. The
// owner's reset() is called only when no voice is left draining. The
// retirement table is preallocated with the bank and the audio path neither
// allocates nor locks.
//
// Neither policy is persistent state: capture_state() never records retired
// voices, their deadlines, active tokens, epochs or pedal holds, and
// restore_state() never revives them.
enum class VoiceBankRetirement : std::uint8_t { HardCut, DrainRetired };

// Longest finite tail a retired voice may drain for (about 21.8 s at 48 kHz).
inline constexpr std::uint32_t kVoiceBankMaxRetirementTailFrames = 1u << 20;

struct VoiceBankRetirementPolicy {
    VoiceBankRetirement mode = VoiceBankRetirement::HardCut;
    // DrainRetired: voices that may drain at once, 1 to kVoiceBankSlots.
    // HardCut: must be 0.
    std::uint32_t capacity = 0;
    // DrainRetired: frames a retired voice may still render, 1 to
    // kVoiceBankMaxRetirementTailFrames. HardCut: must be 0.
    std::uint32_t tail_frames = 0;
};

// Retirement counters since prepare(). Audio owner only, between blocks.
struct VoiceBankRetirementStats {
    std::uint32_t draining = 0;    // voices draining now
    std::uint64_t drained = 0;     // voices moved into a drain
    std::uint64_t refused = 0;     // voices cut on a reset because capacity was full
    std::uint64_t finished = 0;    // drains whose voice ended before its tail ran out
    std::uint64_t cut_at_tail = 0; // drains cut when their tail ran out
    std::uint64_t interrupted = 0; // drains ended by All Sound Off or a steal
};

// Sustain and channel-mode semantics of the bank, applied on the audio thread
// in event order at each event's exact sample:
//
// - CC64 at 64 or above is down, below 64 is up. On a member channel it covers
//   that channel; on a zone's manager channel it covers every member channel of
//   the zone. A note is sustained while either pedal covering its channel is
//   down.
// - A note-off (MIDI, CC123 or a deferred NoteOff token) for a sustained note
//   holds that exact voice generation instead of releasing it. A pedal rise
//   releases each held voice no pedal covers any more, once. A held voice that
//   is stolen leaves the hold table, so a later rise never releases the note
//   that replaced it.
// - CC121 lifts the pedal of its own scope, resets that scope's expression
//   (and, on a member channel, the expression of its held and releasing
//   voices) and releases voices that are no longer held.
// - CC120 cuts every voice in scope at once, sustained or not, and lifts the
//   pedals in scope. CC123 sends note-offs for every tracked note in scope,
//   which sustain still holds.
// - CC120, CC121 and CC123 with a non-zero value, and any controller other than
//   64, 74, 120, 121 and 123, are refused with UnsupportedControl before the
//   block changes anything.
// - A reset (activate() or commit_event_reset()) lifts every pedal and clears
//   the hold table; what happens to the voices is VoiceBankRetirement's
//   policy. Pedals and holds are never persistent state.

// One prepared graph and one eight-voice allocator. Construction, activation,
// and parameter edits are control-thread operations while audio is stopped.
// begin_block/publish/render and deferred dispatch belong to one audio owner.
class VoiceBankBinding final {
  public:
    static std::unique_ptr<VoiceBankBinding>
    prepare(const VoiceTemplateSchema& schema, SubgraphTemplate definition,
            std::unique_ptr<VoiceBankOwner> owner, std::string stereo_port, double sample_rate,
            int max_frames, VoiceBankBindingResult& result,
            midi::MpeConfig mpe_config = midi::MpeConfig::standard_lower(),
            VoiceBankIdleSkip idle_skip = VoiceBankIdleSkip::Off);
    ~VoiceBankBinding();
    VoiceBankBinding(const VoiceBankBinding&) = delete;
    VoiceBankBinding& operator=(const VoiceBankBinding&) = delete;

    const VoiceBankManifest& manifest() const noexcept;
    std::uint64_t epoch() const noexcept;
    bool activate(VoiceBankBindingResult& result) noexcept;
    void set_steal_mode(midi::MpeVoiceStealMode mode) noexcept;
    const VoiceBankOwner& owner() const noexcept;

    // begin_block returns Busy while a block is open and OutOfRange for an
    // empty or oversized window; publish returns StaleEpoch with no open block.
    HostEventResult begin_block(std::uint64_t first_sample, std::size_t frames) noexcept;
    HostEventResult publish(const HostEventRecord& event) noexcept;
    // Audio-owner reset in two steps, so a caller that resets several event
    // families together can refuse the whole transaction before any of them
    // changes. reserve_event_reset() claims a fresh epoch and changes nothing
    // else; it refuses inside an open block (Busy) and when the epoch sequence
    // is exhausted (Overflow). commit_event_reset() applies the epoch the most
    // recent reserve returned: every voice is cut, and the tracker, held
    // releases, pedals and staged records are cleared, so records and deferred
    // tokens of the old epoch are refused as stale from then on. A commit with
    // any other epoch, or a second commit of the same one, is refused with
    // StaleEpoch; a commit inside an open block is refused with Busy. Neither
    // step allocates or locks. Slot parameters and release-tail DSP state are
    // not reset, exactly as for activate().
    HostEventResult reserve_event_reset(std::uint64_t& epoch) noexcept;
    HostEventResult commit_event_reset(std::uint64_t epoch) noexcept;
    // Close an open block without rendering it, for a caller whose sibling
    // event family refused the same block. Staged future records stay staged.
    // Returns StaleEpoch when no block is open.
    HostEventResult abort_block() noexcept;
    // On GraphExecutionFailure the whole block is silenced and closed.
    VoiceBankBindingCode render(audio::BufferView<float>& stereo, std::uint64_t first_sample,
                                std::size_t frames) noexcept;
    // A deferred NoteOff releases the voice and retires its tracker entry, so
    // no MIDI note-off for the same key is needed to free tracker capacity.
    VoiceBankBindingCode dispatch_deferred(std::uint64_t epoch, std::uint32_t slot,
                                           midi::MpeNoteGeneration generation,
                                           midi::MpeExpressionEvent::Kind kind,
                                           const midi::MpeNoteState& state) noexcept;
    bool set_slot_parameter(std::uint32_t slot, std::string_view path, float value);
    // The value a declared slot parameter currently holds, or nullopt for an
    // out-of-range slot or an undeclared parameter path. Control thread.
    std::optional<float> slot_parameter(std::uint32_t slot, std::string_view path) const;
    // Every declared slot parameter as persistent state bound to manifest().
    // Notes, generations, pedals, epochs and release tails are not state.
    VoiceBankState capture_state() const;
    // Apply persistent state to this live bank, all or nothing. The whole
    // state is checked first: manifest identity and schema version, declared
    // slots and targets, duplicates, finite values and each parameter's
    // authored bounds. On any refusal no slot parameter changes and `result`
    // carries InvalidState (or InvalidBlock inside an open block) with the
    // failing slot and target. Parameters the state does not name keep their
    // current values. Sounding voices are not stopped. Control thread, between
    // blocks.
    bool restore_state(const VoiceBankState& state, VoiceBankBindingResult& result);
    // Choose what a reset does to sounding voices (see VoiceBankRetirement).
    // Control thread, between blocks. Refused with InvalidBlock inside an open
    // block, and with InvalidState for capacity or tail_frames outside the
    // documented ranges or while a drain is still running; a refusal leaves the
    // current policy in place.
    bool set_retirement_policy(const VoiceBankRetirementPolicy& policy,
                               VoiceBankBindingResult& result) noexcept;
    VoiceBankRetirementPolicy retirement_policy() const noexcept;
    VoiceBankRetirementStats retirement_stats() const noexcept;
    // Voice-blocks not rendered because of VoiceBankIdleSkip, summed over the
    // eight slots. Always 0 when the option is Off. Counts since prepare().
    // Lock-free; safe to call from any thread, including the audio thread.
    std::uint64_t idle_skipped_blocks() const noexcept;

  private:
    void reset_idle_confirmations() noexcept;
    void retire_voices() noexcept;
    VoiceBankBinding();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::host
