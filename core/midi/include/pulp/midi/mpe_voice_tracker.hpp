#pragma once

/// @file mpe_voice_tracker.hpp
/// MPE (MIDI Polyphonic Expression) voice tracker.
///
/// Tracks active notes across MPE member channels, dispatches per-note
/// pitch bend, pressure, and timbre (CC74), and enforces zone boundaries.
///
/// Feed all incoming `MidiEvent`s via `process()`. The tracker classifies
/// each event against the configured `MpeConfig`:
///   - Note On / Note Off on a member channel → updates the per-note record
///   - Pitch Bend on a member channel → applies to all active notes on that
///     channel (MPE member channels hold one note at a time in typical use,
///     but the tracker correctly handles overlap)
///   - Channel Pressure on a member channel → pressure
///   - CC 74 on a member channel → timbre (also aliased as `slide`)
///
/// Messages on manager channels are surfaced as global (zone-wide) state
/// but do not create notes. Messages on channels outside any zone are
/// ignored (returned `false` from `process()`).
///
/// All mutation is synchronous and lock-free (no allocations after
/// construction). Safe to call from the audio thread.

#include <pulp/midi/message.hpp>
#include <pulp/midi/ump.hpp>
#include <array>
#include <cstdint>
#include <functional>

namespace pulp::midi {

/// Per-note state maintained by `MpeVoiceTracker`.
///
/// Expression values are normalised:
///   - `pitch_bend_semitones` — absolute bend in semitones (member range applied)
///   - `pressure` — 0..1 from channel pressure (Dx status)
///   - `timbre` — 0..1 from CC 74 (aliased as `slide`)
struct MpeNoteState {
    bool active = false;
    uint8_t channel = 0;           ///< MPE member channel the note lives on
    uint8_t note = 0;              ///< MIDI note number (0-127)
    uint8_t velocity = 0;          ///< Note-on velocity (0-127)
    float pitch_bend_semitones = 0.0f;  ///< Current per-note pitch bend in semitones
    float pressure = 0.0f;         ///< Channel pressure (0..1)
    float timbre = 0.0f;           ///< CC 74 (0..1); same value as `slide`
    uint32_t note_id = 0;          ///< Monotonic id assigned on note-on (never 0)
    bool is_upper_zone = false;    ///< true if this note lives in the upper zone
};

/// Tracks MPE voice state across channels.
///
/// Not thread-safe; intended to be owned by a single consumer (usually
/// the plugin's `process()` path or its format-adapter preprocessor).
class MpeVoiceTracker {
public:
    /// Maximum concurrently active notes the tracker can hold.
    static constexpr std::size_t kMaxNotes = 128;

    /// Default member-channel pitch-bend range (MPE spec).
    static constexpr float kDefaultMemberBendSemitones = 48.0f;
    /// Default manager-channel pitch-bend range (MPE spec).
    static constexpr float kDefaultManagerBendSemitones = 2.0f;

    explicit MpeVoiceTracker(MpeConfig config = MpeConfig::standard_lower())
        : config_(config) {}

    // ── Configuration ──────────────────────────────────────────────────────

    void set_config(const MpeConfig& cfg) {
        config_ = cfg;
        reset();
    }
    const MpeConfig& config() const { return config_; }

    /// Set the member-channel pitch-bend range in semitones (per-note bend).
    void set_member_bend_range(float semitones) {
        if (semitones > 0.0f) member_bend_semi_ = semitones;
    }
    /// Set the manager-channel pitch-bend range in semitones (zone-wide bend).
    void set_manager_bend_range(float semitones) {
        if (semitones > 0.0f) manager_bend_semi_ = semitones;
    }
    float member_bend_range() const { return member_bend_semi_; }
    float manager_bend_range() const { return manager_bend_semi_; }

    // ── Event ingestion ────────────────────────────────────────────────────

