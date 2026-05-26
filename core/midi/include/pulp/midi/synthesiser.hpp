#pragma once

/// @file synthesiser.hpp
/// Generic MIDI-1 polyphonic synth framework.
///
/// `Synthesiser<Voice>` owns a fixed-size pool of `Voice` instances
/// (subclasses of `SynthesiserVoice`) and routes MIDI events into
/// them, with a configurable voice-stealing strategy when the pool
/// is exhausted.
///
/// Relationship to MPE: `MpeVoiceAllocator` (in
/// `mpe_synth_voice.hpp`) is the MPE-aware counterpart that routes
/// per-note expression across MPE member channels. `Synthesiser`
/// is the plain-MIDI alternative — one note per channel/note pair,
/// channel-level pitch bend / aftertouch / CC apply to every active
/// note on that channel.
///
/// Use `Synthesiser` when the descriptor sets `supports_mpe = false`
/// (the default for non-expressive synths).

#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

namespace pulp::midi {

/// Per-note metadata maintained by the Synthesiser when a voice is
/// activated. Subclasses read this via `voice.note()`.
struct SynthesiserNote {
    uint8_t channel = 0;          ///< MIDI channel (0–15)
    uint8_t note = 0;             ///< MIDI note number (0–127)
    uint8_t velocity = 0;         ///< Note-on velocity (1–127; 0 → note off)
    int8_t priority = 0;          ///< User-supplied priority for `Priority` steal
    uint32_t note_id = 0;         ///< Monotonic id; younger > older
    bool active = false;          ///< Voice is sounding (held or releasing)
    bool releasing = false;       ///< Note-off received; voice in release tail
};

/// Voice-stealing strategy applied when `note_on` fires and no free
/// voice is available.
enum class VoiceStealStrategy : uint8_t {
    /// Steal the oldest active voice (smallest `note_id`).
    Oldest,
    /// Steal the voice currently reporting the lowest `peak_level()`.
    /// Voices that don't override `peak_level()` all report 0, so this
    /// degrades to a deterministic earliest-found scan — useful for
    /// quiet-tail envelopes.
    Quietest,
    /// Steal the voice with the lowest MIDI note number.
    Lowest,
    /// Steal the voice with the highest MIDI note number.
    Highest,
    /// Steal the voice with the lowest `priority`. Ties broken by
    /// oldest-first.
    Priority,
};

/// Abstract base for one synth voice. Subclasses implement `render`
/// and may override `on_pitch_bend` / `on_aftertouch` / `on_cc` /
/// `peak_level` to participate in channel-level controllers and
/// voice-stealing.
class SynthesiserVoice {
public:
    virtual ~SynthesiserVoice() = default;

    /// Activate this voice for `note`. Called by the synth when the
    /// pool finds (or steals) a slot. Subclasses can override to
    /// schedule envelope attack etc.; the base records the note and
    /// sets `active_ = true`.
    virtual void on_note_on(const SynthesiserNote& note) {
        note_ = note;
        active_ = true;
        releasing_ = false;
    }

    /// Begin release. Subclasses typically schedule envelope release
    /// here; when the envelope tail completes they should call
    /// `mark_inactive()` so the synth's free-voice scan can reclaim
    /// the slot. Keeps `note_.releasing` in sync with the `releasing_`
    /// member so subclasses reading either path see the same state
    /// (Codex P2 on #2870).
    virtual void on_note_off() {
        releasing_ = true;
        note_.releasing = true;
    }

    /// Channel-level pitch bend (semitones, already scaled by the
    /// configured bend range). Default no-op — override to apply.
    virtual void on_pitch_bend(float bend_semitones) { (void)bend_semitones; }

    /// Channel-level aftertouch (0..1). Default no-op.
    virtual void on_aftertouch(float pressure) { (void)pressure; }

    /// Channel-level CC (raw 0–127 value). Default no-op.
    virtual void on_cc(uint8_t cc_number, uint8_t value) {
        (void)cc_number; (void)value;
    }

    /// Render `num_samples` of audio into `out`. The synth calls
    /// `render` ADDITIVELY — voices sum into the existing buffer
    /// contents — so the caller should zero `out` before calling
    /// `Synthesiser::process` if they want a fresh block.
    virtual void render(float* out, int num_samples) = 0;

    /// Reported by the voice for the `Quietest` steal strategy and
    /// for caller-side metering. Default 0 (matches "no audio level
    /// info available"); subclasses with an envelope should return
    /// the envelope's current level.
    virtual float peak_level() const { return 0.0f; }

