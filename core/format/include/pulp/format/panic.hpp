#pragma once

/// @file panic.hpp
/// Processor-level MIDI panic: a request any thread can raise, serviced on the
/// audio thread at the start of the next block.
///
/// `Processor::request_panic()` raises the request. A host adapter that wants to
/// honour it calls `service_panic()` before `Processor::process()`, and
/// `PanicState::observe_midi_out()` after, so the panic can end exactly the
/// notes the plug-in has sent downstream. The request is serviced by:
///
///   1. calling `Processor::on_panic(mode)` so the plug-in stops its own voices
///      (`midi::Synthesiser::panic()` is the one-line implementation), and
///   2. where the adapter has MIDI output, writing note-offs for the notes the
///      plug-in left held plus the channel panic messages from
///      `pulp/midi/panic.hpp`.
///
/// Nothing here knows about any user interface: a button, a menu item or a
/// script calls `request_panic()` and reads `panic_capabilities()`.

#include <pulp/midi/buffer.hpp>
#include <pulp/midi/keyboard_state.hpp>
#include <pulp/midi/panic.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace pulp::format {

/// How hard a panic stops the plug-in.
enum class PanicMode : std::uint8_t {
    /// Hard stop: voices are reset and release tails cut (MIDI CC 120 semantic).
    /// The default.
    immediate = 2,
    /// Release only: voices enter their normal release and tails ring out
    /// (MIDI CC 123 semantic).
    release = 1,
};

/// What a panic request can do in the current host.
struct PanicCapabilities {
    /// A host adapter is servicing requests. False means `request_panic()` is
    /// recorded but nothing runs it, e.g. an adapter that has not been wired.
    bool serviced = false;
    /// The servicing adapter has MIDI output, so panic messages and note-offs
    /// reach downstream devices. False means only the plug-in's own voices are
    /// reset.
    bool midi_out = false;
};

/// What `service_panic()` did.
struct PanicOutcome {
    bool serviced = false;
    PanicMode mode = PanicMode::immediate;
    /// Events written to the MIDI output buffer.
    std::size_t events_emitted = 0;
    /// Events the buffer had no room for this block. They are not lost: the
    /// panic carries over and the next blocks send them (see `complete`).
    std::size_t events_deferred = 0;
    /// True when the whole panic has been written. False means the MIDI output
    /// buffer filled and `service_panic()` continues the panic on the next
    /// blocks without a new request.
    bool complete = true;
    /// This block continued a panic begun in an earlier block.
    bool carried = false;
};

