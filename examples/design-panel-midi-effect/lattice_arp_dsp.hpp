#pragma once

// LATTICE — the arpeggiator the panel is a face for.
//
// A MIDI processor: notes in, notes out, no audio path at all. Kept beside the
// Processor so the note logic can be tested without constructing a plugin.
//
// The rate is in steps per second rather than a note division against host
// tempo. A division is what the panel draws and what a musician wants, but it
// makes every test depend on a transport the harness would have to fake; steps
// per second is the same machine with a directly testable clock. Wiring it to
// `ProcessContext::tempo` is a later change to one function.

#include <pulp/midi/buffer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace pulp::examples {

/// Arpeggiates whatever notes are currently held.
class LatticeArp {
public:
    static constexpr std::size_t kMaxHeld = 16;

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
        reset();
    }

    /// Drops every held note and silences anything sounding.
    ///
    /// Does NOT emit the note-offs itself: reset() is called from prepare()
    /// where there is no output buffer to write them into. Callers that need
    /// the notes released must use `release_all()`.
    void reset() {
        held_count_ = 0;
        step_ = 0;
        counter_ = 0.0;
        sounding_note_ = -1;
        sounding_remaining_ = 0.0;
    }

    void set_rate_hz(float hz) { rate_hz_ = std::clamp(hz, 0.5f, 20.0f); }
    /// Fraction of a step the note is held for.
    void set_gate(float g) { gate_ = std::clamp(g, 0.05f, 1.0f); }
    /// Delays every odd step by this fraction of a step.
    void set_swing(float s) { swing_ = std::clamp(s, 0.0f, 0.75f); }
    void set_octaves(int o) { octaves_ = std::clamp(o, 1, 4); }
    void set_velocity(float v) { velocity_ = std::clamp(v, 0.0f, 1.0f); }

    void note_on(int note) {
        for (std::size_t i = 0; i < held_count_; ++i)
            if (held_[i] == note) return;
        if (held_count_ >= kMaxHeld) return;
        // A chord arrives as several note-ons in one block; inserting in pitch
        // order makes the pattern depend on the notes rather than on the order
        // the keys happened to be pressed.
        std::size_t at = held_count_;
        while (at > 0 && held_[at - 1] > note) { held_[at] = held_[at - 1]; --at; }
        held_[at] = note;
        ++held_count_;
    }

    void note_off(int note) {
        for (std::size_t i = 0; i < held_count_; ++i) {
            if (held_[i] != note) continue;
            for (std::size_t j = i + 1; j < held_count_; ++j) held_[j - 1] = held_[j];
            --held_count_;
            return;
        }
    }

    std::size_t held() const { return held_count_; }
    int sounding_note() const { return sounding_note_; }

    /// Emits the note-off for anything currently sounding.
    ///
    /// The single most important operation here. An arpeggiator that stops
    /// without releasing leaves a note stuck on in the instrument downstream,
    /// which the user can only clear with a panic message.
    void release_all(midi::MidiBuffer& out, int sample_offset) {
        if (sounding_note_ < 0) return;
        out.add(midi::MidiEvent::note_off(sample_offset, sounding_note_));
        sounding_note_ = -1;
        sounding_remaining_ = 0.0;
    }

    /// Advances `num_samples`, writing generated notes into `out`.
    void process(midi::MidiBuffer& out, int num_samples) {
        const double step_samples = sample_rate_ / rate_hz_;

        for (int i = 0; i < num_samples; ++i) {
            // Close a sounding note when its gate expires, before opening the
            // next one — overlapping a step boundary would leave the old note
            // hanging when the next note-on reuses the same pitch.
            if (sounding_note_ >= 0) {
                sounding_remaining_ -= 1.0;
                if (sounding_remaining_ <= 0.0) {
                    out.add(midi::MidiEvent::note_off(i, sounding_note_));
                    sounding_note_ = -1;
                }
            }

            if (held_count_ == 0) { counter_ = 0.0; step_ = 0; continue; }

            counter_ -= 1.0;
            if (counter_ > 0.0) continue;

            // Swing displaces odd steps later; the following step absorbs the
            // displacement so the pattern does not drift out of time.
            const double offset = (step_ % 2 == 1) ? swing_ * step_samples : 0.0;
            counter_ += step_samples + offset - last_offset_;
            last_offset_ = offset;

            const int note = note_for_step(step_);
            if (note >= 0) {
                if (sounding_note_ >= 0) {
                    out.add(midi::MidiEvent::note_off(i, sounding_note_));
                    sounding_note_ = -1;
                }
                const int vel = std::clamp(
                    static_cast<int>(std::lround(velocity_ * 127.0f)), 1, 127);
                out.add(midi::MidiEvent::note_on(i, note, vel));
                sounding_note_ = note;
                sounding_remaining_ = std::max(1.0, step_samples * gate_);
            }
            ++step_;
        }
    }

private:
    /// Walks the held notes upward, then repeats the figure an octave higher
    /// for each additional octave.
    int note_for_step(unsigned step) const {
        if (held_count_ == 0) return -1;
        const unsigned span = static_cast<unsigned>(held_count_) *
                              static_cast<unsigned>(octaves_);
        const unsigned pos = step % span;
        const unsigned index = pos % static_cast<unsigned>(held_count_);
        const unsigned octave = pos / static_cast<unsigned>(held_count_);
        const int note = held_[index] + static_cast<int>(octave) * 12;
        // Above 127 there is no MIDI note to send. Dropping is correct and
        // silent; wrapping would play a note the player did not press.
        return note <= 127 ? note : -1;
    }

    std::array<int, kMaxHeld> held_{};
    std::size_t held_count_ = 0;
    double sample_rate_ = 48000.0;
    double counter_ = 0.0;
    double last_offset_ = 0.0;
    double sounding_remaining_ = 0.0;
    unsigned step_ = 0;
    int sounding_note_ = -1;
    int octaves_ = 1;
    float rate_hz_ = 8.0f;
    float gate_ = 0.5f;
    float swing_ = 0.0f;
    float velocity_ = 0.8f;
};

}  // namespace pulp::examples
