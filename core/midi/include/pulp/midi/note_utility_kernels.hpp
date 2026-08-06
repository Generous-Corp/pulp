#pragma once

#include <pulp/midi/utility_contract.hpp>

namespace pulp::midi {

struct NoteLengthSpec {
    std::int64_t length_samples = 1;
};

template <std::size_t MaximumActiveNotes = 128> class NoteLengthShaper {
  public:
    static_assert(MaximumActiveNotes < std::numeric_limits<std::uint32_t>::max());
    static constexpr MidiUtilityContract contract() noexcept {
        return {2, MaximumActiveNotes, MidiUtilityOverflowPolicy::FailOpenBalanced,
                MidiUtilitySameSampleOrder::ReleaseBeforeAttack,
                MidiUtilityTransportRequirement::FlushOnDiscontinuity};
    }

    static constexpr bool valid_spec(NoteLengthSpec spec) noexcept {
        return spec.length_samples > 0;
    }
    explicit constexpr NoteLengthShaper(NoteLengthSpec spec = {}) noexcept
        : spec_(spec), valid_(valid_spec(spec)) {}
    constexpr bool valid() const noexcept {
        return valid_;
    }

    MidiUtilityProcessReport process(const MidiBuffer& input, MidiBuffer& output,
                                     timebase::SamplePosition block_start,
                                     std::int32_t block_samples) noexcept {
        if (utility_detail::blocks_alias(input, output))
            return {0, input.size(), 0, false};
        utility_detail::clear_output(output);
        current_block_start_ = block_start.value;
        const auto block_sample_count = std::max<std::int64_t>(0, block_samples);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output)) {
            account_unprocessed_input(input);
            return {0, input.size(), 0, false};
        }
        if (!valid_ || block_samples < 0) {
            account_unprocessed_input(input);
            return {0, input.size(), 0, false};
        }
        if (!drain_release_debt(output, report)) {
            account_unprocessed_input(input);
            return report;
        }
        emit_due(output, report, block_start);
        for (const auto& event : input) {
            const auto absolute =
                utility_detail::saturating_sample_add(block_start.value, event.sample_offset);
            emit_due(output, report, {absolute});
            const bool is_note = event.is_note_on() || event.is_note_off();
            const int event_key =
                is_note ? utility_detail::key_index(event.channel(), event.note()) : 0;
            if (is_note && quarantined_[event_key]) {
                ++report.dropped;
                report.complete = false;
                continue;
            }
            if (event.is_note_on()) {
                if (!release_matching(event.channel(), event.note(), event.sample_offset, output,
                                      report)) {
                    track_suppressed(event_key);
                    continue;
                }
                Slot* slot = free_slot();
                if (slot == nullptr) {
                    const int key = utility_detail::key_index(event.channel(), event.note());
                    if (passthrough_depth_[key] == std::numeric_limits<std::uint32_t>::max()) {
                        quarantined_[key] = true;
                        ++report.dropped;
                        report.complete = false;
                    } else if (utility_detail::emit(output, event, report)) {
                        ++passthrough_depth_[key];
                    } else {
                        track_suppressed(key);
                    }
                    continue;
                }
                if (!utility_detail::emit(output, event, report)) {
                    track_suppressed(utility_detail::key_index(event.channel(), event.note()));
                    continue;
                }
                if (next_serial_ == std::numeric_limits<std::uint32_t>::max())
                    rebase_slot_serials();
                *slot = {event.channel(),
                         event.note(),
                         event.velocity(),
                         utility_detail::saturating_sample_add(absolute, spec_.length_samples),
                         block_start.value,
                         event.sample_offset,
                         next_serial_++,
                         true};
            } else if (event.is_note_off()) {
                const int key = event_key;
                if (suppressed_depth_[key] != 0) {
                    --suppressed_depth_[key];
                } else if (passthrough_depth_[key] != 0) {
                    --passthrough_depth_[key];
                    if (!utility_detail::emit(output, event, report))
                        track_release_debt(key);
                }
            } else {
                utility_detail::emit(output, event, report);
            }
        }
        const auto last_sample = block_sample_count > 0 ? block_sample_count - 1 : 0;
        emit_due(output, report,
                 {utility_detail::saturating_sample_add(block_start.value, last_sample)});
        utility_detail::copy_sidecars(input, output, report);
        output.sort();
        return report;
    }

    MidiUtilityProcessReport flush(MidiBuffer& output) noexcept {
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output))
            return {0, 0, 0, false};
        if (!drain_release_debt(output, report))
            return report;
        for (auto& slot : slots_) {
            if (!slot.active)
                continue;
            auto off = MidiEvent::note_off(slot.channel, slot.note);
            if (!utility_detail::emit(output, off, report)) {
                ++report.deferred;
                return report;
            }
            slot = {};
        }
        for (int key = 0; key < 16 * 128; ++key) {
            while (passthrough_depth_[key] != 0) {
                if (!utility_detail::emit(output,
                                          MidiEvent::note_off(static_cast<std::uint8_t>(key / 128),
                                                              static_cast<std::uint8_t>(key % 128)),
                                          report)) {
                    ++report.deferred;
                    report.complete = false;
                    return report;
                }
                --passthrough_depth_[key];
            }
        }
        suppressed_depth_.fill(0);
        quarantined_.fill(false);
        if (empty() && pending_spec_) {
            spec_ = *pending_spec_;
            pending_spec_.reset();
        }
        report.complete = empty();
        return report;
    }

    MidiUtilityProcessReport reset(MidiBuffer& output) noexcept {
        return flush(output);
    }

    MidiUtilityProcessReport replace_spec(NoteLengthSpec spec, MidiBuffer& output) noexcept {
        if (!valid_spec(spec)) {
            utility_detail::clear_output(output);
            return {0, 0, 0, false};
        }
        auto report = flush(output);
        if (report.complete)
            spec_ = spec;
        else
            pending_spec_ = spec;
        return report;
    }

    bool empty() const noexcept {
        for (const auto& slot : slots_)
            if (slot.active)
                return false;
        for (const auto debt : release_debt_)
            if (debt != 0)
                return false;
        for (const auto depth : passthrough_depth_)
            if (depth != 0)
                return false;
        for (const bool quarantined : quarantined_)
            if (quarantined)
                return false;
        return true;
    }

  private:
    struct Slot {
        std::uint8_t channel = 0;
        std::uint8_t note = 0;
        std::uint8_t velocity = 0;
        std::int64_t release_sample = 0;
        std::int64_t activation_block_start = 0;
        std::int32_t minimum_release_offset = 0;
        std::uint32_t serial = 0;
        bool active = false;
    };

    template <typename Counter> static bool increment_checked(Counter& value) noexcept {
        if (value != std::numeric_limits<Counter>::max()) {
            ++value;
            return true;
        }
        return false;
    }

    void track_suppressed(int key) noexcept {
        if (!increment_checked(suppressed_depth_[key]))
            quarantined_[key] = true;
    }

    void track_release_debt(int key) noexcept {
        if (!increment_checked(release_debt_[key]))
            quarantined_[key] = true;
    }

    Slot* free_slot() noexcept {
        for (auto& slot : slots_)
            if (!slot.active)
                return &slot;
        return nullptr;
    }

    void rebase_slot_serials() noexcept {
        std::array<std::uint32_t, MaximumActiveNotes> rebased{};
        std::uint32_t rank = 1;
        for (;;) {
            std::size_t oldest_index = MaximumActiveNotes;
            for (std::size_t i = 0; i < MaximumActiveNotes; ++i) {
                if (!slots_[i].active || rebased[i] != 0)
                    continue;
                if (oldest_index == MaximumActiveNotes ||
                    slots_[i].serial < slots_[oldest_index].serial)
                    oldest_index = i;
            }
            if (oldest_index == MaximumActiveNotes)
                break;
            rebased[oldest_index] = rank++;
        }
        for (std::size_t i = 0; i < MaximumActiveNotes; ++i)
            slots_[i].serial = rebased[i];
        next_serial_ = rank;
    }

    bool release_matching(std::uint8_t channel, std::uint8_t note, std::int32_t offset,
                          MidiBuffer& output, MidiUtilityProcessReport& report) noexcept {
        for (;;) {
            Slot* oldest = nullptr;
            for (auto& slot : slots_) {
                if (slot.active && slot.channel == channel && slot.note == note &&
                    (oldest == nullptr || slot.serial < oldest->serial))
                    oldest = &slot;
            }
            if (oldest == nullptr)
                return true;
            auto off = utility_detail::at(MidiEvent::note_off(channel, note), offset);
            if (!utility_detail::emit(output, off, report))
                return false;
            *oldest = {};
        }
    }

    void emit_due(MidiBuffer& output, MidiUtilityProcessReport& report,
                  timebase::SamplePosition until) noexcept {
        for (;;) {
            Slot* due = nullptr;
            for (auto& slot : slots_) {
                if (!slot.active || slot.release_sample > until.value)
                    continue;
                if (due == nullptr || slot.release_sample < due->release_sample ||
                    (slot.release_sample == due->release_sample && slot.serial < due->serial))
                    due = &slot;
            }
            if (due == nullptr)
                break;
            std::int32_t offset = release_offset(*due);
            auto off = utility_detail::at(MidiEvent::note_off(due->channel, due->note), offset);
            if (!utility_detail::emit(output, off, report)) {
                ++report.deferred;
                break;
            }
            *due = {};
        }
        if (empty() && pending_spec_) {
            spec_ = *pending_spec_;
            pending_spec_.reset();
        }
    }

    std::int32_t release_offset(const Slot& slot) const noexcept {
        std::uint64_t distance = 0;
        if (slot.release_sample > current_block_start_) {
            distance = static_cast<std::uint64_t>(slot.release_sample) -
                       static_cast<std::uint64_t>(current_block_start_);
        }
        auto offset = static_cast<std::int32_t>(std::min<std::uint64_t>(
            distance, static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())));
        if (slot.activation_block_start == current_block_start_)
            offset = std::max(offset, slot.minimum_release_offset);
        return offset;
    }

    bool drain_release_debt(MidiBuffer& output, MidiUtilityProcessReport& report) noexcept {
        for (int key = 0; key < 16 * 128; ++key) {
            while (release_debt_[key] != 0) {
                if (!utility_detail::emit(output,
                                          MidiEvent::note_off(static_cast<std::uint8_t>(key / 128),
                                                              static_cast<std::uint8_t>(key % 128)),
                                          report)) {
                    ++report.deferred;
                    return false;
                }
                --release_debt_[key];
            }
        }
        return true;
    }

    void account_unprocessed_input(const MidiBuffer& input) noexcept {
        for (const auto& event : input) {
            if (!event.is_note_on() && !event.is_note_off())
                continue;
            const int key = utility_detail::key_index(event.channel(), event.note());
            if (quarantined_[key])
                continue;
            if (event.is_note_on()) {
                track_suppressed(key);
            } else if (event.is_note_off()) {
                if (suppressed_depth_[key] != 0)
                    --suppressed_depth_[key];
                else if (passthrough_depth_[key] != 0) {
                    --passthrough_depth_[key];
                    track_release_debt(key);
                }
            }
        }
    }

    NoteLengthSpec spec_{};
    bool valid_ = true;
    std::optional<NoteLengthSpec> pending_spec_;
    std::array<Slot, MaximumActiveNotes> slots_{};
    std::array<std::uint32_t, 16 * 128> passthrough_depth_{};
    std::array<std::uint32_t, 16 * 128> suppressed_depth_{};
    std::array<std::uint32_t, 16 * 128> release_debt_{};
    std::array<bool, 16 * 128> quarantined_{};
    std::uint32_t next_serial_ = 1;
    std::int64_t current_block_start_ = 0;
};

