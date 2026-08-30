#pragma once

#include <pulp/midi/utility_contract.hpp>
#include <pulp/music/chord.hpp>
#include <pulp/music/voicing.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace pulp::midi {

/// How a captured chord is addressed when a single note triggers it.
enum class ChordMemoryMode : std::uint8_t {
    /// One captured chord answers every trigger, transposed to the trigger.
    Single,
    /// Each trigger pitch class owns its own captured chord.
    PerKey,
    /// The captured shape is stored as scale degrees, so a trigger anywhere in
    /// the scale produces the diatonic chord rather than a parallel transposition.
    ScaleDegree,
};

struct ChordMemorySpec {
    ChordMemoryMode mode = ChordMemoryMode::Single;
    /// Pick the voicing closest to the previous chord instead of the literal
    /// transposition. Off by default because parallel transposition is the
    /// behaviour a player expects until they ask for voice leading.
    bool revoice = false;
    music::MidiRange range{};
    music::PitchClass scale_root = music::PitchClass::c;
    music::NamedScale scale = music::NamedScale::major;
    constexpr auto operator<=>(const ChordMemorySpec&) const = default;
};

/// Bounded chord memory: capture a chord once, then play it from single notes.
///
/// Memory holds interval shapes, never absolute pitches, so a captured chord is
/// meaningful at every trigger. With nothing captured the kernel is exact
/// passthrough, which is its declared identity.
template <std::size_t MaximumActiveTriggers = 16> class ChordMemory {
  public:
    static constexpr std::size_t kMaxChordNotes = music::ChordFormula::kMaxNotes;

    static constexpr MidiUtilityContract contract() noexcept {
        return {kMaxChordNotes, MaximumActiveTriggers, MidiUtilityOverflowPolicy::DropUnstarted,
                MidiUtilitySameSampleOrder::ReleaseBeforeAttack,
                MidiUtilityTransportRequirement::FlushOnDiscontinuity};
    }

    static constexpr bool valid_spec(ChordMemorySpec spec) noexcept {
        return spec.range.valid() && music::is_valid(spec.scale_root);
    }

    explicit ChordMemory(ChordMemorySpec spec = {}) noexcept
        : spec_(spec), valid_(valid_spec(spec)) {}

    bool valid() const noexcept {
        return valid_;
    }

    ChordMemorySpec spec() const noexcept {
        return spec_;
    }

    /// True while nothing is captured, which is the kernel's declared bypass
    /// identity: every event passes through byte for byte.
    bool memory_empty() const noexcept {
        if (spec_.mode == ChordMemoryMode::PerKey) {
            for (const auto& slot : per_key_)
                if (slot.has_value())
                    return false;
            return true;
        }
        return !single_.has_value();
    }

    /// Capture `notes` as an interval shape relative to its own lowest note.
    /// Control-thread work: it validates and stores, and never emits.
    bool learn(std::span<const std::uint8_t> notes) noexcept {
        const auto formula = formula_from(notes);
        if (!formula)
            return false;
        single_ = formula;
        return true;
    }

    /// Capture `notes` as the shape `trigger`'s pitch class plays. Only
    /// meaningful in `PerKey` mode.
    bool learn_for(std::uint8_t trigger, std::span<const std::uint8_t> notes) noexcept {
        const auto formula = formula_from(notes);
        if (!formula)
            return false;
        per_key_[static_cast<std::size_t>(trigger % 12)] = formula;
        return true;
    }

    void clear_memory() noexcept {
        single_.reset();
        per_key_.fill(std::nullopt);
        last_voicing_size_ = 0;
    }

    /// The chord `trigger` produces, written into `out`. Returns the note count,
    /// or 0 when nothing is captured for that trigger. Pure: it reads memory and
    /// the spec but no per-block state, so a test or a UI can predict the chord
    /// without pushing events through the kernel.
    std::size_t chord_for(std::uint8_t trigger,
                          std::array<std::uint8_t, kMaxChordNotes>& out) const noexcept {
        const auto* formula = formula_for(trigger);
        if (formula == nullptr)
            return 0;
        return spec_.mode == ChordMemoryMode::ScaleDegree
                   ? scale_chord(*formula, trigger, out)
                   : parallel_chord(*formula, trigger, out);
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
            if (memory_empty() || (!is_attack && !is_release)) {
                utility_detail::emit(output, event, report);
                continue;
            }
            if (is_attack)
                open_chord(event, output, report);
            else
                close_chord(event, output, report);
        }
        utility_detail::copy_sidecars(input, output, report);
        output.sort();
        return report;
    }

    /// Release every note the kernel owns.
    MidiUtilityProcessReport flush(MidiBuffer& output) noexcept {
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output)) {
            report.complete = false;
            return report;
        }
        for (auto& trigger : triggers_) {
            if (!trigger.active)
                continue;
            if (!release_trigger(trigger, 0, output, report)) {
                ++report.deferred;
                report.complete = false;
                return report;
            }
        }
        last_voicing_size_ = 0;
        report.complete = empty();
        return report;
    }

    MidiUtilityProcessReport reset(MidiBuffer& output) noexcept {
        return flush(output);
    }

    /// Swap the spec. Applied only once every owned note has been released, so
    /// a mode change can never orphan a sounding chord.
    MidiUtilityProcessReport replace_spec(ChordMemorySpec spec, MidiBuffer& output) noexcept {
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

    bool empty() const noexcept {
        for (const auto& trigger : triggers_)
            if (trigger.active)
                return false;
        return true;
    }

  private:
    struct Trigger {
        std::array<std::uint8_t, kMaxChordNotes> notes{};
        std::uint8_t count = 0;
        std::uint8_t channel = 0;
        std::uint8_t note = 0;
        bool active = false;
    };

    static std::optional<music::ChordFormula>
    formula_from(std::span<const std::uint8_t> notes) noexcept {
        if (notes.empty() || notes.size() > kMaxChordNotes)
            return std::nullopt;
        std::array<int, kMaxChordNotes> sorted{};
        for (std::size_t index = 0; index < notes.size(); ++index)
            sorted[index] = static_cast<int>(notes[index]);
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(notes.size()));
        const int lowest = sorted[0];
        std::array<int, kMaxChordNotes> intervals{};
        for (std::size_t index = 0; index < notes.size(); ++index)
            intervals[index] = sorted[index] - lowest;
        return music::ChordFormula::from_intervals(
            std::span<const int>(intervals.data(), notes.size()));
    }

    const music::ChordFormula* formula_for(std::uint8_t trigger) const noexcept {
        if (spec_.mode == ChordMemoryMode::PerKey) {
            const auto& slot = per_key_[static_cast<std::size_t>(trigger % 12)];
            return slot ? &*slot : nullptr;
        }
        return single_ ? &*single_ : nullptr;
    }

    static std::size_t parallel_chord(const music::ChordFormula& formula, std::uint8_t trigger,
                                      std::array<std::uint8_t, kMaxChordNotes>& out) noexcept {
        std::size_t count = 0;
        for (std::size_t tone = 0; tone < formula.size(); ++tone) {
            const auto interval = formula.interval(tone);
            if (!interval)
                continue;
            const int pitch = static_cast<int>(trigger) + *interval;
            // A chord that would run off the top is truncated rather than
            // folded, because folding invents a voicing nobody captured.
            if (pitch > 127)
                break;
            out[count++] = static_cast<std::uint8_t>(pitch);
        }
        return count;
    }

    /// Walk the captured semitone intervals as counts of scale steps, so the
    /// shape stays inside the spec's scale instead of transposing in parallel.
    std::size_t scale_chord(const music::ChordFormula& formula, std::uint8_t trigger,
                            std::array<std::uint8_t, kMaxChordNotes>& out) const noexcept {
        const auto scale = music::Scale::named(spec_.scale_root, spec_.scale);
        if (!scale)
            return parallel_chord(formula, trigger, out);
        const int root = utility_detail::nearest_scale_note(static_cast<int>(trigger), *scale);
        std::size_t count = 0;
        for (std::size_t tone = 0; tone < formula.size(); ++tone) {
            const auto interval = formula.interval(tone);
            if (!interval)
                continue;
            // The captured semitone interval names the degree by counting the
            // scale members it spans, which is what makes the replay diatonic.
            const int degree = scale_steps_in(*scale, root, *interval);
            const auto pitch = scale_note_above(*scale, root, degree);
            if (!pitch)
                break;
            out[count++] = *pitch;
        }
        return count;
    }

    static int scale_steps_in(const music::Scale& scale, int root, int semitones) noexcept {
        int steps = 0;
        for (int offset = 1; offset <= semitones; ++offset) {
            const int candidate = root + offset;
            if (candidate > 127)
                break;
            if (scale.contains(static_cast<music::PitchClass>(candidate % 12)))
                ++steps;
        }
        return steps;
    }

    static std::optional<std::uint8_t> scale_note_above(const music::Scale& scale, int root,
                                                        int steps) noexcept {
        if (steps == 0)
            return static_cast<std::uint8_t>(std::clamp(root, 0, 127));
        int remaining = steps;
        for (int candidate = root + 1; candidate <= 127; ++candidate) {
            if (!scale.contains(static_cast<music::PitchClass>(candidate % 12)))
                continue;
            if (--remaining == 0)
                return static_cast<std::uint8_t>(candidate);
        }
        return std::nullopt;
    }

    /// Replace the literal chord with the voicing of the same pitch classes
    /// that moves least from the previous chord.
    std::size_t revoiced(const music::ChordFormula& formula, std::uint8_t trigger,
                         std::array<std::uint8_t, kMaxChordNotes>& notes,
                         std::size_t count) noexcept {
        if (!spec_.revoice || last_voicing_size_ != count || count == 0)
            return count;
        std::array<int, kMaxChordNotes> previous{};
        for (std::size_t index = 0; index < count; ++index)
            previous[index] = static_cast<int>(last_voicing_[index]);
        for (std::size_t index = 1; index < count; ++index)
            if (previous[index - 1] >= previous[index])
                return count;
        const auto voicing = music::minimum_motion_voice_leading(
            std::span<const int>(previous.data(), count),
            static_cast<music::PitchClass>(trigger % 12), formula, spec_.range);
        if (!voicing || voicing->size() != count)
            return count;
        const auto pitches = voicing->pitches();
        for (std::size_t index = 0; index < count; ++index)
            notes[index] = pitches[index];
        return count;
    }

    void open_chord(const MidiEvent& event, MidiBuffer& output,
                    MidiUtilityProcessReport& report) noexcept {
        std::array<std::uint8_t, kMaxChordNotes> notes{};
        auto count = chord_for(event.note(), notes);
        if (count == 0) {
            utility_detail::emit(output, event, report);
            return;
        }
        if (const auto* formula = formula_for(event.note()); formula != nullptr)
            count = revoiced(*formula, event.note(), notes, count);
        Trigger* slot = free_trigger();
        if (slot == nullptr) {
            // DropUnstarted: with no slot to record ownership the chord would
            // have no way to be released, so it must not sound at all.
            ++report.dropped;
            report.complete = false;
            return;
        }
        // A retrigger of the same key releases the chord it already owns, so a
        // key can never own two chords at once.
        close_key(event.channel(), event.note(), event.sample_offset, output, report);
        std::uint8_t emitted = 0;
        for (std::size_t index = 0; index < count; ++index) {
            auto on = MidiEvent::note_on(event.channel(), notes[index], event.velocity());
            on = utility_detail::at(on, event.sample_offset);
            if (!utility_detail::emit(output, on, report))
                break;
            slot->notes[emitted++] = notes[index];
        }
        if (emitted == 0) {
            report.complete = false;
            return;
        }
        slot->count = emitted;
        slot->channel = event.channel();
        slot->note = event.note();
        slot->active = true;
        for (std::size_t index = 0; index < emitted; ++index)
            last_voicing_[index] = slot->notes[index];
        last_voicing_size_ = emitted;
    }

    void close_chord(const MidiEvent& event, MidiBuffer& output,
                     MidiUtilityProcessReport& report) noexcept {
        if (!close_key(event.channel(), event.note(), event.sample_offset, output, report))
            utility_detail::emit(output, event, report);
    }

    bool close_key(std::uint8_t channel, std::uint8_t note, std::int32_t sample_offset,
                   MidiBuffer& output, MidiUtilityProcessReport& report) noexcept {
        bool closed = false;
        for (auto& trigger : triggers_) {
            if (!trigger.active || trigger.channel != channel || trigger.note != note)
                continue;
            release_trigger(trigger, sample_offset, output, report);
            closed = true;
        }
        return closed;
    }

    bool release_trigger(Trigger& trigger, std::int32_t sample_offset, MidiBuffer& output,
                         MidiUtilityProcessReport& report) noexcept {
        while (trigger.count != 0) {
            const auto note = trigger.notes[trigger.count - 1];
            auto off = utility_detail::at(MidiEvent::note_off(trigger.channel, note), sample_offset);
            if (!utility_detail::emit(output, off, report))
                return false;
            --trigger.count;
        }
        trigger = {};
        return true;
    }

    Trigger* free_trigger() noexcept {
        for (auto& trigger : triggers_)
            if (!trigger.active)
                return &trigger;
        return nullptr;
    }

    ChordMemorySpec spec_{};
    bool valid_ = true;
    std::optional<music::ChordFormula> single_{};
    std::array<std::optional<music::ChordFormula>, 12> per_key_{};
    std::array<Trigger, MaximumActiveTriggers> triggers_{};
    std::array<std::uint8_t, kMaxChordNotes> last_voicing_{};
    std::size_t last_voicing_size_ = 0;
};

} // namespace pulp::midi
