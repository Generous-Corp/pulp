#pragma once

#include <pulp/midi/utility_contract.hpp>

namespace pulp::midi {

namespace routing_detail {

inline UmpPacket note_off_packet(bool midi2, std::uint8_t group,
                                 std::uint8_t channel, std::uint8_t note) noexcept {
    if (midi2) return UmpPacket::note_off_2(group, channel, note);
    UmpPacket packet;
    packet.word_count = 1;
    packet.words[0] = (std::uint32_t{0x2} << 28) |
                      (static_cast<std::uint32_t>(group & 0x0f) << 24) |
                      (static_cast<std::uint32_t>(0x80 | (channel & 0x0f)) << 16) |
                      (static_cast<std::uint32_t>(note & 0x7f) << 8);
    return packet;
}

class HeldNoteLedger {
  public:
    static constexpr std::size_t kMaxTrackedOwnership = 4096;

    constexpr bool empty() const noexcept { return total_ == 0; }
    bool output_ownership_empty() const noexcept {
        if (midi_debt_total_ != 0 || ump_debt_total_ != 0)
            return false;
        return !has_forwarded(midi_entries_, midi_size_) &&
               !has_forwarded(ump_entries_, ump_size_);
    }
    void discard_input_ownership() const noexcept { clear_all(); }

    bool can_forward(const MidiEvent& event) const noexcept {
        if (!is_attack(event)) return true;
        const auto key = utility_detail::key_index(event.channel(), event.note());
        return midi_overflow_suppressed_[key] == 0 && midi_size_ < midi_entries_.size();
    }
    bool can_forward(const UmpPacket& packet) const noexcept {
        if (!is_attack(packet)) return true;
        const auto key = ump_key(packet);
        return ump_overflow_suppressed_[key] == 0 && ump_size_ < ump_entries_.size();
    }

    bool consume_suppressed_release(const MidiEvent& event) noexcept {
        if (!is_release(event)) return false;
        return consume_suppressed(midi_entries_, midi_size_, midi_flushed_suppressed_,
                                  midi_overflow_suppressed_,
                                  utility_detail::key_index(event.channel(), event.note()));
    }
    bool consume_suppressed_release(const UmpPacket& packet) noexcept {
        if (!is_release(packet)) return false;
        return consume_suppressed(ump_entries_, ump_size_, ump_flushed_suppressed_,
                                  ump_overflow_suppressed_,
                                  ump_key(packet));
    }

    void record(const MidiEvent& event, bool forwarded) noexcept {
        const auto key = utility_detail::key_index(event.channel(), event.note());
        if (is_attack(event))
            record_attack(midi_entries_, midi_size_, midi_overflow_suppressed_, key,
                          forwarded);
        else if (is_release(event))
            record_release(midi_entries_, midi_size_, midi_release_debt_,
                           midi_debt_total_, key, forwarded);
    }
    void record(const UmpPacket& packet, bool forwarded) noexcept {
        const auto key = ump_key(packet);
        if (is_attack(packet))
            record_attack(ump_entries_, ump_size_, ump_overflow_suppressed_, key,
                          forwarded);
        else if (is_release(packet))
            record_release(ump_entries_, ump_size_, ump_release_debt_,
                           ump_debt_total_, key, forwarded);
    }

    void retain_unprocessed_ownership(const MidiBuffer& input) noexcept {
        for (const auto& event : input) {
            if (!consume_suppressed_release(event)) record(event, false);
        }
        if (const auto* ump = input.ump())
            for (const auto& event : *ump) {
                if (!consume_suppressed_release(event.packet))
                    record(event.packet, false);
            }
    }

    template <typename Emit>
    bool drain_midi_releases(Emit&& emit) const noexcept {
        return drain_pending(midi_release_debt_, midi_debt_total_, midi_drain_cursor_,
                             [&](std::size_t key) {
                                 return emit(static_cast<std::uint8_t>(key / 128),
                                             static_cast<std::uint8_t>(key % 128));
                             });
    }

    template <typename Emit>
    bool drain_ump_releases(Emit&& emit) const noexcept {
        return drain_pending(ump_release_debt_, ump_debt_total_, ump_drain_cursor_,
                             [&](std::size_t key) {
                                 constexpr std::size_t kKeysPerProtocol = 16 * 16 * 128;
                                 const bool midi2 = key >= kKeysPerProtocol;
                                 const auto protocol_key = key % kKeysPerProtocol;
                                 const auto group = static_cast<std::uint8_t>(
                                     protocol_key / (16 * 128));
                                 const auto remainder = protocol_key % (16 * 128);
                                 return emit(
                                     midi2, group,
                                     static_cast<std::uint8_t>(remainder / 128),
                                     static_cast<std::uint8_t>(remainder % 128));
                             });
    }

