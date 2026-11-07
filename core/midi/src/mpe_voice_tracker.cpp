#include <pulp/midi/mpe_voice_tracker.hpp>

#include <pulp/midi/message.hpp>
#include <pulp/midi/ump.hpp>

#include <cstdint>
#include <limits>

namespace pulp::midi {

// ── Configuration ──────────────────────────────────────────────────────────

void MpeVoiceTracker::set_config(const MpeConfig& cfg) {
    config_ = cfg;
    reset();
}

void MpeVoiceTracker::set_member_bend_range(float semitones) {
    if (semitones > 0.0f) member_bend_semi_.fill(semitones);
}

void MpeVoiceTracker::set_manager_bend_range(float semitones) {
    if (semitones > 0.0f) manager_bend_semi_.fill(semitones);
}

// ── Event ingestion ────────────────────────────────────────────────────────

bool MpeVoiceTracker::process(const MidiEvent& event) {
    const uint8_t ch = event.channel();

    // Configuration messages are interpreted before zone classification: an
    // MPE Configuration Message arrives on a manager channel whose zone may
    // not exist yet.
    if (follow_config_messages_ && event.is_cc()) track_registered_parameter(event);

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
        update_channel_pitch_bend(ch, norm * member_bend_semi_[ch], upper);
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
            update_channel_pitch_bend(ch, norm * member_bend_semi_[ch], upper);
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
                                      ump_bend_normalised(p.data_32()) * member_bend_semi_[ch]);
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
    MpeNoteGeneration best_id = 0;
    for (const auto& s : notes_) {
        if (!s.active) continue;
        if (s.channel != channel || s.note != note) continue;
        if (s.note_id > best_id) { best = &s; best_id = s.note_id; }
    }
    return best;
}

