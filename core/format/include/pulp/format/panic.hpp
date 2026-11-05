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
    /// Events the buffer refused (capacity), so a caller can report truncation.
    std::size_t events_dropped = 0;
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
    /// Clears the note tracker. Appends only; it never clears `out`.
    PanicOutcome emit_midi(PanicMode mode, midi::MidiBuffer& out,
                           std::int32_t sample_offset = 0) noexcept {
        PanicOutcome outcome;
        outcome.serviced = true;
        outcome.mode = mode;
        const auto put = [&](const midi::MidiEvent& event) {
            if (out.add(event))
                ++outcome.events_emitted;
            else
                ++outcome.events_dropped;
        };
        midi::for_each_held_note(emitted_, 0xFFFFu, [&](std::uint8_t ch, std::uint8_t note) {
            auto event = midi::MidiEvent::note_off(ch, note);
            event.sample_offset = sample_offset;
            put(event);
        });
        const auto scope =
            mode == PanicMode::immediate ? midi::PanicScope{} : midi::PanicScope::release_only();
        std::array<midi::MidiEvent, midi::kMaxPanicMessages> messages;
        const auto count = midi::panic_messages(scope, messages, sample_offset);
        for (std::size_t i = 0; i < count; ++i)
            put(messages[i]);
        emitted_.reset();
        return outcome;
    }

  private:
    std::atomic<std::uint8_t> pending_{0};
    std::atomic<bool> attached_{false};
    std::atomic<bool> midi_out_{false};
    midi::MidiKeyboardState emitted_;
};

/// Service a pending panic request (audio thread, before process()). `Proc` is
/// any type with `void on_panic(PanicMode)`, i.e. a `Processor`. The plug-in's
/// own voices are always reset; MIDI is written only when the state's adapter
/// reported MIDI output. Returns `serviced == false` when nothing was pending.
template <typename Proc>
inline PanicOutcome service_panic(Proc& processor, PanicState& state, midi::MidiBuffer& midi_out,
                                  std::int32_t sample_offset = 0) noexcept {
    PanicMode mode = PanicMode::immediate;
    if (!state.take(mode))
        return {};
    processor.on_panic(mode);
    if (!state.capabilities().midi_out) {
        PanicOutcome outcome;
        outcome.serviced = true;
        outcome.mode = mode;
        return outcome;
    }
    return state.emit_midi(mode, midi_out, sample_offset);
}

} // namespace pulp::format