    template <typename EmitMidi, typename EmitUmp>
    bool flush(EmitMidi&& emit_midi, EmitUmp&& emit_ump) const noexcept {
        if (!drain_midi_releases(emit_midi) || !drain_ump_releases(emit_ump))
            return false;
        if (!flush_forwarded(midi_entries_, midi_size_, midi_flushed_suppressed_,
                             [&](std::uint32_t key) {
                return emit_midi(static_cast<std::uint8_t>(key / 128),
                                 static_cast<std::uint8_t>(key % 128));
            }))
            return false;
        if (!flush_forwarded(ump_entries_, ump_size_, ump_flushed_suppressed_,
                             [&](std::uint32_t key) {
                constexpr std::size_t kKeysPerProtocol = 16 * 16 * 128;
                const bool midi2 = key >= kKeysPerProtocol;
                const auto protocol_key = key % kKeysPerProtocol;
                const auto group = static_cast<std::uint8_t>(protocol_key / (16 * 128));
                const auto remainder = protocol_key % (16 * 128);
                return emit_ump(midi2, group,
                                static_cast<std::uint8_t>(remainder / 128),
                                static_cast<std::uint8_t>(remainder % 128));
            }))
            return false;
        return output_ownership_empty();
    }

  private:
    enum class OwnershipState : std::uint8_t {
        Forwarded,
        Suppressed,
    };
    struct OwnershipEntry {
        std::uint32_t key = 0;
        std::uint32_t count = 0;
        OwnershipState state = OwnershipState::Suppressed;
    };

    static std::size_t ump_key(const UmpPacket& packet) noexcept {
        constexpr std::size_t kKeysPerProtocol = 16 * 16 * 128;
        const std::size_t protocol =
            packet.message_type() == UmpMessageType::Midi2ChannelVoice ? 1 : 0;
        return protocol * kKeysPerProtocol +
               (static_cast<std::size_t>(packet.group()) * 16 + packet.channel()) * 128 +
               packet.note_number();
    }
    static bool is_attack(const MidiEvent& event) noexcept {
        return event.is_note_on() && event.velocity() != 0;
    }
    static bool is_release(const MidiEvent& event) noexcept {
        return event.is_note_off() || (event.is_note_on() && event.velocity() == 0);
    }
    static bool is_attack(const UmpPacket& packet) noexcept {
        if (!utility_detail::is_channel_voice(packet)) return false;
        const auto status = static_cast<std::uint8_t>(packet.status() & 0xf0);
        if (status != 0x90) return false;
        return packet.message_type() != UmpMessageType::Midi1ChannelVoice ||
               (packet.words[0] & 0x7f) != 0;
    }
    static bool is_release(const UmpPacket& packet) noexcept {
        if (!utility_detail::is_channel_voice(packet)) return false;
        const auto status = static_cast<std::uint8_t>(packet.status() & 0xf0);
        return status == 0x80 ||
               (status == 0x90 && packet.message_type() == UmpMessageType::Midi1ChannelVoice &&
                (packet.words[0] & 0x7f) == 0);
    }
    template <std::size_t N, std::size_t KeyCount>
    bool consume_suppressed(std::array<OwnershipEntry, N>& entries,
                            std::size_t& size,
                            std::array<std::uint64_t, KeyCount>& flushed,
                            std::array<std::uint32_t, KeyCount>& overflow,
                            std::uint32_t key) noexcept {
        if (flushed[key] != 0) {
            --flushed[key];
            --total_;
            return true;
        }
        for (std::size_t i = 0; i < size; ++i) {
            if (entries[i].key != key) continue;
            if (entries[i].state == OwnershipState::Forwarded)
                return false;
            if (--entries[i].count == 0) erase(entries, size, i);
            --total_;
            return true;
        }
        if (overflow[key] == 0) return false;
        --overflow[key];
        --total_;
        return true;
    }