    /// Hard-reset (clear state, drop note). Called when a voice is
    /// stolen and reused.
    virtual void reset() {
        note_ = {};
        active_ = false;
        releasing_ = false;
    }

    bool active() const { return active_; }
    bool releasing() const { return releasing_; }
    const SynthesiserNote& note() const { return note_; }

protected:
    /// Subclasses call this from `render()` once their release tail
    /// finishes so the synth can reclaim the voice on the next
    /// `note_on`.
    void mark_inactive() { active_ = false; releasing_ = false; }

    SynthesiserNote note_{};
    bool active_ = false;
    bool releasing_ = false;
};

/// Polyphonic synth that routes plain-MIDI events into a pool of
/// `Voice` instances. `Voice` must derive from `SynthesiserVoice`.
template <typename Voice>
class Synthesiser {
    static_assert(std::is_base_of<SynthesiserVoice, Voice>::value,
                  "Voice must derive from SynthesiserVoice");

public:
    /// Construct with a fixed polyphony. Voices are
    /// default-constructed.
    explicit Synthesiser(std::size_t polyphony = 16)
        : voices_(polyphony) {}

    void set_steal_strategy(VoiceStealStrategy s) { strategy_ = s; }
    VoiceStealStrategy steal_strategy() const { return strategy_; }

    /// Pitch-bend range applied to channel-level pitch-bend messages.
    /// Default is ±2 semitones (MIDI 1.0 RPN 0 default). Override
    /// with `set_pitch_bend_range_semitones` to match the host's
    /// expected range (typically ±2, ±12, or ±24).
    void set_pitch_bend_range_semitones(float r) {
        if (r > 0.0f) bend_range_semi_ = r;
    }
    float pitch_bend_range_semitones() const { return bend_range_semi_; }

    /// Direct event entry points — usable when the caller wants to
    /// build a Synthesiser pipeline without a full MidiBuffer.
    void note_on(uint8_t channel, uint8_t note, uint8_t velocity,
                 int8_t priority = 0) {
        if (velocity == 0) { note_off(channel, note); return; }
        Voice* v = find_free_voice();
        if (!v) v = steal_voice();
        if (!v) return;
        SynthesiserNote n;
        n.channel = static_cast<uint8_t>(channel & 0x0F);
        n.note = static_cast<uint8_t>(note & 0x7F);
        n.velocity = static_cast<uint8_t>(velocity & 0x7F);
        n.priority = priority;
        n.note_id = ++next_id_;
        n.active = true;
        n.releasing = false;
        v->on_note_on(n);
    }

    void note_off(uint8_t channel, uint8_t note) {
        for (auto& v : voices_) {
            if (v.active() && !v.releasing()
                && v.note().channel == (channel & 0x0F)
                && v.note().note == (note & 0x7F)) {
                v.on_note_off();
            }
        }
    }

    void pitch_bend(uint8_t channel, float bend_semitones) {
        for (auto& v : voices_) {
            if (v.active() && v.note().channel == (channel & 0x0F)) {
                v.on_pitch_bend(bend_semitones);
            }
        }
    }

    void aftertouch(uint8_t channel, float pressure) {
        for (auto& v : voices_) {
            if (v.active() && v.note().channel == (channel & 0x0F)) {
                v.on_aftertouch(pressure);
            }
        }
    }

    void cc(uint8_t channel, uint8_t cc_number, uint8_t value) {
        for (auto& v : voices_) {
            if (v.active() && v.note().channel == (channel & 0x0F)) {
                v.on_cc(cc_number, value);
            }
        }
    }

    /// Drive the synth across one audio block. Events are dispatched
    /// at their sample offsets within the block; voices render the
    /// sub-segments between events. The output buffer is NOT zeroed
    /// — voices render additively, so the caller must clear it (or
    /// pass a buffer that already holds the desired pre-mix).
    void process(const MidiBuffer& events, float* out, int num_samples) {
        if (num_samples <= 0) return;
        int sample_index = 0;
        for (std::size_t i = 0; i < events.size(); ++i) {
            const auto& e = events[i];
            int target = static_cast<int>(e.sample_offset);
            if (target < sample_index) target = sample_index;
            if (target > num_samples) target = num_samples;
            if (target > sample_index) {
                render_active(out + sample_index, target - sample_index);
                sample_index = target;
            }
            dispatch_event(e);
        }
        if (sample_index < num_samples) {
            render_active(out + sample_index, num_samples - sample_index);
        }
    }

    std::size_t polyphony() const { return voices_.size(); }
    Voice& voice(std::size_t i) { return voices_[i]; }
    const Voice& voice(std::size_t i) const { return voices_[i]; }

