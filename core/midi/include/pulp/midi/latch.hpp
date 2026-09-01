#pragma once

#include <pulp/midi/utility_contract.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace pulp::midi {

/// How a latch retains the notes a player releases.
///
/// `Off` is exact passthrough and owns no note. `Hold` retains every note of a
/// phrase until the player lifts every key and starts the next phrase, which is
/// the behaviour an arpeggiator hold pedal provides. `Toggle` makes each press
/// flip one key's retention, so a second press on a latched key releases it.
enum class LatchMode : std::uint8_t {
    Off,
    Hold,
    Toggle,
};

struct LatchSpec {
    LatchMode mode = LatchMode::Off;
    constexpr auto operator<=>(const LatchSpec&) const = default;
};

/// Bounded note-retention kernel over a MIDI event stream.
///
/// The kernel owns every note it retains: a retained note-on is forwarded once
/// and its authored note-off is swallowed, so the matching note-off is emitted
/// later by the latch itself. Retention depth is counted per (channel, note)
/// key, so repeated attacks on one key never leak or double-release. The key
/// space is the whole 16x128 MIDI grid, so retention never overflows and the
/// kernel needs no dynamic allocation on any path.
class Latch {
  public:
    static constexpr std::size_t kKeySpace = 16 * 128;

    static constexpr MidiUtilityContract contract() noexcept {
        return {2, kKeySpace, MidiUtilityOverflowPolicy::FailOpenBalanced,
                MidiUtilitySameSampleOrder::ReleaseBeforeAttack,
                MidiUtilityTransportRequirement::FlushOnDiscontinuity};
    }

    static constexpr bool valid_spec(LatchSpec spec) noexcept {
        return spec.mode == LatchMode::Off || spec.mode == LatchMode::Hold ||
               spec.mode == LatchMode::Toggle;
    }

    explicit constexpr Latch(LatchSpec spec = {}) noexcept
        : spec_(spec), valid_(valid_spec(spec)) {}

    constexpr bool valid() const noexcept {
        return valid_;
    }

    constexpr LatchSpec spec() const noexcept {
        return spec_;
    }