    template <std::size_t N, std::size_t KeyCount>
    void record_attack(std::array<OwnershipEntry, N>& entries, std::size_t& size,
                       std::array<std::uint32_t, KeyCount>& overflow,
                       std::uint32_t key, bool forwarded) noexcept {
        if (overflow[key] != 0 || size == entries.size()) {
            if (overflow[key] != std::numeric_limits<std::uint32_t>::max()) {
                ++overflow[key];
                ++total_;
            }
            return;
        }
        const auto state = forwarded ? OwnershipState::Forwarded
                                     : OwnershipState::Suppressed;
        for (std::size_t i = size; i != 0; --i) {
            auto& previous = entries[i - 1];
            if (previous.key != key) continue;
            if (previous.state == state &&
                previous.count != std::numeric_limits<std::uint32_t>::max()) {
                ++previous.count;
                ++total_;
                return;
            }
            break;
        }
        entries[size++] = {key, 1, state};
        ++total_;
    }

    template <std::size_t N, std::size_t KeyCount>
    void record_release(std::array<OwnershipEntry, N>& entries,
                        std::size_t& size,
                        std::array<std::uint32_t, KeyCount>& debt,
                        std::size_t& debt_total, std::uint32_t key,
                        bool forwarded) noexcept {
        for (std::size_t i = 0; i < size; ++i) {
            auto& entry = entries[i];
            if (entry.key != key || entry.state != OwnershipState::Forwarded)
                continue;
            if (--entry.count == 0) erase(entries, size, i);
            if (forwarded)
                --total_;
            else if (debt[key] != std::numeric_limits<std::uint32_t>::max()) {
                ++debt[key];
                ++debt_total;
            }
            return;
        }
    }

    template <std::size_t N, typename Emit>
    bool drain_pending(std::array<std::uint32_t, N>& debt,
                       std::size_t& debt_total, std::size_t& cursor,
                       Emit&& emit) const noexcept {
        if (debt_total == 0) return true;
        constexpr std::size_t kAttemptsPerBlock = 32;
        std::size_t attempts = 0;
        std::size_t visited = 0;
        while (visited < debt.size() && attempts < kAttemptsPerBlock) {
            const auto key = cursor;
            cursor = (cursor + 1) % debt.size();
            ++visited;
            if (debt[key] == 0) continue;
            ++attempts;
            if (emit(key)) {
                --debt[key];
                --debt_total;
                --total_;
            }
        }
        return debt_total == 0;
    }

    template <std::size_t N, std::size_t KeyCount, typename Emit>
    bool flush_forwarded(std::array<OwnershipEntry, N>& entries,
                         std::size_t& size,
                         std::array<std::uint64_t, KeyCount>& flushed,
                         Emit&& emit) const noexcept {
        constexpr std::size_t kAttemptsPerCall = 32;
        std::size_t attempts = 0;
        for (std::size_t i = 0; i < size;) {
            auto& entry = entries[i];
            if (entry.state == OwnershipState::Suppressed) {
                ++i;
                continue;
            }
            while (entry.count != 0 && attempts < kAttemptsPerCall) {
                if (flushed[entry.key] == std::numeric_limits<std::uint64_t>::max())
                    return false;
                ++attempts;
                if (!emit(entry.key))
                    return false;
                --entry.count;
                ++flushed[entry.key];
            }
            if (entry.count != 0)
                return false;
            erase(entries, size, i);
        }
        return true;
    }

    template <std::size_t N>
    static bool has_forwarded(const std::array<OwnershipEntry, N>& entries,
                              std::size_t size) noexcept {
        for (std::size_t i = 0; i < size; ++i)
            if (entries[i].state == OwnershipState::Forwarded)
                return true;
        return false;
    }

    void clear_all() const noexcept {
        midi_entries_.fill({});
        ump_entries_.fill({});
        midi_overflow_suppressed_.fill(0);
        ump_overflow_suppressed_.fill(0);
        midi_flushed_suppressed_.fill(0);
        ump_flushed_suppressed_.fill(0);
        midi_release_debt_.fill(0);
        ump_release_debt_.fill(0);
        midi_size_ = 0;
        ump_size_ = 0;
        midi_drain_cursor_ = 0;
        ump_drain_cursor_ = 0;
        midi_debt_total_ = 0;
        ump_debt_total_ = 0;
        total_ = 0;
    }

    template <std::size_t N>
    static void erase(std::array<OwnershipEntry, N>& entries,
                      std::size_t& size, std::size_t index) noexcept {
        for (std::size_t i = index + 1; i < size; ++i)
            entries[i - 1] = entries[i];
        --size;
    }