bool MpeVoiceTracker::advance_note_generation(MpeNoteGeneration next) noexcept {
    if (next == 0 || note_generation_exhausted() || active_count() != 0
        || next < next_note_generation_) {
        return false;
    }
    next_note_generation_ = next;
    return true;
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

void MpeVoiceTracker::reset() noexcept {
    for (auto& s : notes_) s = {};
    channel_pitch_bend_.fill(0.0f);
    channel_pressure_.fill(0.0f);
    channel_timbre_.fill(0.0f);
    lower_zone_state_ = {};
    upper_zone_state_ = {};
    pending_note_offs_.fill(MpeNoteState{});
    pending_note_off_head_ = 0;
    pending_note_off_count_ = 0;
    selection_.fill(ParameterSelection{});
}

// ── Configuration from incoming MIDI ──────────────────────────────────────

namespace {

constexpr uint16_t kRpnPitchBendRange = 0;
constexpr uint16_t kRpnMpeConfiguration = 6;

uint16_t zone_channel_mask(const MpeZone& z) {
    if (z.member_channels == 0) return 0;
    uint16_t mask = static_cast<uint16_t>(1u << (z.manager_channel & 0x0F));
    for (uint8_t c = 0; c < 16; ++c) {
        if (z.contains_channel(c)) mask = static_cast<uint16_t>(mask | (1u << c));
    }
    return mask;
}

} // namespace

void MpeVoiceTracker::track_registered_parameter(const MidiEvent& event) {
    const uint8_t ch = event.channel();
    const uint8_t val = event.cc_value();
    auto& sel = selection_[ch];
    switch (event.cc_number()) {
        case 101:  // RPN MSB: a new MSB invalidates the previously selected LSB.
            sel.is_rpn = true; sel.msb = val; sel.lsb = 127;
            break;
        case 100:
            sel.is_rpn = true; sel.lsb = val;
            break;
        case 99:   // NRPN selection: RPN data entry must not follow it.
        case 98:
            sel.is_rpn = false; sel.msb = 127; sel.lsb = 127;
            break;
        case 6:
            sel.data_msb = val; sel.data_lsb = 0;
            if (sel.is_rpn && !(sel.msb == 127 && sel.lsb == 127)) {
                apply_registered_parameter(ch, static_cast<uint16_t>((sel.msb << 7) | sel.lsb),
                                           sel.data_msb, sel.data_lsb);
            }
            break;
        case 38:
            sel.data_lsb = val;
            if (sel.is_rpn && !(sel.msb == 127 && sel.lsb == 127)) {
                apply_registered_parameter(ch, static_cast<uint16_t>((sel.msb << 7) | sel.lsb),
                                           sel.data_msb, sel.data_lsb);
            }
            break;
        default:
            break;
    }
}

void MpeVoiceTracker::apply_registered_parameter(uint8_t ch, uint16_t param,
                                                 uint8_t data_msb, uint8_t data_lsb) {
    if (param == kRpnPitchBendRange) {
        const float semitones = static_cast<float>(data_msb)
                              + static_cast<float>(data_lsb > 99 ? 99 : data_lsb) / 100.0f;
        if (ch == config_.lower_zone.manager_channel && config_.lower_zone.member_channels > 0) {
            manager_bend_semi_[0] = semitones;
        } else if (ch == config_.upper_zone.manager_channel
                   && config_.upper_zone.member_channels > 0) {
            manager_bend_semi_[1] = semitones;
        } else {
            member_bend_semi_[ch] = semitones;
        }
    } else if (param == kRpnMpeConfiguration && (ch == 0 || ch == 15)) {
        apply_configuration_message(ch, data_msb > 15 ? uint8_t{15} : data_msb);
    }
}

void MpeVoiceTracker::apply_configuration_message(uint8_t manager_channel, uint8_t member_count) {
    const MpeConfig before = config_;
    const bool upper = manager_channel == 15;
    MpeZone& target = upper ? config_.upper_zone : config_.lower_zone;
    MpeZone& other = upper ? config_.lower_zone : config_.upper_zone;

    target = MpeZone{manager_channel, member_count};
    other.manager_channel = upper ? uint8_t{0} : uint8_t{15};
    // Both zones share channels 1-14. The zone being configured wins.
    if (other.member_channels + member_count > 14 && other.member_channels > 0) {
        other.member_channels = member_count >= 14 ? uint8_t{0}
                                                   : static_cast<uint8_t>(14 - member_count);
    }
    if (other.member_channels == 0) other = MpeZone{other.manager_channel, 0};

    // The configured zone, and the other zone if it lost channels, return to
    // default bend ranges and cleared expression state.
    const MpeZone& before_target = upper ? before.upper_zone : before.lower_zone;
    const MpeZone& before_other = upper ? before.lower_zone : before.upper_zone;
    uint16_t reset_mask = zone_channel_mask(before_target) | zone_channel_mask(target);
    if (before_other.member_channels != other.member_channels) {
        reset_mask |= zone_channel_mask(before_other) | zone_channel_mask(other);
    }
    for (uint8_t c = 0; c < 16; ++c) {
        if (!(reset_mask & (1u << c))) continue;
        member_bend_semi_[c] = kDefaultMemberBendSemitones;
        channel_pitch_bend_[c] = 0.0f;
        channel_pressure_[c] = 0.0f;
        channel_timbre_[c] = 0.0f;
    }
    manager_bend_semi_[upper ? 1 : 0] = kDefaultManagerBendSemitones;
    (upper ? upper_zone_state_ : lower_zone_state_) = {};
    if (before_other.member_channels != other.member_channels) {
        manager_bend_semi_[upper ? 0 : 1] = kDefaultManagerBendSemitones;
        (upper ? lower_zone_state_ : upper_zone_state_) = {};
    }

    release_notes_outside_zones();
    if (on_config_change) on_config_change(config_);
}

void MpeVoiceTracker::release_notes_outside_zones() {
    for (std::size_t i = 0; i < notes_.size(); ++i) {
        if (!notes_[i].active) continue;
        const MpeZone* z = config_.zone_for_channel(notes_[i].channel);
        if (z && z->is_upper() == notes_[i].is_upper_zone) continue;
        remove_note(notes_[i].channel, notes_[i].note);
    }
}

// ── Private helpers ────────────────────────────────────────────────────────

void MpeVoiceTracker::add_note(uint8_t ch, uint8_t note, uint8_t velocity, bool upper) {
    // Exhaustion takes precedence over the ordinary table-full policy so the
    // terminal refusal counter remains reliable even when every slot is live.
    if (note_generation_exhausted()) {
        record_generation_refusal();
        return;
    }
    // A rejected physical release is retained in the fixed pending queue.
    // Refuse starts until the owner flushes that queue: active + pending can
    // then never exceed kMaxNotes, which proves defer_note_off() cannot run out
    // of storage while draining the current block's remaining releases.
    if (pending_note_off_count_ != 0) return;

    // Reuse an existing slot if one matches (retrigger). The old generation
    // must retire before the replacement starts. Prepare both snapshots first
    // so a bounded sink can accept or reject the pair as one transaction.
    for (auto& s : notes_) {
        if (s.active && s.channel == ch && s.note == note) {
            const MpeNoteState retiring = s;
            MpeNoteState replacement = s;
            replacement.velocity = velocity;
            replacement.note_id = next_note_generation_;
            replacement.is_upper_zone = upper;
            replacement.detached = false;
            // Re-seed expression from the per-channel caches, mirroring the
            // fresh-slot branch below. A note that detached (and then saw
            // channel-level controller changes that were withheld while
            // detached) must resume at the *current* channel value on
            // retrigger, not the stale value frozen at detach time.
            replacement.pitch_bend_semitones = channel_pitch_bend_[ch];
            replacement.pressure = channel_pressure_[ch];
            replacement.timbre = channel_timbre_[ch];

            if (!accept_note_lifecycle(&retiring, &replacement)) return;
            const auto generation = take_note_generation();
            if (!generation) return; // guarded by the exhaustion check above
            replacement.note_id = *generation;
            s = replacement;
            if (on_note_off) on_note_off(retiring);
            if (on_note_on) on_note_on(s);
            return;
        }
    }
    for (auto& s : notes_) {
        if (s.active) continue;
        MpeNoteState added{};
        added.active = true;
        added.channel = ch;
        added.note = note;
        added.velocity = velocity;
        added.note_id = next_note_generation_;
        added.is_upper_zone = upper;
        // Seed with current per-channel expression so freshly-added
        // notes inherit any running MPE state.
        added.pitch_bend_semitones = channel_pitch_bend_[ch];
        added.pressure = channel_pressure_[ch];
        added.timbre = channel_timbre_[ch];

        if (!accept_note_lifecycle(nullptr, &added)) return;
        const auto generation = take_note_generation();
        if (!generation) return; // guarded by the exhaustion check above
        added.note_id = *generation;
        s = added;
        if (on_note_on) on_note_on(s);
        return;
    }
    // Table full — drop silently (audio-thread policy).
}

bool MpeVoiceTracker::accept_note_lifecycle(const MpeNoteState* note_off,
                                            const MpeNoteState* note_on) const {
    return !on_note_lifecycle || on_note_lifecycle(note_off, note_on);
}

bool MpeVoiceTracker::defer_note_off(const MpeNoteState& note_off) noexcept {
    if (pending_note_off_count_ >= pending_note_offs_.size()) return false;
    const auto tail = (pending_note_off_head_ + pending_note_off_count_)
        % pending_note_offs_.size();
    pending_note_offs_[tail] = note_off;
    ++pending_note_off_count_;
    return true;
}

bool MpeVoiceTracker::flush_pending_note_offs() {
    while (pending_note_off_count_ != 0) {
        const MpeNoteState& deferred = pending_note_offs_[pending_note_off_head_];
        if (!accept_note_lifecycle(&deferred, nullptr)) return false;
        if (on_note_off) on_note_off(deferred);
        pending_note_offs_[pending_note_off_head_] = {};
        pending_note_off_head_ = (pending_note_off_head_ + 1) % pending_note_offs_.size();
        --pending_note_off_count_;
    }
    pending_note_off_head_ = 0;
    return true;
}

std::optional<MpeNoteGeneration> MpeVoiceTracker::take_note_generation() noexcept {
    if (next_note_generation_ == 0) return std::nullopt;
    const auto generation = next_note_generation_;
    next_note_generation_ = generation == std::numeric_limits<MpeNoteGeneration>::max()
        ? 0
        : generation + 1;
    return generation;
}

void MpeVoiceTracker::record_generation_refusal() noexcept {
    if (refused_note_on_count_ != std::numeric_limits<std::uint64_t>::max()) {
        ++refused_note_on_count_;
    }
}

void MpeVoiceTracker::remove_note(uint8_t ch, uint8_t note) {
    // Oldest matching note wins (FIFO), matching typical MPE hardware
    // which retriggers with the newest and releases oldest-first.
    MpeNoteState* oldest = nullptr;
    MpeNoteGeneration oldest_id = std::numeric_limits<MpeNoteGeneration>::max();
    for (auto& s : notes_) {
        if (!s.active || s.channel != ch || s.note != note) continue;
        if (!oldest || s.note_id < oldest_id) { oldest = &s; oldest_id = s.note_id; }
    }
    if (!oldest) return;
    MpeNoteState copy = *oldest;
    if (!accept_note_lifecycle(&copy, nullptr)) {
        if (defer_note_off(copy)) oldest->active = false;
        return;
    }
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
        zs.pitch_bend_semitones = norm * manager_bend_semi_[upper ? 1 : 0];
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
        zs.pitch_bend_semitones = ump_bend_normalised(p.data_32()) * manager_bend_semi_[upper ? 1 : 0];
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
    // The flag-combination semantics per the MIDI 2.0 UMP spec:
    //   S alone   → reset the currently sounding note immediately
    //   D alone   → detach, leave expression untouched
    //   D + S     → detach takes effect on the currently sounding note
    //               (its state is preserved for its remaining lifecycle);
    //               reset arms for FUTURE notes at the same (channel,
    //               note) index. Pulp does not yet maintain the
    //               armed-for-future-note memory — D+S degrades to
    //               detach-only on the live note here, which matches
    //               the spec for the sounding note. See the MPE skill's
    //               UMP per-note management gotcha before changing this
    //               behavior.
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
    // 0..0xFFFFFFFF with center 0x80000000 → -1..+1.
    // Explicit narrowing cast silences MSVC C4244 — the math runs in
    // double for precision, then we narrow to float to match the
    // declared return type. Was implicit when this body lived in the
    // header (different warning context); explicit now that this file
    // is compiled with MSVC's warning level 4 enabled.
    return static_cast<float>(
        (static_cast<double>(v32) - 2147483648.0) / 2147483648.0);
}

} // namespace pulp::midi