/// Lock-free panic request and output-note state for one Processor. Created off
/// the audio thread (`Processor::panic_state()`); the audio thread only touches
/// the atomics and the note tracker.
class PanicState {
  public:
    /// Raise a panic request from any thread. A pending release request is
    /// upgraded by an immediate one, never the reverse.
    void request(PanicMode mode) noexcept {
        const auto wanted = static_cast<std::uint8_t>(mode);
        auto current = pending_.load(std::memory_order_relaxed);
        while (current < wanted &&
               !pending_.compare_exchange_weak(current, wanted, std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
        }
    }

    /// Consume the pending request (audio thread). False when none is pending.
    bool take(PanicMode& mode) noexcept {
        const auto taken = pending_.exchange(0, std::memory_order_acq_rel);
        if (taken == 0)
            return false;
        mode = static_cast<PanicMode>(taken);
        return true;
    }

    /// A panic whose MIDI did not fit in one block and is still being sent.
    bool carrying() const noexcept {
        return carry_.load(std::memory_order_acquire) != 0;
    }

    /// Total events that did not fit the MIDI output buffer in some block and
    /// were deferred to a later one. Readable from any thread; a persistently
    /// rising value means the adapter's per-block MIDI buffer is too small for
    /// the panic (a full panic is up to `midi::kMaxPanicMessages` events plus
    /// one note-off per held note).
    std::uint64_t deferred_events() const noexcept {
        return deferred_total_.load(std::memory_order_relaxed);
    }

    bool pending() const noexcept {
        return pending_.load(std::memory_order_acquire) != 0;
    }

    /// Adapter wiring, off the audio thread. `midi_out` states whether the
    /// adapter can deliver MIDI output.
    void attach(bool midi_out) noexcept {
        midi_out_.store(midi_out, std::memory_order_relaxed);
        attached_.store(true, std::memory_order_release);
    }
    void detach() noexcept {
        attached_.store(false, std::memory_order_release);
    }

    PanicCapabilities capabilities() const noexcept {
        const bool attached = attached_.load(std::memory_order_acquire);
        return {attached, attached && midi_out_.load(std::memory_order_relaxed)};
    }

    /// Track the notes the plug-in sent downstream (audio thread, after
    /// process()). Only meaningful when the adapter has MIDI output.
    void observe_midi_out(const midi::MidiBuffer& midi_out) noexcept {
        if (!midi_out_.load(std::memory_order_relaxed))
            return;
        for (const auto& event : midi_out)
            emitted_.process(event);
    }

    /// Notes currently believed held downstream.
    const midi::MidiKeyboardState& emitted_notes() const noexcept {
        return emitted_;
    }

    /// Write the panic into `out` (audio thread): note-offs for the held
    /// notes, then the channel messages for `mode`, all at `sample_offset`.
    /// Appends only; it never clears `out`. If `out` fills, what was written
    /// stays written and the rest is remembered: notes not yet ended stay in
    /// the tracker, and the channel messages resume at the first one not
    /// written. The next `service_panic()` call continues from there.
    PanicOutcome emit_midi(PanicMode mode, midi::MidiBuffer& out,
                           std::int32_t sample_offset = 0) noexcept {
        PanicOutcome outcome;
        outcome.serviced = true;
        outcome.mode = mode;
        bool full = false;
        const auto put = [&](const midi::MidiEvent& event) {
            if (!full && out.add(event)) {
                ++outcome.events_emitted;
                return true;
            }
            full = true;
            ++outcome.events_deferred;
            return false;
        };
        midi::for_each_held_note(emitted_, 0xFFFFu, [&](std::uint8_t ch, std::uint8_t note) {
            auto event = midi::MidiEvent::note_off(ch, note);
            event.sample_offset = sample_offset;
            if (put(event))
                emitted_.process(event); // ended: no longer held downstream
        });
        const auto scope =
            mode == PanicMode::immediate ? midi::PanicScope{} : midi::PanicScope::release_only();
        std::array<midi::MidiEvent, midi::kMaxPanicMessages> messages;
        const auto count = midi::panic_messages(scope, messages, sample_offset);
        std::size_t next = message_cursor_;
        for (; next < count; ++next) {
            if (!put(messages[next]))
                break;
        }
        // Everything after the first refused message is deferred too.
        outcome.events_deferred += next < count ? count - next - 1 : 0;
        message_cursor_ = static_cast<std::uint16_t>(next < count ? next : 0);
        outcome.complete = !full;
        if (full) {
            const auto wanted = static_cast<std::uint8_t>(mode);
            if (carry_.load(std::memory_order_relaxed) < wanted)
                carry_.store(wanted, std::memory_order_release);
            deferred_total_.fetch_add(outcome.events_deferred, std::memory_order_relaxed);
        } else {
            carry_.store(0, std::memory_order_release);
            emitted_.reset();
        }
        return outcome;
    }

    /// Mode of a panic still being sent (audio thread).
    bool take_carry(PanicMode& mode) noexcept {
        const auto carried = carry_.load(std::memory_order_acquire);
        if (carried == 0)
            return false;
        mode = static_cast<PanicMode>(carried);
        return true;
    }

    /// Start the message sequence from the top (a new request arrived).
    void restart_messages() noexcept {
        message_cursor_ = 0;
    }

  private:
    std::atomic<std::uint8_t> pending_{0};
    std::atomic<std::uint8_t> carry_{0};
    std::atomic<std::uint64_t> deferred_total_{0};
    std::uint16_t message_cursor_ = 0; // audio thread only
    std::atomic<bool> attached_{false};
    std::atomic<bool> midi_out_{false};
    midi::MidiKeyboardState emitted_;
};

/// Service a pending panic request (audio thread, before process()). `Proc` is
/// any type with `void on_panic(PanicMode)`, i.e. a `Processor`. The plug-in's
/// own voices are reset once per request; MIDI is written only when the state's
/// adapter reported MIDI output, and a panic too large for one block's MIDI
/// buffer continues over the following blocks without a new request. Returns
/// `serviced == false` when nothing was pending or carried.
template <typename Proc>
inline PanicOutcome service_panic(Proc& processor, PanicState& state, midi::MidiBuffer& midi_out,
                                  std::int32_t sample_offset = 0) noexcept {
    PanicMode mode = PanicMode::immediate;
    const bool fresh = state.take(mode);
    bool carried = false;
    if (!fresh) {
        if (!state.take_carry(mode))
            return {};
        carried = true;
    } else {
        state.restart_messages();
        // A new request while an earlier panic is still being sent keeps the
        // harder of the two.
        PanicMode carried_mode = PanicMode::release;
        if (state.take_carry(carried_mode) &&
            static_cast<std::uint8_t>(carried_mode) > static_cast<std::uint8_t>(mode))
            mode = carried_mode;
        processor.on_panic(mode);
    }
    if (!state.capabilities().midi_out) {
        PanicOutcome outcome;
        outcome.serviced = true;
        outcome.mode = mode;
        return outcome;
    }
    auto outcome = state.emit_midi(mode, midi_out, sample_offset);
    outcome.carried = carried;
    return outcome;
}

} // namespace pulp::format