    mutable std::array<OwnershipEntry, kMaxTrackedOwnership> midi_entries_{};
    mutable std::array<OwnershipEntry, kMaxTrackedOwnership> ump_entries_{};
    mutable std::array<std::uint32_t, 16 * 128> midi_overflow_suppressed_{};
    mutable std::array<std::uint32_t, 2 * 16 * 16 * 128> ump_overflow_suppressed_{};
    mutable std::array<std::uint64_t, 16 * 128> midi_flushed_suppressed_{};
    mutable std::array<std::uint64_t, 2 * 16 * 16 * 128> ump_flushed_suppressed_{};
    mutable std::array<std::uint32_t, 16 * 128> midi_release_debt_{};
    mutable std::array<std::uint32_t, 2 * 16 * 16 * 128> ump_release_debt_{};
    mutable std::size_t midi_size_ = 0;
    mutable std::size_t ump_size_ = 0;
    mutable std::size_t midi_drain_cursor_ = 0;
    mutable std::size_t ump_drain_cursor_ = 0;
    mutable std::size_t midi_debt_total_ = 0;
    mutable std::size_t ump_debt_total_ = 0;
    mutable std::size_t total_ = 0;
};

} // namespace routing_detail

struct ChannelRouteSpec {
    std::uint16_t accepted_channels = 0xffff;
    std::array<std::uint8_t, 16> output_channel{0, 1, 2,  3,  4,  5,  6,  7,
                                                8, 9, 10, 11, 12, 13, 14, 15};
};

class ChannelRouter {
  public:
    static constexpr MidiUtilityContract contract() noexcept {
        return {2, routing_detail::HeldNoteLedger::kMaxTrackedOwnership,
                MidiUtilityOverflowPolicy::RetainReleaseDebt,
                MidiUtilitySameSampleOrder::InputStable, MidiUtilityTransportRequirement::None};
    }

    static constexpr bool valid_spec(const ChannelRouteSpec& spec) noexcept {
        for (const auto channel : spec.output_channel)
            if (channel > 15)
                return false;
        return true;
    }
    explicit constexpr ChannelRouter(ChannelRouteSpec spec = {}) noexcept
        : spec_(spec), valid_(valid_spec(spec)) {}
    constexpr bool valid() const noexcept {
        return valid_;
    }
    constexpr bool replace_spec(ChannelRouteSpec spec) noexcept {
        if (!valid_spec(spec) || !held_notes_.empty())
            return false;
        spec_ = spec;
        valid_ = true;
        return true;
    }

    MidiUtilityProcessReport flush(MidiBuffer& output) const noexcept {
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (held_notes_.output_ownership_empty())
            return report;
        if (!utility_detail::ready(output))
            return {0, 0, 1, false};
        report.complete = held_notes_.flush(
            [&](std::uint8_t channel, std::uint8_t note) {
                return utility_detail::emit(
                    output,
                    utility_detail::with_channel(
                        MidiEvent::note_off(channel, note), spec_.output_channel[channel]),
                    report);
            },
            [&](bool midi2, std::uint8_t group, std::uint8_t channel,
                std::uint8_t note) {
                const bool emitted = utility_detail::emit_ump(
                    output.ump(),
                    {routing_detail::note_off_packet(
                         midi2, group, spec_.output_channel[channel], note),
                     0});
                if (!emitted) {
                    ++report.dropped;
                    report.complete = false;
                }
                return emitted;
            });
        if (!report.complete)
            ++report.deferred;
        return report;
    }

    MidiUtilityProcessReport reset(MidiBuffer& output) const noexcept {
        auto report = flush(output);
        if (report.complete)
            held_notes_.discard_input_ownership();
        return report;
    }

    MidiUtilityProcessReport replace_spec(ChannelRouteSpec spec, MidiBuffer& output) noexcept {
        if (!valid_spec(spec)) {
            utility_detail::clear_output(output);
            return {0, 0, 0, false};
        }
        auto report = flush(output);
        if (report.complete) {
            spec_ = spec;
            valid_ = true;
        }
        return report;
    }

