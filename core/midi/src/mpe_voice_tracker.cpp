#include <pulp/midi/mpe_voice_tracker.hpp>

#include <pulp/midi/message.hpp>
#include <pulp/midi/ump.hpp>

#include <cstdint>

namespace pulp::midi {

// ── Configuration ──────────────────────────────────────────────────────────

void MpeVoiceTracker::set_config(const MpeConfig& cfg) {
    config_ = cfg;
    reset();
}

void MpeVoiceTracker::set_member_bend_range(float semitones) {
    if (semitones > 0.0f) member_bend_semi_ = semitones;
}

void MpeVoiceTracker::set_manager_bend_range(float semitones) {
    if (semitones > 0.0f) manager_bend_semi_ = semitones;
}

// ── Event ingestion ────────────────────────────────────────────────────────

bool MpeVoiceTracker::process(const MidiEvent& event) {
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

bool MpeVoiceTracker::process(const UmpPacket& p) {
    const auto mt = p.message_type();
    if (mt == UmpMessageType::Midi1ChannelVoice) {
        // Reconstruct a MidiEvent and dispatch to the MIDI 1.0 path.
        const uint8_t s = static_cast<uint8_t>((p.words[0] >> 16) & 0xFF);
        const uint8_t d1 = static_cast<uint8_t>((p.words[0] >> 8) & 0xFF);
        const uint8_t d2 = static_cast<uint8_t>(p.words[0] & 0xFF);
        MidiEvent ev{choc::midi::ShortMessage(s, d1, d2), 0, 0.0};
        return process(ev);
    }
    if (mt != UmpMessageType::Midi2ChannelVoice) return false;

    const uint8_t ch = p.channel();
    const uint8_t status = static_cast<uint8_t>((p.words[0] >> 16) & 0xF0);

    // Manager-channel packets: zone-wide state, no notes created.
    if (ch == config_.lower_zone.manager_channel
        && config_.lower_zone.member_channels > 0) {
        apply_manager_ump(p, /*upper=*/false);
        return true;
    }
    if (ch == config_.upper_zone.manager_channel
        && config_.upper_zone.member_channels > 0
        && ch != config_.lower_zone.manager_channel) {
        apply_manager_ump(p, /*upper=*/true);
        return true;
    }

    const MpeZone* zone = config_.zone_for_channel(ch);
    if (!zone) return false;
    const bool upper = zone->is_upper();

    switch (status) {
        case 0x90: {
            const uint16_t v16 = p.velocity_16();
            if (v16 == 0) {
                remove_note(ch, p.note_number());
            } else {
                add_note(ch, p.note_number(),
                         static_cast<uint8_t>(v16 >> 9), upper);
            }
            return true;
        }
        case 0x80:
            remove_note(ch, p.note_number());
            return true;
        case 0xE0: {
            const float norm = ump_bend_normalised(p.data_32());
            update_channel_pitch_bend(ch, norm * member_bend_semi_, upper);
            return true;
        }
        case 0xD0: {
            // Channel Pressure (MIDI 2.0): 32-bit value, top bit sets it to half.
            const float pressure = static_cast<float>(p.data_32()) / 4294967295.0f;
            update_channel_pressure(ch, pressure, upper);
            return true;
        }
        case 0xB0: {
            const uint8_t cc = static_cast<uint8_t>((p.words[0] >> 8) & 0x7F);
            if (cc == 74) {
                const float timbre = static_cast<float>(p.data_32()) / 4294967295.0f;
                update_channel_timbre(ch, timbre, upper);
            }
            return true;
        }
        case 0x60: {  // Per-note pitch bend
            apply_per_note_pitch_bend(ch, p.note_number(),
                                      ump_bend_normalised(p.data_32()) * member_bend_semi_);
            return true;
        }
        case 0x00: {  // Registered per-note CC
            // MIDI 2.0 per-note controller layout: byte 2 is the note
            // number (p.note_number()), byte 3 is the controller index.
            const uint8_t cc = static_cast<uint8_t>(p.words[0] & 0x7F);
            if (cc == 74) {
                apply_per_note_timbre(ch, p.note_number(),
                                      static_cast<float>(p.data_32()) / 4294967295.0f);
            }
            return true;
        }
        case 0x10: {  // Assignable per-note CC
            // The assignable PNC controller index is host-defined; only
            // route to timbre when the plugin has bound an index via
            // set_assignable_timbre_index().
            if (assignable_timbre_index_.has_value()) {
                const uint8_t cc = static_cast<uint8_t>(p.words[0] & 0x7F);
                if (cc == *assignable_timbre_index_) {
                    apply_per_note_timbre(ch, p.note_number(),
                                          static_cast<float>(p.data_32()) / 4294967295.0f);
                }
            }
            return true;
        }
        case 0xF0: {  // Per-note management — reset + detach flags
            const uint8_t flags = static_cast<uint8_t>(p.words[0] & 0xFF);
            apply_per_note_management(ch, p.note_number(), flags);
            return true;
        }
        default:
            return true;  // belongs to zone but we don't map this status
    }
}

// ── Queries ────────────────────────────────────────────────────────────────

std::size_t MpeVoiceTracker::active_count() const {
    std::size_t n = 0;
    for (const auto& s : notes_) if (s.active) ++n;
    return n;
}

const MpeNoteState* MpeVoiceTracker::find(uint8_t channel, uint8_t note) const {
    const MpeNoteState* best = nullptr;
    uint32_t best_id = 0;
    for (const auto& s : notes_) {
        if (!s.active) continue;
        if (s.channel != channel || s.note != note) continue;
        if (s.note_id > best_id) { best = &s; best_id = s.note_id; }
    }
    return best;
}

std::size_t MpeVoiceTracker::snapshot(MpeNoteState* out, std::size_t max) const {
    std::size_t n = 0;
    for (const auto& s : notes_) {
        if (!s.active) continue;
        if (n < max) out[n] = s;
        ++n;
    }
    return n;
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

void MpeVoiceTracker::reset() {
    for (auto& s : notes_) s = {};
    channel_pitch_bend_.fill(0.0f);
    channel_pressure_.fill(0.0f);
    channel_timbre_.fill(0.0f);
    lower_zone_state_ = {};
    upper_zone_state_ = {};
    next_note_id_ = 1;
}

// ── Private helpers ────────────────────────────────────────────────────────

void MpeVoiceTracker::add_note(uint8_t ch, uint8_t note, uint8_t velocity, bool upper) {
    // Reuse existing slot if one matches (retrigger). Re-attach the
    // slot so channel-level controllers resume affecting it.
    for (auto& s : notes_) {
        if (s.active && s.channel == ch && s.note == note) {
            s.velocity = velocity;
            s.note_id = next_note_id_++;
            s.is_upper_zone = upper;
            s.detached = false;
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

void MpeVoiceTracker::remove_note(uint8_t ch, uint8_t note) {
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

void MpeVoiceTracker::update_channel_pitch_bend(uint8_t ch, float semitones, bool upper) {
    // The per-channel cache is updated regardless of detach so that
    // *fresh* notes added to the same channel after a detach still
    // inherit the running channel state (add_note seeds from it).
    channel_pitch_bend_[ch] = semitones;
    for (auto& s : notes_) {
        if (!s.active || s.channel != ch || s.detached) continue;
        s.pitch_bend_semitones = semitones;
        if (on_pitch_bend) on_pitch_bend(s);
    }
    (void)upper;
}

void MpeVoiceTracker::update_channel_pressure(uint8_t ch, float pressure, bool upper) {
    channel_pressure_[ch] = pressure;
    for (auto& s : notes_) {
        if (!s.active || s.channel != ch || s.detached) continue;
        s.pressure = pressure;
        if (on_pressure) on_pressure(s);
    }
    (void)upper;
}

void MpeVoiceTracker::update_channel_timbre(uint8_t ch, float timbre, bool upper) {
    channel_timbre_[ch] = timbre;
    for (auto& s : notes_) {
        if (!s.active || s.channel != ch || s.detached) continue;
        s.timbre = timbre;
        if (on_timbre) on_timbre(s);
    }
    (void)upper;
}

void MpeVoiceTracker::apply_manager_message(const MidiEvent& event, bool upper) {
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

void MpeVoiceTracker::apply_manager_ump(const UmpPacket& p, bool upper) {
    ZoneState& zs = upper ? upper_zone_state_ : lower_zone_state_;
    const uint8_t status = static_cast<uint8_t>((p.words[0] >> 16) & 0xF0);
    if (status == 0xE0) {
        zs.pitch_bend_semitones = ump_bend_normalised(p.data_32()) * manager_bend_semi_;
    } else if (status == 0xD0) {
        zs.pressure = static_cast<float>(p.data_32()) / 4294967295.0f;
    } else if (status == 0xB0) {
        const uint8_t cc = static_cast<uint8_t>((p.words[0] >> 8) & 0x7F);
        if (cc == 74) zs.timbre = static_cast<float>(p.data_32()) / 4294967295.0f;
    }
}

// Per-note helpers — MIDI 2.0 UMP per-note pitch bend / per-note CC
// target a specific (channel, note) pair rather than going via the
// member-channel cache, so they only update that one note.
void MpeVoiceTracker::apply_per_note_pitch_bend(uint8_t ch, uint8_t note, float semitones) {
    for (auto& s : notes_) {
        if (!s.active || s.channel != ch || s.note != note) continue;
        s.pitch_bend_semitones = semitones;
        if (on_pitch_bend) on_pitch_bend(s);
    }
}

void MpeVoiceTracker::apply_per_note_timbre(uint8_t ch, uint8_t note, float timbre) {
    for (auto& s : notes_) {
        if (!s.active || s.channel != ch || s.note != note) continue;
        s.timbre = timbre;
        if (on_timbre) on_timbre(s);
    }
}

void MpeVoiceTracker::apply_per_note_management(uint8_t ch, uint8_t note, uint8_t flags) {
    // UMP Per-Note Management has two bit flags:
    //   bit 0 (kPerNoteResetControllers, S) — reset per-note expression.
    //   bit 1 (kPerNoteDetachControllers, D) — detach channel-level
    //     controllers from the currently sounding note (its expression
    //     state is frozen for the rest of its lifecycle).
    //
    // The flag-combination semantics per the UMP spec
    // (Codex P1 on #2860, MIDI 2.0 spec § Per-Note Management):
    //   S alone   → reset the currently sounding note immediately
    //   D alone   → detach, leave expression untouched
    //   D + S     → detach takes effect on the currently sounding note
    //               (its state is preserved for its remaining lifecycle);
    //               reset arms for FUTURE notes at the same (channel,
    //               note) index. Pulp does not yet maintain the
    //               armed-for-future-note memory — D+S degrades to
    //               detach-only on the live note here, which matches
    //               the spec for the sounding note. The "arm reset for
    //               future" slice is tracked as a deferred follow-up
    //               in the macOS plan; see SKILL.md "UMP per-note
    //               management" gotchas.
    const bool reset_controllers = (flags & UmpPacket::kPerNoteResetControllers) != 0;
    const bool detach_controllers = (flags & UmpPacket::kPerNoteDetachControllers) != 0;
    const bool reset_live_note = reset_controllers && !detach_controllers;
    for (auto& s : notes_) {
        if (!s.active || s.channel != ch || s.note != note) continue;
        if (reset_live_note) {
            s.pitch_bend_semitones = 0.0f;
            s.pressure = 0.0f;
            s.timbre = 0.0f;
            if (on_pitch_bend) on_pitch_bend(s);
            if (on_pressure) on_pressure(s);
            if (on_timbre) on_timbre(s);
        }
        if (detach_controllers) {
            s.detached = true;
        }
    }
}

float MpeVoiceTracker::ump_bend_normalised(uint32_t v32) {
    // 0..0xFFFFFFFF with centre 0x80000000 → -1..+1.
    // Explicit narrowing cast silences MSVC C4244 — the math runs in
    // double for precision, then we narrow to float to match the
    // declared return type. Was implicit when this body lived in the
    // header (different warning context); explicit now that it's in
    // a .cpp compiled at /W4.
    return static_cast<float>(
        (static_cast<double>(v32) - 2147483648.0) / 2147483648.0);
}

} // namespace pulp::midi