enum class MonophonicPriority : std::uint8_t { Low, High, Last };

struct MonophonicSpec {
    MonophonicPriority priority = MonophonicPriority::Last;
    bool legato = true;
    double glide_seconds = 0.0;
};

struct MonophonicPitchState {
    bool active = false;
    bool legato = false;
    bool glide_active = false;
    std::uint8_t channel = 0;
    std::uint8_t note = 0;
    float current_note = 0.0f;
    float target_note = 0.0f;
};

template <typename Serial>
inline constexpr bool monophonic_serial_capacity_supported = std::is_unsigned_v<Serial> &&
                                                             std::numeric_limits<Serial>::max() >
                                                                 static_cast<std::uint64_t>(16 *
                                                                                            128);

template <typename Serial = std::uint32_t> class BasicMonophonicNoteSelector {
  public:
    static_assert(monophonic_serial_capacity_supported<Serial>,
                  "Serial must represent every held MIDI key plus the next-order sentinel");
    static constexpr MidiUtilityContract contract() noexcept {
        return {2, 16 * 128, MidiUtilityOverflowPolicy::RetainReleaseDebt,
                MidiUtilitySameSampleOrder::ReleaseBeforeAttack,
                MidiUtilityTransportRequirement::FlushOnDiscontinuity};
    }

    explicit constexpr BasicMonophonicNoteSelector(MonophonicSpec spec = {}) noexcept
        : spec_(sanitize(spec)) {}

    MidiUtilityProcessReport process(const MidiBuffer& input, MidiBuffer& output) noexcept {
        if (utility_detail::blocks_alias(input, output))
            return {0, input.size(), 0, false};
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output))
            return {0, input.size(), 0, false};
        reconcile(0, output, report);
        for (const auto& event : input) {
            if (event.is_note_on()) {
                const int key = utility_detail::key_index(event.channel(), event.note());
                if (held_depth_[key] != std::numeric_limits<std::uint32_t>::max())
                    ++held_depth_[key];
                velocity_[key] = event.velocity();
                if (next_serial_ == std::numeric_limits<Serial>::max())
                    rebase_serials();
                serial_[key] = next_serial_++;
                reconcile(event.sample_offset, output, report);
            } else if (event.is_note_off()) {
                const int key = utility_detail::key_index(event.channel(), event.note());
                if (held_depth_[key] != 0)
                    --held_depth_[key];
                reconcile(event.sample_offset, output, report);
            } else {
                utility_detail::emit(output, event, report);
            }
        }
        utility_detail::copy_sidecars(input, output, report);
        output.sort();
        return report;
    }

    void advance_glide(std::uint32_t samples, double sample_rate) noexcept {
        if (!pitch_.active || !pitch_.glide_active)
            return;
        if (spec_.glide_seconds <= 0.0 || sample_rate <= 0.0 || !std::isfinite(sample_rate)) {
            pitch_.current_note = pitch_.target_note;
            pitch_.glide_active = false;
            return;
        }
        const double coefficient =
            std::exp(-static_cast<double>(samples) / (sample_rate * spec_.glide_seconds));
        pitch_.current_note = static_cast<float>(
            pitch_.target_note + (pitch_.current_note - pitch_.target_note) * coefficient);
        if (std::abs(pitch_.current_note - pitch_.target_note) < 1.0e-5f) {
            pitch_.current_note = pitch_.target_note;
            pitch_.glide_active = false;
        }
    }

    MidiUtilityProcessReport flush(MidiBuffer& output) noexcept {
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output))
            return {0, 0, 0, false};
        if (pitch_.active) {
            if (!utility_detail::emit(output, MidiEvent::note_off(pitch_.channel, pitch_.note),
                                      report)) {
                report.deferred = 1;
                return report;
            }
        }
        held_depth_.fill(0);
        serial_.fill(0);
        pitch_ = {};
        if (pending_spec_) {
            spec_ = *pending_spec_;
            pending_spec_.reset();
        }
        return report;
    }

    MidiUtilityProcessReport reset(MidiBuffer& output) noexcept {
        return flush(output);
    }

    MidiUtilityProcessReport replace_spec(MonophonicSpec spec, MidiBuffer& output) noexcept {
        auto report = flush(output);
        if (report.complete)
            spec_ = sanitize(spec);
        else
            pending_spec_ = sanitize(spec);
        return report;
    }

    const MonophonicPitchState& pitch_state() const noexcept {
        return pitch_;
    }

  private:
    static constexpr MonophonicSpec sanitize(MonophonicSpec spec) noexcept {
        if (!(spec.glide_seconds >= 0.0) || !std::isfinite(spec.glide_seconds))
            spec.glide_seconds = 0.0;
        return spec;
    }

    int selected_key() const noexcept {
        int selected = -1;
        for (int key = 0; key < 16 * 128; ++key) {
            if (held_depth_[key] == 0)
                continue;
            if (selected < 0) {
                selected = key;
                continue;
            }
            const int note = key % 128;
            const int selected_note = selected % 128;
            if ((spec_.priority == MonophonicPriority::Low && note < selected_note) ||
                (spec_.priority == MonophonicPriority::High && note > selected_note) ||
                (spec_.priority == MonophonicPriority::Last && serial_[key] > serial_[selected]))
                selected = key;
        }
        return selected;
    }

    void rebase_serials() noexcept {
        std::array<Serial, 16 * 128> rebased{};
        Serial rank = 1;
        for (;;) {
            int oldest = -1;
            for (int key = 0; key < 16 * 128; ++key) {
                if (held_depth_[key] == 0 || rebased[key] != 0)
                    continue;
                if (oldest < 0 || serial_[key] < serial_[oldest] ||
                    (serial_[key] == serial_[oldest] && key < oldest))
                    oldest = key;
            }
            if (oldest < 0)
                break;
            rebased[oldest] = rank++;
        }
        serial_ = rebased;
        next_serial_ = rank;
    }

    void reconcile(std::int32_t offset, MidiBuffer& output,
                   MidiUtilityProcessReport& report) noexcept {
        const int next = selected_key();
        const int current =
            pitch_.active ? utility_detail::key_index(pitch_.channel, pitch_.note) : -1;
        if (next == current)
            return;
        const bool overlapping = pitch_.active && next >= 0;
        const float previous_pitch = pitch_.current_note;
        if (pitch_.active) {
            if (!utility_detail::emit(
                    output,
                    utility_detail::at(MidiEvent::note_off(pitch_.channel, pitch_.note), offset),
                    report))
                return;
            pitch_.active = false;
        }
        if (next < 0) {
            pitch_ = {};
            return;
        }
        const auto channel = static_cast<std::uint8_t>(next / 128);
        const auto note = static_cast<std::uint8_t>(next % 128);
        if (!utility_detail::emit(
                output,
                utility_detail::at(MidiEvent::note_on(channel, note, velocity_[next]), offset),
                report))
            return;
        pitch_.active = true;
        pitch_.legato = overlapping && spec_.legato;
        pitch_.channel = channel;
        pitch_.note = note;
        pitch_.target_note = static_cast<float>(note);
        pitch_.current_note = pitch_.legato ? previous_pitch : pitch_.target_note;
        pitch_.glide_active = pitch_.legato && spec_.glide_seconds > 0.0;
    }

    MonophonicSpec spec_{};
    std::optional<MonophonicSpec> pending_spec_;
    std::array<std::uint32_t, 16 * 128> held_depth_{};
    std::array<std::uint8_t, 16 * 128> velocity_{};
    std::array<Serial, 16 * 128> serial_{};
    Serial next_serial_ = 1;
    MonophonicPitchState pitch_{};
};

using MonophonicNoteSelector = BasicMonophonicNoteSelector<>;

} // namespace pulp::midi