    MidiUtilityProcessReport process(const MidiBuffer& input, MidiBuffer& output) const {
        if (utility_detail::blocks_alias(input, output))
            return {0, input.size(), 0, false};
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output)) {
            held_notes_.retain_unprocessed_ownership(input);
            return {0, input.size(), 0, false};
        }
        if (!valid_) {
            held_notes_.retain_unprocessed_ownership(input);
            return {0, input.size(), 0, false};
        }
        const bool midi_drained = held_notes_.drain_midi_releases(
            [&](std::uint8_t channel, std::uint8_t note) {
                return utility_detail::emit(
                    output,
                    utility_detail::with_channel(
                        MidiEvent::note_off(channel, note), spec_.output_channel[channel]),
                    report);
            });
        const bool ump_drained = held_notes_.drain_ump_releases(
            [&](bool midi2, std::uint8_t group, std::uint8_t channel,
                std::uint8_t note) {
                const bool emitted = utility_detail::emit_ump(
                    output.ump(),
                    {routing_detail::note_off_packet(
                         midi2, group, spec_.output_channel[channel], note),
                     0});
                if (!emitted) {
                    ++report.dropped;
                    report.complete = false;
                }
                return emitted;
            });
        if (!midi_drained || !ump_drained) {
            held_notes_.retain_unprocessed_ownership(input);
            report.dropped += input.size();
            report.complete = false;
            return report;
        }
        for (const auto& event : input) {
            if (!utility_detail::is_channel_voice(event)) {
                utility_detail::emit(output, event, report);
                continue;
            }
            if (held_notes_.consume_suppressed_release(event))
                continue;
            const auto channel = event.channel();
            if ((spec_.accepted_channels & (std::uint16_t{1} << channel)) == 0) {
                held_notes_.record(event, false);
                continue;
            }
            if (!held_notes_.can_forward(event)) {
                held_notes_.record(event, false);
                ++report.dropped;
                report.complete = false;
                continue;
            }
            const bool forwarded = utility_detail::emit(
                output, utility_detail::with_channel(event, spec_.output_channel[channel]), report);
            held_notes_.record(event, forwarded);
        }
        bool copied_sidecars = utility_detail::copy_sysex_sidecar(input, output);
        if (const auto* source = input.ump()) {
            for (const auto& event : *source) {
                if (!utility_detail::is_channel_voice(event.packet)) {
                    copied_sidecars =
                        utility_detail::emit_ump(output.ump(), event) && copied_sidecars;
                    continue;
                }
                if (held_notes_.consume_suppressed_release(event.packet))
                    continue;
                const auto channel = event.packet.channel();
                if ((spec_.accepted_channels & (std::uint16_t{1} << channel)) == 0) {
                    held_notes_.record(event.packet, false);
                    continue;
                }
                auto routed = event;
                routed.packet =
                    utility_detail::with_channel(event.packet, spec_.output_channel[channel]);
                if (!held_notes_.can_forward(event.packet)) {
                    held_notes_.record(event.packet, false);
                    ++report.dropped;
                    report.complete = false;
                    continue;
                }
                const bool forwarded = utility_detail::emit_ump(output.ump(), routed);
                held_notes_.record(event.packet, forwarded);
                copied_sidecars = forwarded && copied_sidecars;
            }
        }
        if (!copied_sidecars) {
            ++report.dropped;
            report.complete = false;
        }
        return report;
    }

  private:
    ChannelRouteSpec spec_{};
    bool valid_ = true;
    mutable routing_detail::HeldNoteLedger held_notes_{};
};

struct NoteRangeSpec {
    std::uint8_t lowest = 0;
    std::uint8_t highest = 127;
};

class NoteRangeFilter {
  public:
    static constexpr MidiUtilityContract contract() noexcept {
        return {2, routing_detail::HeldNoteLedger::kMaxTrackedOwnership,
                MidiUtilityOverflowPolicy::RetainReleaseDebt,
                MidiUtilitySameSampleOrder::InputStable, MidiUtilityTransportRequirement::None};
    }

    static constexpr bool valid_spec(NoteRangeSpec spec) noexcept {
        return spec.lowest <= 127 && spec.highest <= 127 && spec.lowest <= spec.highest;
    }
    explicit constexpr NoteRangeFilter(NoteRangeSpec spec = {}) noexcept
        : spec_(spec), valid_(valid_spec(spec)) {}
    constexpr bool valid() const noexcept {
        return valid_;
    }
    constexpr bool replace_spec(NoteRangeSpec spec) noexcept {
        if (!valid_spec(spec) || !held_notes_.empty())
            return false;
        spec_ = spec;
        valid_ = true;
        return true;
    }