    std::size_t active_count() const {
        std::size_t n = 0;
        for (const auto& v : voices_) if (v.active()) ++n;
        return n;
    }

    void reset() {
        for (auto& v : voices_) v.reset();
        next_id_ = 0;
    }

private:
    Voice* find_free_voice() {
        for (auto& v : voices_) if (!v.active()) return &v;
        return nullptr;
    }

    Voice* steal_voice() {
        if (voices_.empty()) return nullptr;
        Voice* chosen = nullptr;
        switch (strategy_) {
            case VoiceStealStrategy::Oldest: {
                uint32_t oldest_id = std::numeric_limits<uint32_t>::max();
                for (auto& v : voices_) {
                    if (v.active() && v.note().note_id < oldest_id) {
                        chosen = &v;
                        oldest_id = v.note().note_id;
                    }
                }
                break;
            }
            case VoiceStealStrategy::Quietest: {
                float min_level = std::numeric_limits<float>::infinity();
                uint32_t tie_id = std::numeric_limits<uint32_t>::max();
                for (auto& v : voices_) {
                    if (!v.active()) continue;
                    const float lvl = v.peak_level();
                    if (lvl < min_level
                        || (lvl == min_level && v.note().note_id < tie_id)) {
                        chosen = &v;
                        min_level = lvl;
                        tie_id = v.note().note_id;
                    }
                }
                break;
            }
            case VoiceStealStrategy::Lowest: {
                uint8_t lowest = 127;
                uint32_t tie_id = std::numeric_limits<uint32_t>::max();
                for (auto& v : voices_) {
                    if (!v.active()) continue;
                    const uint8_t n = v.note().note;
                    if (n < lowest || (n == lowest && v.note().note_id < tie_id)) {
                        chosen = &v;
                        lowest = n;
                        tie_id = v.note().note_id;
                    }
                }
                break;
            }
            case VoiceStealStrategy::Highest: {
                int highest = -1;
                uint32_t tie_id = std::numeric_limits<uint32_t>::max();
                for (auto& v : voices_) {
                    if (!v.active()) continue;
                    const int n = v.note().note;
                    if (n > highest
                        || (n == highest && v.note().note_id < tie_id)) {
                        chosen = &v;
                        highest = n;
                        tie_id = v.note().note_id;
                    }
                }
                break;
            }
            case VoiceStealStrategy::Priority: {
                int lowest_prio = std::numeric_limits<int>::max();
                uint32_t tie_id = std::numeric_limits<uint32_t>::max();
                for (auto& v : voices_) {
                    if (!v.active()) continue;
                    const int p = v.note().priority;
                    if (p < lowest_prio
                        || (p == lowest_prio && v.note().note_id < tie_id)) {
                        chosen = &v;
                        lowest_prio = p;
                        tie_id = v.note().note_id;
                    }
                }
                break;
            }
        }
        if (chosen) chosen->reset();
        return chosen;
    }

    void dispatch_event(const MidiEvent& e) {
        const uint8_t status_top = static_cast<uint8_t>(e.data()[0] & 0xF0);
        const uint8_t channel = e.channel();
        if (e.is_note_on() && e.velocity() > 0) {
            note_on(channel, e.note(), e.velocity());
            return;
        }
        if (e.is_note_off() || (e.is_note_on() && e.velocity() == 0)) {
            note_off(channel, e.note());
            return;
        }
        if (e.is_pitch_bend()) {
            const uint16_t raw = static_cast<uint16_t>(
                e.data()[1] | (uint16_t(e.data()[2]) << 7));
            const float norm = (static_cast<float>(raw) - 8192.0f) / 8192.0f;
            pitch_bend(channel, norm * bend_range_semi_);
            return;
        }
        if (status_top == 0xD0) {
            aftertouch(channel, e.data()[1] / 127.0f);
            return;
        }
        if (e.is_cc()) {
            cc(channel, e.cc_number(), e.cc_value());
            return;
        }
        // PolyAftertouch (0xA0), ProgramChange (0xC0), system messages
        // are ignored at this layer — subclasses can route them via
        // direct event taps if needed.
    }

    void render_active(float* out, int n) {
        for (auto& v : voices_) {
            if (v.active()) v.render(out, n);
        }
    }

    std::vector<Voice> voices_;
    VoiceStealStrategy strategy_ = VoiceStealStrategy::Oldest;
    float bend_range_semi_ = 2.0f;
    uint32_t next_id_ = 0;
};

} // namespace pulp::midi