    /// Process a single MIDI 1.0 event. Returns true if the event belonged
    /// to a known MPE zone (manager or member), false otherwise. A `false`
    /// return indicates the host should treat the event as a plain MIDI
    /// message (no MPE routing).
    bool process(const MidiEvent& event) {
        const uint8_t ch = event.channel();

        // Manager channel — global zone state (pitch bend, pressure, CC74).
        if (ch == config_.lower_zone.manager_channel && config_.lower_zone.member_channels > 0) {
            apply_manager_message(event, /*upper=*/false);
            return true;
        }
        if (ch == config_.upper_zone.manager_channel && config_.upper_zone.member_channels > 0
            && ch != config_.lower_zone.manager_channel) {
            apply_manager_message(event, /*upper=*/true);
            return true;
        }

        const MpeZone* zone = config_.zone_for_channel(ch);
        if (!zone) return false;
        const bool upper = zone->is_upper();

        if (event.is_note_on() && event.velocity() > 0) {
            add_note(ch, event.note(), event.velocity(), upper);
            return true;
        }
        if (event.is_note_off() || (event.is_note_on() && event.velocity() == 0)) {
            remove_note(ch, event.note());
            return true;
        }
        if (event.is_pitch_bend()) {
            const uint16_t raw = static_cast<uint16_t>(event.data()[1]
                | (uint16_t(event.data()[2]) << 7));
            const float norm = (static_cast<float>(raw) - 8192.0f) / 8192.0f;
            update_channel_pitch_bend(ch, norm * member_bend_semi_, upper);
            return true;
        }
        if ((event.data()[0] & 0xF0) == 0xD0) {
            // Channel Pressure (status Dx): 1 data byte.
            const float pressure = event.data()[1] / 127.0f;
            update_channel_pressure(ch, pressure, upper);
            return true;
        }
        if (event.is_cc() && event.cc_number() == 74) {
            const float timbre = event.cc_value() / 127.0f;
            update_channel_timbre(ch, timbre, upper);
            return true;
        }
        return true;  // event belongs to zone but isn't an MPE expression (e.g. other CC)
    }

    // ── Queries ────────────────────────────────────────────────────────────

    std::size_t active_count() const {
        std::size_t n = 0;
        for (const auto& s : notes_) if (s.active) ++n;
        return n;
    }

    /// Find the newest active note for `channel`/`note`, or nullptr.
    const MpeNoteState* find(uint8_t channel, uint8_t note) const {
        const MpeNoteState* best = nullptr;
        uint32_t best_id = 0;
        for (const auto& s : notes_) {
            if (!s.active) continue;
            if (s.channel != channel || s.note != note) continue;
            if (s.note_id > best_id) { best = &s; best_id = s.note_id; }
        }
        return best;
    }

    /// Iterate all active notes. Writes up to `max` records to `out` and
    /// returns the count written.
    std::size_t snapshot(MpeNoteState* out, std::size_t max) const {
        std::size_t n = 0;
        for (const auto& s : notes_) {
            if (!s.active) continue;
            if (n < max) out[n] = s;
            ++n;
        }
        return n;
    }

    /// Direct access to the underlying slots (active flag identifies live
    /// notes). Stable indices across calls.
    const std::array<MpeNoteState, kMaxNotes>& slots() const { return notes_; }

    // ── Zone-wide (manager) state ─────────────────────────────────────────

    struct ZoneState {
        float pitch_bend_semitones = 0.0f;  ///< Zone-wide pitch bend
        float pressure = 0.0f;              ///< Zone-wide pressure
        float timbre = 0.0f;                ///< Zone-wide timbre (CC 74)
    };
    const ZoneState& lower_zone_state() const { return lower_zone_state_; }
    const ZoneState& upper_zone_state() const { return upper_zone_state_; }

    // ── Callbacks (host-thread setup; invoked inline during process()) ────

    std::function<void(const MpeNoteState&)> on_note_on;
    std::function<void(const MpeNoteState&)> on_note_off;
    std::function<void(const MpeNoteState&)> on_pitch_bend;
    std::function<void(const MpeNoteState&)> on_pressure;
    std::function<void(const MpeNoteState&)> on_timbre;

    // ── Lifecycle ──────────────────────────────────────────────────────────