    MidiUtilityProcessReport process(const MidiBuffer& input, MidiBuffer& output) noexcept {
        if (utility_detail::blocks_alias(input, output))
            return {0, input.size(), 0, false};
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output) || !valid_) {
            report.dropped = input.size();
            report.complete = false;
            return report;
        }
        for (const auto& event : input) {
            const bool is_attack = event.is_note_on() && event.velocity() != 0;
            const bool is_release =
                event.is_note_off() || (event.is_note_on() && event.velocity() == 0);
            if (!is_attack && !is_release) {
                utility_detail::emit(output, event, report);
                continue;
            }
            const int key = utility_detail::key_index(event.channel(), event.note());
            if (is_attack)
                handle_attack(key, event, output, report);
            else
                handle_release(key, event, output, report);
        }
        utility_detail::copy_sidecars(input, output, report);
        output.sort();
        return report;
    }

    /// Release every note the latch still owns. Used on transport
    /// discontinuities and when the caller tears the kernel down.
    MidiUtilityProcessReport flush(MidiBuffer& output) noexcept {
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output)) {
            report.complete = false;
            return report;
        }
        for (std::size_t key = 0; key < kKeySpace && owned_total_ != 0; ++key) {
            while (owned_depth_[key] != 0) {
                if (!utility_detail::emit(output, note_off_for(key), report)) {
                    ++report.deferred;
                    report.complete = false;
                    return report;
                }
                --owned_depth_[key];
                --owned_total_;
            }
        }
        physical_depth_.fill(0);
        physical_down_ = 0;
        report.complete = empty();
        if (report.complete && pending_spec_) {
            spec_ = *pending_spec_;
            valid_ = true;
            pending_spec_.reset();
        }
        return report;
    }

    MidiUtilityProcessReport reset(MidiBuffer& output) noexcept {
        return flush(output);
    }

    /// Swap the retention mode. The pending mode is applied only once every
    /// owned note has been released, so a mode change can never orphan a
    /// sounding note.
    MidiUtilityProcessReport replace_spec(LatchSpec spec, MidiBuffer& output) noexcept {
        if (!valid_spec(spec)) {
            utility_detail::clear_output(output);
            return {0, 0, 0, false};
        }
        auto report = flush(output);
        if (report.complete) {
            spec_ = spec;
            valid_ = true;
            pending_spec_.reset();
        } else {
            pending_spec_ = spec;
        }
        return report;
    }

    bool empty() const noexcept {
        return owned_total_ == 0;
    }

    /// Number of note-offs the latch still owes for `channel`/`note`.
    std::uint32_t owned_depth(std::uint8_t channel, std::uint8_t note) const noexcept {
        return owned_depth_[static_cast<std::size_t>(utility_detail::key_index(channel, note))];
    }

  private:
    void handle_attack(int key, const MidiEvent& event, MidiBuffer& output,
                       MidiUtilityProcessReport& report) noexcept {
        if (spec_.mode == LatchMode::Off) {
            if (utility_detail::emit(output, event, report))
                acquire(key);
            increment(physical_depth_[static_cast<std::size_t>(key)]);
            increment(physical_down_);
            return;
        }
        if (spec_.mode == LatchMode::Hold && physical_down_ == 0)
            release_all_owned(event.sample_offset, output, report);
        if (spec_.mode == LatchMode::Toggle &&
            owned_depth_[static_cast<std::size_t>(key)] != 0) {
            // A second press on a latched key releases it and consumes the
            // attack, so the key stops sounding instead of retriggering.
            release_key(key, event.sample_offset, output, report);
            increment(physical_depth_[static_cast<std::size_t>(key)]);
            increment(physical_down_);
            return;
        }
        if (utility_detail::emit(output, event, report))
            acquire(key);
        increment(physical_depth_[static_cast<std::size_t>(key)]);
        increment(physical_down_);
    }

    void handle_release(int key, const MidiEvent& event, MidiBuffer& output,
                        MidiUtilityProcessReport& report) noexcept {
        auto& physical = physical_depth_[static_cast<std::size_t>(key)];
        if (physical != 0) {
            --physical;
            if (physical_down_ != 0)
                --physical_down_;
        }
        if (spec_.mode == LatchMode::Off) {
            if (utility_detail::emit(output, event, report))
                surrender(key);
            return;
        }
        // Retained: the latch, not the player, decides when this note ends.
        ++report.deferred;
    }

    void release_key(int key, std::int32_t sample_offset, MidiBuffer& output,
                     MidiUtilityProcessReport& report) noexcept {
        while (owned_depth_[static_cast<std::size_t>(key)] != 0) {
            auto off =
                utility_detail::at(note_off_for(static_cast<std::size_t>(key)), sample_offset);
            if (!utility_detail::emit(output, off, report)) {
                ++report.deferred;
                report.complete = false;
                return;
            }
            surrender(key);
        }
    }

    /// Take ownership of one more sounding note on `key`. The per-key depth and
    /// the total move together so `empty()` and the scan skip can never
    /// disagree with the table they summarize.
    void acquire(int key) noexcept {
        auto& depth = owned_depth_[static_cast<std::size_t>(key)];
        if (depth == std::numeric_limits<std::uint32_t>::max())
            return;
        ++depth;
        increment(owned_total_);
    }

    void surrender(int key) noexcept {
        auto& depth = owned_depth_[static_cast<std::size_t>(key)];
        if (depth == 0)
            return;
        --depth;
        if (owned_total_ != 0)
            --owned_total_;
    }

    void release_all_owned(std::int32_t sample_offset, MidiBuffer& output,
                           MidiUtilityProcessReport& report) noexcept {
        // The retention table spans the whole key space so retention can never
        // overflow, which makes the scan the price of that guarantee. Counting
        // owned notes keeps the common case — a phrase start with nothing
        // retained — off the audio thread's critical path entirely.
        if (owned_total_ == 0)
            return;
        for (std::size_t key = 0; key < kKeySpace; ++key)
            release_key(static_cast<int>(key), sample_offset, output, report);
    }

    static MidiEvent note_off_for(std::size_t key) noexcept {
        return MidiEvent::note_off(static_cast<std::uint8_t>(key / 128),
                                   static_cast<std::uint8_t>(key % 128));
    }

    template <typename Counter> static void increment(Counter& value) noexcept {
        if (value != std::numeric_limits<Counter>::max())
            ++value;
    }

    LatchSpec spec_{};
    bool valid_ = true;
    std::optional<LatchSpec> pending_spec_{};
    std::array<std::uint32_t, kKeySpace> owned_depth_{};
    std::array<std::uint32_t, kKeySpace> physical_depth_{};
    std::uint32_t physical_down_ = 0;
    std::uint32_t owned_total_ = 0;
};

} // namespace pulp::midi