    MidiUtilityProcessReport flush(MidiBuffer& output) const noexcept {
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (held_notes_.output_ownership_empty())
            return report;
        if (!utility_detail::ready(output))
            return {0, 0, 1, false};
        report.complete = held_notes_.flush(
            [&](std::uint8_t channel, std::uint8_t note) {
                return utility_detail::emit(output, MidiEvent::note_off(channel, note), report);
            },
            [&](bool midi2, std::uint8_t group, std::uint8_t channel,
                std::uint8_t note) {
                const bool emitted = utility_detail::emit_ump(
                    output.ump(),
                    {routing_detail::note_off_packet(midi2, group, channel, note), 0});
                if (!emitted) {
                    ++report.dropped;
                    report.complete = false;
                }
                return emitted;
            });
        if (!report.complete)
            ++report.deferred;
        return report;
    }

    MidiUtilityProcessReport reset(MidiBuffer& output) const noexcept {
        auto report = flush(output);
        if (report.complete)
            held_notes_.discard_input_ownership();
        return report;
    }

    MidiUtilityProcessReport replace_spec(NoteRangeSpec spec, MidiBuffer& output) noexcept {
        if (!valid_spec(spec)) {
            utility_detail::clear_output(output);
            return {0, 0, 0, false};
        }
        auto report = flush(output);
        if (report.complete) {
            spec_ = spec;
            valid_ = true;
        }
        return report;
    }

    MidiUtilityProcessReport process(const MidiBuffer& input, MidiBuffer& output) const {
        if (utility_detail::blocks_alias(input, output))
            return {0, input.size(), 0, false};
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output)) {
            held_notes_.retain_unprocessed_ownership(input);
            return {0, input.size(), 0, false};
        }
        if (!valid_) {
            held_notes_.retain_unprocessed_ownership(input);
            return {0, input.size(), 0, false};
        }
        const bool midi_drained = held_notes_.drain_midi_releases(
            [&](std::uint8_t channel, std::uint8_t note) {
                return utility_detail::emit(
                    output, MidiEvent::note_off(channel, note), report);
            });
        const bool ump_drained = held_notes_.drain_ump_releases(
            [&](bool midi2, std::uint8_t group, std::uint8_t channel,
                std::uint8_t note) {
                const bool emitted = utility_detail::emit_ump(
                    output.ump(),
                    {routing_detail::note_off_packet(midi2, group, channel, note), 0});
                if (!emitted) {
                    ++report.dropped;
                    report.complete = false;
                }
                return emitted;
            });
        if (!midi_drained || !ump_drained) {
            held_notes_.retain_unprocessed_ownership(input);
            report.dropped += input.size();
            report.complete = false;
            return report;
        }
        for (const auto& event : input) {
            if (held_notes_.consume_suppressed_release(event))
                continue;
            if (utility_detail::is_note_addressed(event) &&
                (event.note() < spec_.lowest || event.note() > spec_.highest)) {
                held_notes_.record(event, false);
                continue;
            }
            if (!held_notes_.can_forward(event)) {
                held_notes_.record(event, false);
                ++report.dropped;
                report.complete = false;
                continue;
            }
            const bool forwarded = utility_detail::emit(output, event, report);
            held_notes_.record(event, forwarded);
        }
        bool copied_sidecars = utility_detail::copy_sysex_sidecar(input, output);
        if (const auto* source = input.ump()) {
            for (const auto& event : *source) {
                if (held_notes_.consume_suppressed_release(event.packet))
                    continue;
                if (utility_detail::is_note_addressed(event.packet) &&
                    (event.packet.note_number() < spec_.lowest ||
                     event.packet.note_number() > spec_.highest)) {
                    held_notes_.record(event.packet, false);
                    continue;
                }
                if (!held_notes_.can_forward(event.packet)) {
                    held_notes_.record(event.packet, false);
                    ++report.dropped;
                    report.complete = false;
                    continue;
                }
                const bool forwarded = utility_detail::emit_ump(output.ump(), event);
                held_notes_.record(event.packet, forwarded);
                copied_sidecars = forwarded && copied_sidecars;
            }
        }
        if (!copied_sidecars) {
            ++report.dropped;
            report.complete = false;
        }
        return report;
    }

  private:
    NoteRangeSpec spec_{};
    bool valid_ = true;
    mutable routing_detail::HeldNoteLedger held_notes_{};
};

struct KeyboardSplitSpec {
    std::uint8_t split_note = 60;
    bool split_note_is_upper = true;
    std::uint8_t lower_channel = 0;
    std::uint8_t upper_channel = 1;
};

