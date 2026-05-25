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
#include <optional>

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
    /// True after a UMP Per-Note Management message with the "detach
    /// controllers" flag landed on this note: subsequent channel-level
    /// controller updates (pitch bend, pressure, CC74 on the member
    /// channel) no longer write into this note. Per-note targeted
    /// messages (status 0x60 / 0x00 / 0x10) still apply. Cleared when
    /// the slot is reused for a fresh note-on (retrigger included).
    bool detached = false;
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

    void set_config(const MpeConfig& cfg);
    const MpeConfig& config() const { return config_; }

    /// Set the member-channel pitch-bend range in semitones (per-note bend).
    void set_member_bend_range(float semitones);
    /// Set the manager-channel pitch-bend range in semitones (zone-wide bend).
    void set_manager_bend_range(float semitones);
    float member_bend_range() const { return member_bend_semi_; }
    float manager_bend_range() const { return manager_bend_semi_; }

    /// Bind a MIDI 2.0 Assignable Per-Note Controller index (status
    /// 0x10) to per-note timbre. The assignable PNC space is host-
    /// configured per the UMP spec; this hook lets a Pulp plugin
    /// advertise which assignable index it accepts as timbre. Pass
    /// `std::nullopt` to disable assignable-PNC routing entirely
    /// (registered PNC 74, status 0x00, continues to route to timbre
    /// regardless). Range: 0-127.
    void set_assignable_timbre_index(std::optional<uint8_t> cc_index) {
        if (cc_index) assignable_timbre_index_ = static_cast<uint8_t>(*cc_index & 0x7F);
        else assignable_timbre_index_.reset();
    }
    std::optional<uint8_t> assignable_timbre_index() const {
        return assignable_timbre_index_;
    }

    // ── Event ingestion ────────────────────────────────────────────────────

    /// Process a single MIDI 1.0 event. Returns true if the event belonged
    /// to a known MPE zone (manager or member), false otherwise. A `false`
    /// return indicates the host should treat the event as a plain MIDI
    /// message (no MPE routing).
    bool process(const MidiEvent& event);

    /// Process a MIDI 2.0 UMP Channel Voice packet. Returns true when the
    /// packet belongs to a known MPE zone (and was consumed), false
    /// otherwise. MIDI 2.0 per-note pitch bend (status 0x60) is routed
    /// directly to the matching note rather than going via member-channel
    /// state, preserving the full per-note resolution UMP carries.
    ///
    /// Non-Channel-Voice UMP types (Utility, System, SysEx, Data128) are
    /// ignored and return false.
    bool process(const UmpPacket& p);

    // ── Queries ────────────────────────────────────────────────────────────

    std::size_t active_count() const;

    /// Find the newest active note for `channel`/`note`, or nullptr.
    const MpeNoteState* find(uint8_t channel, uint8_t note) const;

    /// Iterate all active notes. Writes up to `max` records to `out` and
    /// returns the count written.
    std::size_t snapshot(MpeNoteState* out, std::size_t max) const;

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

    void reset();

private:
    void add_note(uint8_t ch, uint8_t note, uint8_t velocity, bool upper);
    void remove_note(uint8_t ch, uint8_t note);

    void update_channel_pitch_bend(uint8_t ch, float semitones, bool upper);
    void update_channel_pressure(uint8_t ch, float pressure, bool upper);
    void update_channel_timbre(uint8_t ch, float timbre, bool upper);

    void apply_manager_message(const MidiEvent& event, bool upper);
    void apply_manager_ump(const UmpPacket& p, bool upper);

    // Per-note helpers — MIDI 2.0 UMP per-note pitch bend / per-note CC
    // target a specific (channel, note) pair rather than going via the
    // member-channel cache, so they only update that one note.
    void apply_per_note_pitch_bend(uint8_t ch, uint8_t note, float semitones);
    void apply_per_note_timbre(uint8_t ch, uint8_t note, float timbre);
    // UMP Per-Note Management (status 0xF0): `flags` bit-0 resets per-note
    // controllers to their defaults, bit-1 detaches channel-level
    // controllers from the note (`MpeNoteState::detached`).
    void apply_per_note_management(uint8_t ch, uint8_t note, uint8_t flags);

    static float ump_bend_normalised(uint32_t v32);

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
    std::optional<uint8_t> assignable_timbre_index_;
};

} // namespace pulp::midi