    void reset() {
        for (auto& s : notes_) s = {};
        channel_pitch_bend_.fill(0.0f);
        channel_pressure_.fill(0.0f);
        channel_timbre_.fill(0.0f);
        lower_zone_state_ = {};
        upper_zone_state_ = {};
        next_note_id_ = 1;
    }

private:
    void add_note(uint8_t ch, uint8_t note, uint8_t velocity, bool upper) {
        // Reuse existing slot if one matches (retrigger).
        for (auto& s : notes_) {
            if (s.active && s.channel == ch && s.note == note) {
                s.velocity = velocity;
                s.note_id = next_note_id_++;
                s.is_upper_zone = upper;
                if (on_note_on) on_note_on(s);
                return;
            }
        }
        for (auto& s : notes_) {
            if (s.active) continue;
            s = MpeNoteState{};
            s.active = true;
            s.channel = ch;
            s.note = note;
            s.velocity = velocity;
            s.note_id = next_note_id_++;
            s.is_upper_zone = upper;
            // Seed with current per-channel expression so freshly-added
            // notes inherit any running MPE state.
            s.pitch_bend_semitones = channel_pitch_bend_[ch];
            s.pressure = channel_pressure_[ch];
            s.timbre = channel_timbre_[ch];
            if (on_note_on) on_note_on(s);
            return;
        }
        // Table full — drop silently (audio-thread policy).
    }

    void remove_note(uint8_t ch, uint8_t note) {
        // Oldest matching note wins (FIFO), matching typical MPE hardware
        // which retriggers with the newest and releases oldest-first.
        MpeNoteState* oldest = nullptr;
        uint32_t oldest_id = UINT32_MAX;
        for (auto& s : notes_) {
            if (!s.active || s.channel != ch || s.note != note) continue;
            if (s.note_id < oldest_id) { oldest = &s; oldest_id = s.note_id; }
        }
        if (!oldest) return;
        MpeNoteState copy = *oldest;
        oldest->active = false;
        if (on_note_off) on_note_off(copy);
    }

    void update_channel_pitch_bend(uint8_t ch, float semitones, bool upper) {
        channel_pitch_bend_[ch] = semitones;
        for (auto& s : notes_) {
            if (!s.active || s.channel != ch) continue;
            s.pitch_bend_semitones = semitones;
            if (on_pitch_bend) on_pitch_bend(s);
        }
        (void)upper;
    }
    void update_channel_pressure(uint8_t ch, float pressure, bool upper) {
        channel_pressure_[ch] = pressure;
        for (auto& s : notes_) {
            if (!s.active || s.channel != ch) continue;
            s.pressure = pressure;
            if (on_pressure) on_pressure(s);
        }
        (void)upper;
    }
    void update_channel_timbre(uint8_t ch, float timbre, bool upper) {
        channel_timbre_[ch] = timbre;
        for (auto& s : notes_) {
            if (!s.active || s.channel != ch) continue;
            s.timbre = timbre;
            if (on_timbre) on_timbre(s);
        }
        (void)upper;
    }

    void apply_manager_message(const MidiEvent& event, bool upper) {
        ZoneState& zs = upper ? upper_zone_state_ : lower_zone_state_;
        if (event.is_pitch_bend()) {
            const uint16_t raw = static_cast<uint16_t>(event.data()[1]
                | (uint16_t(event.data()[2]) << 7));
            const float norm = (static_cast<float>(raw) - 8192.0f) / 8192.0f;
            zs.pitch_bend_semitones = norm * manager_bend_semi_;
        } else if ((event.data()[0] & 0xF0) == 0xD0) {
            zs.pressure = event.data()[1] / 127.0f;
        } else if (event.is_cc() && event.cc_number() == 74) {
            zs.timbre = event.cc_value() / 127.0f;
        }
    }

    MpeConfig config_;
    std::array<MpeNoteState, kMaxNotes> notes_{};
    std::array<float, 16> channel_pitch_bend_{};
    std::array<float, 16> channel_pressure_{};
    std::array<float, 16> channel_timbre_{};
    ZoneState lower_zone_state_{};
    ZoneState upper_zone_state_{};
    float member_bend_semi_ = kDefaultMemberBendSemitones;
    float manager_bend_semi_ = kDefaultManagerBendSemitones;
    uint32_t next_note_id_ = 1;
};

} // namespace pulp::midi