class KeyboardSplit {
  public:
    static constexpr MidiUtilityContract contract() noexcept {
        return {3, routing_detail::HeldNoteLedger::kMaxTrackedOwnership,
                MidiUtilityOverflowPolicy::RetainReleaseDebt,
                MidiUtilitySameSampleOrder::InputStable, MidiUtilityTransportRequirement::None};
    }

    static constexpr bool valid_spec(KeyboardSplitSpec spec) noexcept {
        return spec.split_note <= 127 && spec.lower_channel <= 15 && spec.upper_channel <= 15;
    }
    explicit constexpr KeyboardSplit(KeyboardSplitSpec spec = {}) noexcept
        : spec_(spec), valid_(valid_spec(spec)) {}
    constexpr bool valid() const noexcept {
        return valid_;
    }
    constexpr bool replace_spec(KeyboardSplitSpec spec) noexcept {
        if (!valid_spec(spec) || !held_notes_.empty())
            return false;
        spec_ = spec;
        valid_ = true;
        return true;
    }

    MidiUtilityProcessReport flush(MidiBuffer& lower, MidiBuffer& upper) const noexcept {
        utility_detail::clear_output(lower);
        utility_detail::clear_output(upper);
        MidiUtilityProcessReport report;
        if (held_notes_.output_ownership_empty())
            return report;
        if (!utility_detail::ready(lower) || !utility_detail::ready(upper))
            return {0, 0, 1, false};
        report.complete = held_notes_.flush(
            [&](std::uint8_t, std::uint8_t note) {
                const bool is_upper =
                    note > spec_.split_note ||
                    (note == spec_.split_note && spec_.split_note_is_upper);
                return utility_detail::emit(
                    is_upper ? upper : lower,
                    MidiEvent::note_off(
                        is_upper ? spec_.upper_channel : spec_.lower_channel, note),
                    report);
            },
            [&](bool midi2, std::uint8_t group, std::uint8_t,
                std::uint8_t note) {
                const bool is_upper =
                    note > spec_.split_note ||
                    (note == spec_.split_note && spec_.split_note_is_upper);
                const bool emitted = utility_detail::emit_ump(
                    is_upper ? upper.ump() : lower.ump(),
                    {routing_detail::note_off_packet(
                         midi2, group,
                         is_upper ? spec_.upper_channel : spec_.lower_channel, note),
                     0});
                if (!emitted) {
                    ++report.dropped;
                    report.complete = false;
                }
                return emitted;
            });
        if (!report.complete)
            ++report.deferred;
        return report;
    }

    MidiUtilityProcessReport reset(MidiBuffer& lower, MidiBuffer& upper) const noexcept {
        auto report = flush(lower, upper);
        if (report.complete)
            held_notes_.discard_input_ownership();
        return report;
    }

    MidiUtilityProcessReport replace_spec(KeyboardSplitSpec spec, MidiBuffer& lower,
                                          MidiBuffer& upper) noexcept {
        if (!valid_spec(spec)) {
            utility_detail::clear_output(lower);
            utility_detail::clear_output(upper);
            return {0, 0, 0, false};
        }
        auto report = flush(lower, upper);
        if (report.complete) {
            spec_ = spec;
            valid_ = true;
        }
        return report;
    }

    MidiUtilityProcessReport process(const MidiBuffer& input, MidiBuffer& lower,
                                     MidiBuffer& upper) const {
        if (utility_detail::blocks_alias(input, lower) ||
            utility_detail::blocks_alias(input, upper) ||
            utility_detail::blocks_alias(lower, upper))
            return {0, input.size(), 0, false};
        utility_detail::clear_output(lower);
        utility_detail::clear_output(upper);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(lower) || !utility_detail::ready(upper)) {
            held_notes_.retain_unprocessed_ownership(input);
            return {0, input.size(), 0, false};
        }
        if (!valid_) {
            held_notes_.retain_unprocessed_ownership(input);
            return {0, input.size(), 0, false};
        }
        const bool midi_drained = held_notes_.drain_midi_releases(
            [&](std::uint8_t channel, std::uint8_t note) {
                const bool is_upper =
                    note > spec_.split_note ||
                    (note == spec_.split_note && spec_.split_note_is_upper);
                return utility_detail::emit(
                    is_upper ? upper : lower,
                    MidiEvent::note_off(
                        is_upper ? spec_.upper_channel : spec_.lower_channel, note),
                    report);
            });
        const bool ump_drained = held_notes_.drain_ump_releases(
            [&](bool midi2, std::uint8_t group, std::uint8_t channel,
                std::uint8_t note) {
                (void)channel;
                const bool is_upper =
                    note > spec_.split_note ||
                    (note == spec_.split_note && spec_.split_note_is_upper);
                const bool emitted = utility_detail::emit_ump(
                    is_upper ? upper.ump() : lower.ump(),
                    {routing_detail::note_off_packet(
                         midi2, group,
                         is_upper ? spec_.upper_channel : spec_.lower_channel, note),
                     0});
                if (!emitted) {
                    ++report.dropped;
                    report.complete = false;
                }
                return emitted;
            });
        if (!midi_drained || !ump_drained) {
            held_notes_.retain_unprocessed_ownership(input);
            report.dropped += input.size();
            report.complete = false;
            return report;
        }
        for (const auto& event : input) {
            if (!utility_detail::is_channel_voice(event)) {
                utility_detail::emit(lower, event, report);
                utility_detail::emit(upper, event, report);
                continue;
            }
            if (utility_detail::is_note_addressed(event)) {
                const bool is_upper =
                    event.note() > spec_.split_note ||
                    (event.note() == spec_.split_note && spec_.split_note_is_upper);
                auto routed = utility_detail::with_channel(event, is_upper ? spec_.upper_channel
                                                                           : spec_.lower_channel);
                if (held_notes_.consume_suppressed_release(event))
                    continue;
                if (!held_notes_.can_forward(event)) {
                    held_notes_.record(event, false);
                    ++report.dropped;
                    report.complete = false;
                    continue;
                }
                const bool forwarded = utility_detail::emit(
                    is_upper ? upper : lower, routed, report);
                held_notes_.record(event, forwarded);
                continue;
            }
            utility_detail::emit(lower, utility_detail::with_channel(event, spec_.lower_channel),
                                 report);
            utility_detail::emit(upper, utility_detail::with_channel(event, spec_.upper_channel),
                                 report);
        }
        bool copied_sidecars = utility_detail::copy_sysex_sidecar(input, lower);
        copied_sidecars = utility_detail::copy_sysex_sidecar(input, upper) && copied_sidecars;
        if (const auto* source = input.ump()) {
            for (const auto& event : *source) {
                if (!utility_detail::is_channel_voice(event.packet)) {
                    copied_sidecars =
                        utility_detail::emit_ump(lower.ump(), event) && copied_sidecars;
                    copied_sidecars =
                        utility_detail::emit_ump(upper.ump(), event) && copied_sidecars;
                    continue;
                }
                if (utility_detail::is_note_addressed(event.packet)) {
                    const bool is_upper = event.packet.note_number() > spec_.split_note ||
                                          (event.packet.note_number() == spec_.split_note &&
                                           spec_.split_note_is_upper);
                    auto routed = event;
                    routed.packet = utility_detail::with_channel(
                        event.packet, is_upper ? spec_.upper_channel : spec_.lower_channel);
                    if (held_notes_.consume_suppressed_release(event.packet))
                        continue;
                    if (!held_notes_.can_forward(event.packet)) {
                        held_notes_.record(event.packet, false);
                        ++report.dropped;
                        report.complete = false;
                        continue;
                    }
                    const bool forwarded = utility_detail::emit_ump(
                        is_upper ? upper.ump() : lower.ump(), routed);
                    held_notes_.record(event.packet, forwarded);
                    copied_sidecars = forwarded && copied_sidecars;
                    continue;
                }
                auto lower_event = event;
                lower_event.packet =
                    utility_detail::with_channel(event.packet, spec_.lower_channel);
                copied_sidecars =
                    utility_detail::emit_ump(lower.ump(), lower_event) && copied_sidecars;
                auto upper_event = event;
                upper_event.packet =
                    utility_detail::with_channel(event.packet, spec_.upper_channel);
                copied_sidecars =
                    utility_detail::emit_ump(upper.ump(), upper_event) && copied_sidecars;
            }
        }
        if (!copied_sidecars) {
            ++report.dropped;
            report.complete = false;
        }
        return report;
    }

  private:
    KeyboardSplitSpec spec_{};
    bool valid_ = true;
    mutable routing_detail::HeldNoteLedger held_notes_{};
};

} // namespace pulp::midi
