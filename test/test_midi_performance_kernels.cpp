#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/chord_memory.hpp>
#include <pulp/midi/humanize.hpp>
#include <pulp/midi/latch.hpp>
#include <pulp/midi/note_delay.hpp>
#include <pulp/midi/note_repeat.hpp>
#include <pulp/midi/strum.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <tuple>
#include <vector>

namespace {

using namespace pulp;

constexpr std::uint64_t kSampleRate = 48'000;
// Shipped timebase constants: a quarter note is 705'600 ticks, so at 120 bpm and
// 48 kHz one quarter is 24'000 samples and a sixteenth is exactly 6'000. Every
// grid oracle below is derived from these numbers rather than from the kernels.
constexpr std::int64_t kSixteenthTicks = timebase::kTicksPerQuarter / 4;
constexpr std::int64_t kSixteenthSamples = 6'000;

midi::MidiBuffer prepared_buffer(std::size_t capacity = 512) {
    midi::MidiBuffer buffer;
    buffer.reserve(capacity, 4, 16);
    buffer.set_realtime_capacity_limit(true);
    return buffer;
}

struct AbsoluteMidiEvent {
    std::int64_t sample = 0;
    midi::MidiEvent event{};

    bool attack() const {
        return event.is_note_on() && event.velocity() != 0;
    }
    bool release() const {
        return event.is_note_off() || (event.is_note_on() && event.velocity() == 0);
    }
    auto identity() const {
        return std::tuple{sample,          attack(),     release(),
                          event.channel(), event.note(), event.velocity()};
    }
};

AbsoluteMidiEvent on(std::int64_t sample, std::uint8_t note, std::uint8_t velocity = 100,
                     std::uint8_t channel = 0) {
    return {sample, midi::MidiEvent::note_on(channel, note, velocity)};
}

AbsoluteMidiEvent off(std::int64_t sample, std::uint8_t note, std::uint8_t channel = 0) {
    return {sample, midi::MidiEvent::note_off(channel, note)};
}

/// Tracks note-on/note-off depth per key. `balanced()` is false if any note-off
/// ever arrived without a matching note-on, or if any note is left sounding.
struct EventLedger {
    std::array<int, 16 * 128> depth{};
    bool orphaned = false;

    void feed(const AbsoluteMidiEvent& absolute) {
        auto& value = depth[absolute.event.channel() * 128 + absolute.event.note()];
        if (absolute.attack()) {
            ++value;
        } else if (absolute.release()) {
            if (value == 0)
                orphaned = true;
            else
                --value;
        }
    }

    bool balanced() const {
        return !orphaned && std::all_of(depth.begin(), depth.end(), [](int v) { return v == 0; });
    }
    bool no_orphans() const {
        return !orphaned;
    }
};

/// Drives a kernel over `total_samples` split by `partitions`, feeding
/// `input_events` at their absolute positions and collecting the output back at
/// absolute positions. `invoke` adapts each kernel's own process signature.
using ProcessFn = std::function<void(const midi::MidiBuffer&, midi::MidiBuffer&, std::int64_t,
                                     std::int32_t)>;

std::vector<AbsoluteMidiEvent> render(const ProcessFn& invoke, std::int64_t total_samples,
                                      std::span<const std::int32_t> partitions,
                                      std::span<const AbsoluteMidiEvent> input_events) {
    std::vector<AbsoluteMidiEvent> result;
    std::size_t partition_index = 0;
    std::size_t input_index = 0;
    std::int64_t block_start = 0;
    while (block_start < total_samples) {
        const auto requested = partitions[partition_index++ % partitions.size()];
        const auto block_size = static_cast<std::int32_t>(
            std::min<std::int64_t>(requested, total_samples - block_start));
        auto input = prepared_buffer();
        while (input_index < input_events.size() &&
               input_events[input_index].sample < block_start + block_size) {
            REQUIRE(input_events[input_index].sample >= block_start);
            auto event = input_events[input_index].event;
            event.sample_offset =
                static_cast<std::int32_t>(input_events[input_index].sample - block_start);
            REQUIRE(input.add(event));
            ++input_index;
        }
        auto output = prepared_buffer();
        invoke(input, output, block_start, block_size);
        for (const auto& event : output)
            result.push_back({block_start + event.sample_offset, event});
        block_start += block_size;
    }
    return result;
}

std::vector<std::int64_t> attack_samples(const std::vector<AbsoluteMidiEvent>& events,
                                         std::uint8_t note) {
    std::vector<std::int64_t> result;
    for (const auto& event : events)
        if (event.attack() && event.event.note() == note)
            result.push_back(event.sample);
    return result;
}

std::vector<std::uint8_t> attack_velocities(const std::vector<AbsoluteMidiEvent>& events) {
    std::vector<std::uint8_t> result;
    for (const auto& event : events)
        if (event.attack())
            result.push_back(event.event.velocity());
    return result;
}

std::vector<std::uint8_t> attack_notes(const std::vector<AbsoluteMidiEvent>& events) {
    std::vector<std::uint8_t> result;
    for (const auto& event : events)
        if (event.attack())
            result.push_back(event.event.note());
    return result;
}

std::size_t count_attacks(const std::vector<AbsoluteMidiEvent>& events) {
    return static_cast<std::size_t>(
        std::count_if(events.begin(), events.end(), [](const auto& e) { return e.attack(); }));
}

bool identical(const std::vector<AbsoluteMidiEvent>& lhs,
               const std::vector<AbsoluteMidiEvent>& rhs) {
    if (lhs.size() != rhs.size())
        return false;
    for (std::size_t index = 0; index < lhs.size(); ++index)
        if (lhs[index].identity() != rhs[index].identity())
            return false;
    return true;
}

midi::note_schedule::Block constant_block(std::int64_t start, std::int32_t count) {
    midi::note_schedule::Block block;
    block.sample_start = {start};
    // Tick and sample origins share an anchor, so a constant tempo projects a
    // sixteenth onto exactly kSixteenthSamples.
    block.tick_start = {start * timebase::kTicksPerQuarter / (kSixteenthSamples * 4)};
    block.sample_count = count;
    block.sample_rate = timebase::RationalRate{kSampleRate, 1};
    block.tempo_bpm = 120.0;
    return block;
}

constexpr std::array<std::int32_t, 1> kWholeBlock{1 << 20};
constexpr std::array<std::int32_t, 1> kSmallBlocks{64};
constexpr std::array<std::int32_t, 3> kRaggedBlocks{37, 512, 111};

} // namespace

// ---------------------------------------------------------------------------
// Latch
// ---------------------------------------------------------------------------

TEST_CASE("latch off is exact passthrough", "[midi][latch][parity]") {
    // Negative control: with the kernel bypassed the output must be the input,
    // event for event. If this ever diverges the kernel is doing something on a
    // path that is supposed to be inert.
    const std::array input{on(0, 60), on(10, 64), off(500, 60), off(510, 64)};
    midi::Latch latch{{midi::LatchMode::Off}};
    auto out = render([&](const auto& in, auto& o, std::int64_t, std::int32_t) { latch.process(in, o); },
                      1'000, kWholeBlock, input);
    REQUIRE(out.size() == input.size());
    for (std::size_t index = 0; index < input.size(); ++index)
        REQUIRE(out[index].identity() == input[index].identity());
}

TEST_CASE("latch hold retains a phrase and releases it on the next", "[midi][latch]") {
    const std::array input{on(0, 60), off(100, 60), on(1'000, 67), off(1'100, 67)};
    midi::Latch latch{{midi::LatchMode::Hold}};
    auto out = render([&](const auto& in, auto& o, std::int64_t, std::int32_t) { latch.process(in, o); },
                      2'000, kWholeBlock, input);

    // The first note's authored release is swallowed, so note 60 stays on until
    // the next phrase begins at 1'000.
    const auto sixty_offs = std::count_if(out.begin(), out.end(), [](const auto& e) {
        return e.release() && e.event.note() == 60;
    });
    REQUIRE(sixty_offs == 1);
    const auto sixty_off_at = std::find_if(out.begin(), out.end(), [](const auto& e) {
        return e.release() && e.event.note() == 60;
    });
    REQUIRE(sixty_off_at->sample == 1'000);

    EventLedger ledger;
    for (const auto& event : out)
        ledger.feed(event);
    REQUIRE(ledger.no_orphans());

    auto flush_out = prepared_buffer();
    latch.flush(flush_out);
    for (const auto& event : flush_out)
        ledger.feed({0, event});
    REQUIRE(ledger.balanced());
    REQUIRE(latch.empty());
}

TEST_CASE("latch toggle flips one key at a time", "[midi][latch]") {
    const std::array input{on(0, 60), off(50, 60), on(100, 60), off(150, 60)};
    midi::Latch latch{{midi::LatchMode::Toggle}};
    auto out = render([&](const auto& in, auto& o, std::int64_t, std::int32_t) { latch.process(in, o); },
                      1'000, kWholeBlock, input);
    // Press latches, second press releases: exactly one on and one off.
    REQUIRE(count_attacks(out) == 1);
    REQUIRE(out.front().sample == 0);
    EventLedger ledger;
    for (const auto& event : out)
        ledger.feed(event);
    REQUIRE(ledger.balanced());
    REQUIRE(latch.empty());
}

TEST_CASE("latch counts retention depth so repeats stay balanced", "[midi][latch][rt-safety]") {
    // Two attacks on one key without an intervening release must produce two
    // note-offs, not one. A kernel tracking a bare boolean fails here.
    const std::array input{on(0, 60), on(10, 60), off(20, 60)};
    midi::Latch latch{{midi::LatchMode::Hold}};
    auto out = render([&](const auto& in, auto& o, std::int64_t, std::int32_t) { latch.process(in, o); },
                      500, kWholeBlock, input);
    REQUIRE(latch.owned_depth(0, 60) == 2);
    auto flush_out = prepared_buffer();
    latch.flush(flush_out);
    REQUIRE(flush_out.size() == 2);
    EventLedger ledger;
    for (const auto& event : out)
        ledger.feed(event);
    for (const auto& event : flush_out)
        ledger.feed({0, event});
    REQUIRE(ledger.balanced());
}

TEST_CASE("latch allocates nothing while processing", "[midi][latch][rt-safety]") {
    midi::Latch latch{{midi::LatchMode::Hold}};
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    REQUIRE(input.add(midi::MidiEvent::note_on(0, 60, 100)));
    {
        pulp::test::RtAllocationProbe probe;
        latch.process(input, output);
        REQUIRE_FALSE(probe.saw_allocation());
    }
}

// ---------------------------------------------------------------------------
// Humanize
// ---------------------------------------------------------------------------

TEST_CASE("humanize with zero amounts is exact identity", "[midi][humanize][parity]") {
    // Negative control for the bypass claim.
    const std::array input{on(0, 60), on(37, 64, 90), off(500, 60), off(513, 64)};
    REQUIRE(midi::Humanize<>::is_identity({0, 0, 99}));
    midi::Humanize<> humanize{{0, 0, 99}};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            humanize.process(in, o, {start}, count);
        },
        1'000, kWholeBlock, input);
    REQUIRE(out.size() == input.size());
    for (std::size_t index = 0; index < input.size(); ++index)
        REQUIRE(out[index].identity() == input[index].identity());
}

TEST_CASE("humanize keeps jitter inside the declared bounds", "[midi][humanize]") {
    constexpr std::int64_t kTiming = 128;
    constexpr std::uint8_t kVelocity = 12;
    std::vector<AbsoluteMidiEvent> input;
    for (std::uint8_t note = 48; note < 72; ++note) {
        input.push_back(on(static_cast<std::int64_t>(note - 48) * 400, note, 100));
        input.push_back(off(static_cast<std::int64_t>(note - 48) * 400 + 300, note));
    }
    midi::Humanize<> humanize{{kTiming, kVelocity, 7'331}};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            humanize.process(in, o, {start}, count);
        },
        12'000, kRaggedBlocks, input);

    std::size_t attacks = 0;
    bool saw_timing_move = false;
    bool saw_velocity_move = false;
    for (const auto& event : out) {
        if (!event.attack())
            continue;
        ++attacks;
        const auto authored = static_cast<std::int64_t>(event.event.note() - 48) * 400;
        const auto shift = event.sample - authored;
        REQUIRE(shift >= 0);
        REQUIRE(shift <= kTiming);
        saw_timing_move = saw_timing_move || shift != 0;
        const auto velocity = static_cast<int>(event.event.velocity());
        REQUIRE(velocity >= 100 - static_cast<int>(kVelocity));
        REQUIRE(velocity <= 100 + static_cast<int>(kVelocity));
        saw_velocity_move = saw_velocity_move || velocity != 100;
    }
    REQUIRE(attacks == 24);
    // Sensitivity control: a kernel that shifted nothing would also satisfy the
    // bounds above, so prove the measurement can see movement at all.
    REQUIRE(saw_timing_move);
    REQUIRE(saw_velocity_move);
}

TEST_CASE("humanize is invariant under block partition", "[midi][humanize][parity]") {
    // The determinism oracle: the same seed and the same authored events must
    // produce one event stream regardless of how the host slices its blocks.
    std::vector<AbsoluteMidiEvent> input;
    for (std::uint8_t note = 60; note < 68; ++note) {
        input.push_back(on(static_cast<std::int64_t>(note - 60) * 700, note, 88));
        input.push_back(off(static_cast<std::int64_t>(note - 60) * 700 + 600, note));
    }
    auto run = [&](std::span<const std::int32_t> partitions) {
        midi::Humanize<> humanize{{96, 9, 4'242}};
        return render(
            [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
                humanize.process(in, o, {start}, count);
            },
            8'000, partitions, input);
    };
    const auto whole = run(kWholeBlock);
    const auto small = run(kSmallBlocks);
    const auto ragged = run(kRaggedBlocks);
    REQUIRE(identical(whole, small));
    REQUIRE(identical(whole, ragged));
    REQUIRE(count_attacks(whole) == 8);
}

TEST_CASE("humanize never lets a jittered attack outrun its own release", "[midi][humanize]") {
    // A note shorter than the jitter window would otherwise emit its note-off
    // before its note-on.
    const std::array input{on(0, 60, 100), off(4, 60)};
    midi::Humanize<> humanize{{4'096, 0, 5}};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            humanize.process(in, o, {start}, count);
        },
        8'192, kWholeBlock, input);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].attack());
    REQUIRE(out[1].release());
    REQUIRE(out[0].sample <= out[1].sample);
    EventLedger ledger;
    for (const auto& event : out)
        ledger.feed(event);
    REQUIRE(ledger.balanced());
}

TEST_CASE("humanize allocates nothing while processing", "[midi][humanize][rt-safety]") {
    midi::Humanize<> humanize{{64, 8, 11}};
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    REQUIRE(input.add(midi::MidiEvent::note_on(0, 60, 100)));
    {
        pulp::test::RtAllocationProbe probe;
        humanize.process(input, output, {0}, 512);
        REQUIRE_FALSE(probe.saw_allocation());
    }
}

// ---------------------------------------------------------------------------
// Note repeat
// ---------------------------------------------------------------------------

TEST_CASE("note repeat with count 1 is exact identity", "[midi][note-repeat][parity]") {
    // Negative control for the bypass claim.
    const std::array input{on(0, 60), off(500, 60)};
    REQUIRE(midi::NoteRepeat<>::is_identity({{kSixteenthTicks}, 1, 50, 100, 100, 0}));
    midi::NoteRepeat<> repeat{{{kSixteenthTicks}, 1, 50, 100, 100, 0}};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            repeat.process(in, o, constant_block(start, count));
        },
        48'000, kWholeBlock, input);
    REQUIRE(out.size() == input.size());
    for (std::size_t index = 0; index < input.size(); ++index)
        REQUIRE(out[index].identity() == input[index].identity());
}

TEST_CASE("note repeat spaces hits on the authored division", "[midi][note-repeat]") {
    const std::array input{on(0, 60, 120), off(30'000, 60)};
    midi::NoteRepeat<> repeat{{{kSixteenthTicks}, 4, 50, 100, 100, 0}};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            repeat.process(in, o, constant_block(start, count));
        },
        48'000, kRaggedBlocks, input);

    // Oracle: the hit grid comes from the shipped tick constants, computed here
    // without calling the kernel. A sixteenth at 120 bpm / 48 kHz is 6'000
    // samples, so four hits land at 0, 6'000, 12'000 and 18'000.
    const std::vector<std::int64_t> expected{0, kSixteenthSamples, 2 * kSixteenthSamples,
                                             3 * kSixteenthSamples};
    REQUIRE(attack_samples(out, 60) == expected);

    EventLedger ledger;
    for (const auto& event : out)
        ledger.feed(event);
    REQUIRE(ledger.balanced());
}

TEST_CASE("note repeat decays velocity by the shipped law", "[midi][note-repeat]") {
    const std::array input{on(0, 60, 100), off(30'000, 60)};
    constexpr std::uint8_t kDecay = 80;
    midi::NoteRepeat<> repeat{{{kSixteenthTicks}, 4, 50, 100, kDecay, 0}};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            repeat.process(in, o, constant_block(start, count));
        },
        48'000, kWholeBlock, input);

    // Oracle: repeated integer percent decay, derived here rather than read back
    // from the kernel's own helper.
    std::vector<std::uint8_t> expected;
    int value = 100;
    for (int index = 0; index < 4; ++index) {
        expected.push_back(static_cast<std::uint8_t>(std::clamp(value, 1, 127)));
        value = value * kDecay / 100;
    }
    REQUIRE(attack_velocities(out) == expected);
}

TEST_CASE("note repeat at zero probability emits no hits", "[midi][note-repeat][parity]") {
    // Negative control: the probability path must be able to silence the series
    // completely, otherwise a passing probability test proves nothing.
    // Held past the whole series, so the count below measures the probability
    // draw and not the release-cancels-unstarted-hits path.
    const std::array input{on(0, 60, 100), off(47'000, 60)};
    midi::NoteRepeat<> repeat{{{kSixteenthTicks}, 8, 50, 0, 100, 12'345}};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            repeat.process(in, o, constant_block(start, count));
        },
        48'000, kWholeBlock, input);
    REQUIRE(count_attacks(out) == 0);

    // Sensitivity control: the identical setup at full probability must emit,
    // so the zero above is the world and not a broken instrument.
    midi::NoteRepeat<> certain{{{kSixteenthTicks}, 8, 50, 100, 100, 12'345}};
    auto certain_out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            certain.process(in, o, constant_block(start, count));
        },
        48'000, kWholeBlock, input);
    REQUIRE(count_attacks(certain_out) == 8);
}

TEST_CASE("note repeat cancels unstarted hits when the key is released", "[midi][note-repeat]") {
    // Release after the second hit: hits three and four never sound, and the
    // note already sounding still gets its own note-off.
    const std::array input{on(0, 60, 100), off(kSixteenthSamples + 10, 60)};
    midi::NoteRepeat<> repeat{{{kSixteenthTicks}, 4, 50, 100, 100, 0}};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            repeat.process(in, o, constant_block(start, count));
        },
        48'000, kRaggedBlocks, input);
    REQUIRE(attack_samples(out, 60) == std::vector<std::int64_t>{0, kSixteenthSamples});
    EventLedger ledger;
    for (const auto& event : out)
        ledger.feed(event);
    REQUIRE(ledger.balanced());
    REQUIRE(repeat.empty());
}

TEST_CASE("note repeat is invariant under block partition", "[midi][note-repeat][parity]") {
    const std::array input{on(0, 60, 100), off(40'000, 60)};
    auto run = [&](std::span<const std::int32_t> partitions) {
        midi::NoteRepeat<> repeat{{{kSixteenthTicks}, 6, 40, 70, 90, 2'024}};
        return render(
            [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
                repeat.process(in, o, constant_block(start, count));
            },
            48'000, partitions, input);
    };
    REQUIRE(identical(run(kWholeBlock), run(kSmallBlocks)));
    REQUIRE(identical(run(kWholeBlock), run(kRaggedBlocks)));
}

TEST_CASE("note repeat allocates nothing while processing", "[midi][note-repeat][rt-safety]") {
    midi::NoteRepeat<> repeat{{{kSixteenthTicks}, 8, 50, 100, 90, 3}};
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    REQUIRE(input.add(midi::MidiEvent::note_on(0, 60, 100)));
    {
        pulp::test::RtAllocationProbe probe;
        repeat.process(input, output, constant_block(0, 512));
        REQUIRE_FALSE(probe.saw_allocation());
    }
}

// ---------------------------------------------------------------------------
// Note delay
// ---------------------------------------------------------------------------

TEST_CASE("note delay with zero repeats is exact dry passthrough", "[midi][note-delay][parity]") {
    // Negative control for the bypass claim.
    const std::array input{on(0, 60), on(20, 64), off(500, 60), off(520, 64)};
    midi::NoteDelaySpec spec{};
    spec.repeats = 0;
    REQUIRE(midi::NoteDelay<>::is_identity(spec));
    midi::NoteDelay<> delay{spec};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            delay.process(in, o, constant_block(start, count));
        },
        48'000, kWholeBlock, input);
    REQUIRE(out.size() == input.size());
    for (std::size_t index = 0; index < input.size(); ++index)
        REQUIRE(out[index].identity() == input[index].identity());
}

TEST_CASE("note delay echoes on the authored division", "[midi][note-delay]") {
    const std::array input{on(0, 60, 100), off(3'000, 60)};
    midi::NoteDelaySpec spec{};
    spec.sync = midi::NoteDelaySync::Division;
    spec.interval = {kSixteenthTicks};
    spec.repeats = 3;
    spec.velocity_decay_percent = 50;
    midi::NoteDelay<> delay{spec};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            delay.process(in, o, constant_block(start, count));
        },
        48'000, kRaggedBlocks, input);

    // Oracle: the dry note plus three echoes one sixteenth apart, derived from
    // the shipped tick constants.
    const std::vector<std::int64_t> expected{0, kSixteenthSamples, 2 * kSixteenthSamples,
                                             3 * kSixteenthSamples};
    REQUIRE(attack_samples(out, 60) == expected);
    // Velocity decays 100 -> 50 -> 25 -> 12 under repeated integer percent.
    REQUIRE(attack_velocities(out) == std::vector<std::uint8_t>{100, 50, 25, 12});

    EventLedger ledger;
    for (const auto& event : out)
        ledger.feed(event);
    REQUIRE(ledger.balanced());
}

TEST_CASE("note delay echoes on a millisecond clock", "[midi][note-delay]") {
    const std::array input{on(0, 60, 100), off(1'000, 60)};
    midi::NoteDelaySpec spec{};
    spec.sync = midi::NoteDelaySync::Milliseconds;
    spec.milliseconds = 125;
    spec.repeats = 2;
    spec.velocity_decay_percent = 100;
    midi::NoteDelay<> delay{spec};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            delay.process(in, o, constant_block(start, count));
        },
        48'000, kRaggedBlocks, input);
    // Oracle: 125 ms at 48 kHz is exactly 6'000 samples, computed from the rate,
    // not from the kernel.
    const std::vector<std::int64_t> expected{0, 6'000, 12'000};
    REQUIRE(attack_samples(out, 60) == expected);
}

TEST_CASE("note delay transposes each repeat cumulatively", "[midi][note-delay]") {
    const std::array input{on(0, 60, 100), off(2'000, 60)};
    midi::NoteDelaySpec spec{};
    spec.interval = {kSixteenthTicks};
    spec.repeats = 3;
    spec.velocity_decay_percent = 100;
    spec.transpose_semitones = 5;
    midi::NoteDelay<> delay{spec};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            delay.process(in, o, constant_block(start, count));
        },
        48'000, kWholeBlock, input);
    REQUIRE(attack_notes(out) == std::vector<std::uint8_t>{60, 65, 70, 75});
    EventLedger ledger;
    for (const auto& event : out)
        ledger.feed(event);
    REQUIRE(ledger.balanced());
}

TEST_CASE("note delay rolls an armed echo back to the authored length",
          "[midi][note-delay]") {
    // The echo's length is not known until the authored release arrives, so each
    // echo must end up as long as the note that produced it.
    constexpr std::int64_t kHeld = 1'500;
    const std::array input{on(0, 60, 100), off(kHeld, 60)};
    midi::NoteDelaySpec spec{};
    spec.interval = {kSixteenthTicks};
    spec.repeats = 2;
    spec.velocity_decay_percent = 100;
    midi::NoteDelay<> delay{spec};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            delay.process(in, o, constant_block(start, count));
        },
        48'000, kRaggedBlocks, input);

    std::vector<std::int64_t> lengths;
    std::vector<std::int64_t> open_at;
    for (const auto& event : out) {
        if (event.attack())
            open_at.push_back(event.sample);
        else if (event.release() && !open_at.empty()) {
            lengths.push_back(event.sample - open_at.front());
            open_at.erase(open_at.begin());
        }
    }
    REQUIRE(lengths.size() == 3);
    for (const auto length : lengths)
        REQUIRE(length == kHeld);
}

TEST_CASE("note delay is invariant under block partition", "[midi][note-delay][parity]") {
    const std::array input{on(0, 60, 100), off(2'500, 60)};
    auto run = [&](std::span<const std::int32_t> partitions) {
        midi::NoteDelaySpec spec{};
        spec.interval = {kSixteenthTicks};
        spec.repeats = 4;
        spec.velocity_decay_percent = 75;
        spec.transpose_semitones = -2;
        midi::NoteDelay<> delay{spec};
        return render(
            [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
                delay.process(in, o, constant_block(start, count));
            },
            48'000, partitions, input);
    };
    REQUIRE(identical(run(kWholeBlock), run(kSmallBlocks)));
    REQUIRE(identical(run(kWholeBlock), run(kRaggedBlocks)));
}

TEST_CASE("note delay allocates nothing while processing", "[midi][note-delay][rt-safety]") {
    midi::NoteDelaySpec spec{};
    spec.repeats = 4;
    midi::NoteDelay<> delay{spec};
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    REQUIRE(input.add(midi::MidiEvent::note_on(0, 60, 100)));
    {
        pulp::test::RtAllocationProbe probe;
        delay.process(input, output, constant_block(0, 512));
        REQUIRE_FALSE(probe.saw_allocation());
    }
}

// ---------------------------------------------------------------------------
// Strum
// ---------------------------------------------------------------------------

TEST_CASE("strum passes a single note through unchanged", "[midi][strum][parity]") {
    // Negative control: one note is not a cluster, so nothing may be spread. Its
    // only cost is the declared window of latency.
    const std::array input{on(0, 60, 100), off(5'000, 60)};
    midi::StrumSpec spec{};
    spec.window_samples = 0;
    spec.sync = midi::StrumSpacingSync::Milliseconds;
    spec.spacing_milliseconds = 20;
    midi::Strum<> strum{spec};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            strum.process(in, o, constant_block(start, count));
        },
        48'000, kWholeBlock, input);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].identity() == input[0].identity());
    REQUIRE(out[1].identity() == input[1].identity());
}

TEST_CASE("strum spreads a cluster on the spacing grid", "[midi][strum]") {
    // Four simultaneous notes, 20 ms apart at 48 kHz = 960 samples per step.
    const std::array input{on(0, 60, 100), on(0, 64, 100), on(0, 67, 100), on(0, 72, 100),
                           off(20'000, 60), off(20'000, 64), off(20'000, 67), off(20'000, 72)};
    midi::StrumSpec spec{};
    spec.direction = midi::StrumDirection::Up;
    spec.shape = midi::StrumShape::Linear;
    spec.sync = midi::StrumSpacingSync::Milliseconds;
    spec.spacing_milliseconds = 20;
    spec.window_samples = 64;
    midi::Strum<> strum{spec};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            strum.process(in, o, constant_block(start, count));
        },
        48'000, kRaggedBlocks, input);

    // Oracle: base is the window close, then 960 samples per linear step.
    constexpr std::int64_t kStep = 960;
    constexpr std::int64_t kBase = 64;
    REQUIRE(attack_notes(out) == std::vector<std::uint8_t>{60, 64, 67, 72});
    std::vector<std::int64_t> starts;
    for (const auto& event : out)
        if (event.attack())
            starts.push_back(event.sample);
    REQUIRE(starts == std::vector<std::int64_t>{kBase, kBase + kStep, kBase + 2 * kStep,
                                                kBase + 3 * kStep});
    EventLedger ledger;
    for (const auto& event : out)
        ledger.feed(event);
    REQUIRE(ledger.balanced());
}

TEST_CASE("strum orders a cluster per direction", "[midi][strum]") {
    auto run = [](midi::StrumDirection direction) {
        const std::array input{on(0, 64, 100), on(0, 60, 100), on(0, 67, 100),
                               off(20'000, 60), off(20'000, 64), off(20'000, 67)};
        midi::StrumSpec spec{};
        spec.direction = direction;
        spec.sync = midi::StrumSpacingSync::Milliseconds;
        spec.spacing_milliseconds = 10;
        spec.window_samples = 64;
        midi::Strum<> strum{spec};
        return attack_notes(render(
            [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
                strum.process(in, o, constant_block(start, count));
            },
            48'000, kWholeBlock, input));
    };
    REQUIRE(run(midi::StrumDirection::Up) == std::vector<std::uint8_t>{60, 64, 67});
    REQUIRE(run(midi::StrumDirection::Down) == std::vector<std::uint8_t>{67, 64, 60});
    // As played keeps arrival order, which differs from both sorts above, so
    // this also proves the ordering is really being applied.
    REQUIRE(run(midi::StrumDirection::AsPlayed) == std::vector<std::uint8_t>{64, 60, 67});
}

TEST_CASE("strum shape curves match integer step arithmetic", "[midi][strum]") {
    // Oracle derived here from the documented integer law, not from the kernel.
    constexpr std::size_t kCount = 5;
    for (std::size_t index = 0; index < kCount; ++index) {
        const auto last = static_cast<std::int64_t>(kCount - 1);
        const auto position = static_cast<std::int64_t>(index);
        REQUIRE(midi::Strum<>::shaped_step(midi::StrumShape::Linear, index, kCount) == position);
        REQUIRE(midi::Strum<>::shaped_step(midi::StrumShape::Accelerate, index, kCount) ==
                position * position / last);
        REQUIRE(midi::Strum<>::shaped_step(midi::StrumShape::Decelerate, index, kCount) ==
                last - (last - position) * (last - position) / last);
    }
    // A one-note cluster has no spread at all.
    REQUIRE(midi::Strum<>::shaped_step(midi::StrumShape::Accelerate, 0, 1) == 0);
}

TEST_CASE("strum with zero jitter is exactly deterministic", "[midi][strum][parity]") {
    const std::array input{on(0, 60, 100), on(0, 64, 100), on(0, 67, 100),
                           off(20'000, 60), off(20'000, 64), off(20'000, 67)};
    auto run = [&](std::span<const std::int32_t> partitions, std::uint64_t seed) {
        midi::StrumSpec spec{};
        spec.sync = midi::StrumSpacingSync::Milliseconds;
        spec.spacing_milliseconds = 15;
        spec.window_samples = 128;
        spec.timing_jitter_samples = 0;
        spec.velocity_jitter = 0;
        spec.seed = seed;
        midi::Strum<> strum{spec};
        return render(
            [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
                strum.process(in, o, constant_block(start, count));
            },
            48'000, partitions, input);
    };
    // With no jitter the seed cannot matter, and the partition cannot either.
    REQUIRE(identical(run(kWholeBlock, 1), run(kWholeBlock, 999'999)));
    REQUIRE(identical(run(kWholeBlock, 1), run(kRaggedBlocks, 1)));
}

TEST_CASE("strum allocates nothing while processing", "[midi][strum][rt-safety]") {
    midi::StrumSpec spec{};
    spec.window_samples = 64;
    midi::Strum<> strum{spec};
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    REQUIRE(input.add(midi::MidiEvent::note_on(0, 60, 100)));
    REQUIRE(input.add(midi::MidiEvent::note_on(0, 64, 100)));
    {
        pulp::test::RtAllocationProbe probe;
        strum.process(input, output, constant_block(0, 512));
        REQUIRE_FALSE(probe.saw_allocation());
    }
}

// ---------------------------------------------------------------------------
// Chord memory
// ---------------------------------------------------------------------------

TEST_CASE("chord memory with nothing captured is exact passthrough",
          "[midi][chord-memory][parity]") {
    // Negative control for the bypass claim.
    const std::array input{on(0, 60), on(10, 64), off(500, 60), off(510, 64)};
    midi::ChordMemory<> memory{};
    REQUIRE(memory.memory_empty());
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t, std::int32_t) { memory.process(in, o); }, 1'000,
        kWholeBlock, input);
    REQUIRE(out.size() == input.size());
    for (std::size_t index = 0; index < input.size(); ++index)
        REQUIRE(out[index].identity() == input[index].identity());
}

TEST_CASE("chord memory preserves its interval set across transposition",
          "[midi][chord-memory]") {
    // The oracle is the interval set itself: a captured shape must survive every
    // trigger unchanged, which is what makes it a chord memory rather than a
    // stored chord.
    const std::array<std::uint8_t, 3> captured{60, 64, 67}; // major triad: 0, 4, 7
    midi::ChordMemory<> memory{};
    REQUIRE(memory.learn(captured));
    REQUIRE_FALSE(memory.memory_empty());

    for (std::uint8_t trigger : {48, 55, 61, 72}) {
        std::array<std::uint8_t, midi::ChordMemory<>::kMaxChordNotes> chord{};
        const auto count = memory.chord_for(trigger, chord);
        REQUIRE(count == 3);
        REQUIRE(chord[0] - trigger == 0);
        REQUIRE(chord[1] - trigger == 4);
        REQUIRE(chord[2] - trigger == 7);
    }
}

TEST_CASE("chord memory plays and releases the whole chord", "[midi][chord-memory]") {
    const std::array<std::uint8_t, 3> captured{60, 64, 67};
    midi::ChordMemory<> memory{};
    REQUIRE(memory.learn(captured));
    const std::array input{on(0, 55, 100), off(1'000, 55)};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t, std::int32_t) { memory.process(in, o); }, 2'000,
        kWholeBlock, input);
    REQUIRE(attack_notes(out) == std::vector<std::uint8_t>{55, 59, 62});
    EventLedger ledger;
    for (const auto& event : out)
        ledger.feed(event);
    REQUIRE(ledger.balanced());
    REQUIRE(memory.empty());
}

TEST_CASE("chord memory scale-degree mode stays diatonic", "[midi][chord-memory]") {
    // A parallel transposition of a C major triad to D gives F#; a scale-degree
    // replay in C major must give F natural instead.
    const std::array<std::uint8_t, 3> captured{60, 64, 67};
    midi::ChordMemorySpec spec{};
    spec.mode = midi::ChordMemoryMode::ScaleDegree;
    spec.scale_root = music::PitchClass::c;
    spec.scale = music::NamedScale::major;
    midi::ChordMemory<> memory{spec};
    REQUIRE(memory.learn(captured));

    std::array<std::uint8_t, midi::ChordMemory<>::kMaxChordNotes> chord{};
    const auto count = memory.chord_for(62, chord); // trigger D
    REQUIRE(count == 3);
    REQUIRE(chord[0] == 62); // D
    REQUIRE(chord[1] == 65); // F natural, not F#
    REQUIRE(chord[2] == 69); // A

    // Sensitivity control: the parallel mode on the same capture really does
    // produce F#, so the diatonic reading above is a behaviour difference and
    // not an accident of the trigger.
    midi::ChordMemory<> parallel{};
    REQUIRE(parallel.learn(captured));
    std::array<std::uint8_t, midi::ChordMemory<>::kMaxChordNotes> parallel_chord{};
    REQUIRE(parallel.chord_for(62, parallel_chord) == 3);
    REQUIRE(parallel_chord[1] == 66); // F#
}

TEST_CASE("chord memory per-key mode answers each trigger separately",
          "[midi][chord-memory]") {
    midi::ChordMemorySpec spec{};
    spec.mode = midi::ChordMemoryMode::PerKey;
    midi::ChordMemory<> memory{spec};
    const std::array<std::uint8_t, 3> major{60, 64, 67};
    const std::array<std::uint8_t, 3> minor{62, 65, 69};
    REQUIRE(memory.learn_for(60, major));
    REQUIRE(memory.learn_for(62, minor));

    std::array<std::uint8_t, midi::ChordMemory<>::kMaxChordNotes> chord{};
    REQUIRE(memory.chord_for(60, chord) == 3);
    REQUIRE(chord[1] - 60 == 4);
    REQUIRE(memory.chord_for(62, chord) == 3);
    REQUIRE(chord[1] - 62 == 3);
}

TEST_CASE("chord memory revoicing reduces motion against the music reference",
          "[midi][chord-memory]") {
    // The reference is pulp::music's own voice-leading solver: the revoiced
    // chord must be the one that solver picks, and it must move no further than
    // the parallel transposition it replaces.
    const std::array<std::uint8_t, 3> captured{60, 64, 67};
    midi::ChordMemorySpec spec{};
    spec.revoice = true;
    midi::ChordMemory<> memory{spec};
    REQUIRE(memory.learn(captured));

    const std::array input{on(0, 60, 100), off(500, 60), on(1'000, 65, 100), off(1'500, 65)};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t, std::int32_t) { memory.process(in, o); }, 2'000,
        kWholeBlock, input);

    std::vector<std::uint8_t> second;
    for (const auto& event : out)
        if (event.attack() && event.sample == 1'000)
            second.push_back(event.event.note());
    REQUIRE(second.size() == 3);

    const std::array<int, 3> previous{60, 64, 67};
    const std::array<int, 3> intervals{0, 4, 7};
    const auto formula = music::ChordFormula::from_intervals(intervals);
    REQUIRE(formula);
    const auto reference = music::minimum_motion_voice_leading(
        previous, music::PitchClass::f, *formula, music::MidiRange{});
    REQUIRE(reference);
    const auto reference_pitches = reference->pitches();
    REQUIRE(second.size() == reference_pitches.size());
    for (std::size_t index = 0; index < second.size(); ++index)
        REQUIRE(second[index] == reference_pitches[index]);

    // And it really is less motion than the parallel transposition {65, 69, 72}.
    const std::array<int, 3> parallel{65, 69, 72};
    int revoiced_motion = 0;
    int parallel_motion = 0;
    for (std::size_t index = 0; index < 3; ++index) {
        revoiced_motion += std::abs(static_cast<int>(second[index]) - previous[index]);
        parallel_motion += std::abs(parallel[index] - previous[index]);
    }
    REQUIRE(revoiced_motion <= parallel_motion);
}

TEST_CASE("chord memory retrigger does not strand the previous chord",
          "[midi][chord-memory]") {
    const std::array<std::uint8_t, 3> captured{60, 64, 67};
    midi::ChordMemory<> memory{};
    REQUIRE(memory.learn(captured));
    // Two attacks on one key with no release between them.
    const std::array input{on(0, 60, 100), on(100, 60, 100), off(200, 60)};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t, std::int32_t) { memory.process(in, o); }, 1'000,
        kWholeBlock, input);
    EventLedger ledger;
    for (const auto& event : out)
        ledger.feed(event);
    auto flush_out = prepared_buffer();
    memory.flush(flush_out);
    for (const auto& event : flush_out)
        ledger.feed({0, event});
    REQUIRE(ledger.balanced());
    REQUIRE(memory.empty());
}

TEST_CASE("chord memory allocates nothing while processing", "[midi][chord-memory][rt-safety]") {
    const std::array<std::uint8_t, 3> captured{60, 64, 67};
    midi::ChordMemory<> memory{};
    REQUIRE(memory.learn(captured));
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    REQUIRE(input.add(midi::MidiEvent::note_on(0, 55, 100)));
    {
        pulp::test::RtAllocationProbe probe;
        memory.process(input, output);
        REQUIRE_FALSE(probe.saw_allocation());
    }
}

// ---------------------------------------------------------------------------
// Shared utility-kernel contract
// ---------------------------------------------------------------------------

TEST_CASE("every MIDI utility kernel declares a bounded contract", "[midi][parity]") {
    const std::array contracts{midi::Latch::contract(),        midi::Humanize<>::contract(),
                               midi::NoteRepeat<>::contract(), midi::NoteDelay<>::contract(),
                               midi::Strum<>::contract(),      midi::ChordMemory<>::contract()};
    for (const auto& contract : contracts) {
        REQUIRE(contract.maximum_event_amplification >= 1);
        REQUIRE(contract.state_capacity > 0);
        REQUIRE(contract.requires_reserved_capacity_limited_output);
        REQUIRE(contract.requires_distinct_input_output);
    }
}

TEST_CASE("every MIDI utility kernel refuses an aliased output block", "[midi][rt-safety]") {
    // The contract declares distinct input and output blocks; each kernel must
    // enforce it rather than corrupt the buffer it is reading.
    auto shared = prepared_buffer();
    REQUIRE(shared.add(midi::MidiEvent::note_on(0, 60, 100)));

    midi::Latch latch{{midi::LatchMode::Hold}};
    REQUIRE_FALSE(latch.process(shared, shared).complete);

    midi::Humanize<> humanize{{16, 4, 1}};
    REQUIRE_FALSE(humanize.process(shared, shared, {0}, 512).complete);

    midi::NoteRepeat<> repeat{{{kSixteenthTicks}, 4, 50, 100, 100, 0}};
    REQUIRE_FALSE(repeat.process(shared, shared, constant_block(0, 512)).complete);

    midi::NoteDelaySpec delay_spec{};
    delay_spec.repeats = 2;
    midi::NoteDelay<> delay{delay_spec};
    REQUIRE_FALSE(delay.process(shared, shared, constant_block(0, 512)).complete);

    midi::StrumSpec strum_spec{};
    strum_spec.window_samples = 32;
    midi::Strum<> strum{strum_spec};
    REQUIRE_FALSE(strum.process(shared, shared, constant_block(0, 512)).complete);

    midi::ChordMemory<> memory{};
    REQUIRE_FALSE(memory.process(shared, shared).complete);
}

// ---------------------------------------------------------------------------
// Contract surfaces the cases above do not reach
// ---------------------------------------------------------------------------

namespace {

std::vector<std::uint8_t> strum_order(midi::StrumDirection direction, std::uint64_t seed,
                                      int clusters) {
    std::vector<AbsoluteMidiEvent> input;
    for (int cluster = 0; cluster < clusters; ++cluster) {
        const auto at = static_cast<std::int64_t>(cluster) * 20'000;
        for (std::uint8_t note : {64, 60, 67, 72}) {
            input.push_back(on(at, note, 100));
            input.push_back(off(at + 15'000, note));
        }
    }
    std::stable_sort(input.begin(), input.end(),
                     [](const auto& a, const auto& b) { return a.sample < b.sample; });
    midi::StrumSpec spec{};
    spec.direction = direction;
    spec.sync = midi::StrumSpacingSync::Milliseconds;
    spec.spacing_milliseconds = 10;
    spec.window_samples = 64;
    spec.seed = seed;
    midi::Strum<> strum{spec};
    return attack_notes(render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            strum.process(in, o, constant_block(start, count));
        },
        static_cast<std::int64_t>(clusters) * 20'000 + 20'000, kRaggedBlocks, input));
}

} // namespace

TEST_CASE("strum alternate flips direction between clusters", "[midi][strum]") {
    const auto notes = strum_order(midi::StrumDirection::Alternate, 0, 2);
    REQUIRE(notes.size() == 8);
    const std::vector<std::uint8_t> first(notes.begin(), notes.begin() + 4);
    const std::vector<std::uint8_t> second(notes.begin() + 4, notes.end());
    REQUIRE(first == std::vector<std::uint8_t>{60, 64, 67, 72});
    REQUIRE(second == std::vector<std::uint8_t>{72, 67, 64, 60});
}

TEST_CASE("strum random is a stable seeded permutation", "[midi][strum]") {
    const auto a = strum_order(midi::StrumDirection::Random, 12'345, 1);
    const auto b = strum_order(midi::StrumDirection::Random, 12'345, 1);
    REQUIRE(a.size() == 4);
    REQUIRE(a == b); // same seed, same order

    // It really is a permutation of the cluster, not a dropped or invented note.
    auto sorted = a;
    std::sort(sorted.begin(), sorted.end());
    REQUIRE(sorted == std::vector<std::uint8_t>{60, 64, 67, 72});

    // Sensitivity control: some seed must order differently from ascending,
    // otherwise "random" could be silently returning the Up order and every
    // assertion above would still pass.
    bool saw_non_ascending = false;
    for (std::uint64_t seed = 1; seed < 40 && !saw_non_ascending; ++seed)
        saw_non_ascending = strum_order(midi::StrumDirection::Random, seed, 1) !=
                            std::vector<std::uint8_t>{60, 64, 67, 72};
    REQUIRE(saw_non_ascending);
}

TEST_CASE("strum jitter moves notes and stays inside its bound", "[midi][strum]") {
    constexpr std::int64_t kJitter = 512;
    constexpr std::uint8_t kVelocityJitter = 10;
    const std::array input{on(0, 60, 100), on(0, 64, 100), on(0, 67, 100), on(0, 72, 100),
                           off(30'000, 60), off(30'000, 64), off(30'000, 67), off(30'000, 72)};
    auto run = [&](std::int64_t timing, std::uint8_t velocity) {
        midi::StrumSpec spec{};
        spec.sync = midi::StrumSpacingSync::Milliseconds;
        spec.spacing_milliseconds = 20;
        spec.window_samples = 64;
        spec.timing_jitter_samples = timing;
        spec.velocity_jitter = velocity;
        spec.seed = 99;
        midi::Strum<> strum{spec};
        return render(
            [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
                strum.process(in, o, constant_block(start, count));
            },
            48'000, kRaggedBlocks, input);
    };
    const auto plain = run(0, 0);
    const auto jittered = run(kJitter, kVelocityJitter);
    REQUIRE(count_attacks(plain) == 4);
    REQUIRE(count_attacks(jittered) == 4);
    // Jitter must actually change the stream — otherwise the bound assertions
    // below would pass on a kernel that ignores the setting entirely.
    REQUIRE_FALSE(identical(plain, jittered));

    for (std::size_t index = 0; index < plain.size(); ++index) {
        if (!plain[index].attack())
            continue;
        const auto shift = jittered[index].sample - plain[index].sample;
        REQUIRE(shift >= 0);
        REQUIRE(shift <= kJitter);
        const auto velocity = static_cast<int>(jittered[index].event.velocity());
        REQUIRE(velocity >= 100 - static_cast<int>(kVelocityJitter));
        REQUIRE(velocity <= 100 + static_cast<int>(kVelocityJitter));
    }
}

TEST_CASE("strum flush emits every buffered note", "[midi][strum]") {
    // Notes admitted but never released: flush must not strand them in the
    // cluster buffer.
    const std::array input{on(0, 60, 100), on(0, 64, 100)};
    midi::StrumSpec spec{};
    spec.sync = midi::StrumSpacingSync::Milliseconds;
    spec.spacing_milliseconds = 500;
    spec.window_samples = 48'000;
    midi::Strum<> strum{spec};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            strum.process(in, o, constant_block(start, count));
        },
        64, kWholeBlock, input);
    REQUIRE(out.empty()); // still inside the cluster window
    REQUIRE_FALSE(strum.empty());

    auto flushed = prepared_buffer();
    strum.flush(flushed, constant_block(64, 64));
    REQUIRE(flushed.size() == 2);
    REQUIRE(strum.empty());
}

TEST_CASE("latch replace_spec releases what it owns before switching",
          "[midi][latch]") {
    midi::Latch latch{{midi::LatchMode::Hold}};
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    REQUIRE(input.add(midi::MidiEvent::note_on(0, 60, 100)));
    latch.process(input, output);
    REQUIRE(latch.owned_depth(0, 60) == 1);

    auto swap_out = prepared_buffer();
    const auto report = latch.replace_spec({midi::LatchMode::Toggle}, swap_out);
    REQUIRE(report.complete);
    REQUIRE(swap_out.size() == 1);
    REQUIRE(swap_out[0].is_note_off());
    REQUIRE(latch.empty());
    REQUIRE(latch.spec().mode == midi::LatchMode::Toggle);
}

TEST_CASE("note repeat and note delay release owned notes on flush", "[midi][rt-safety]") {
    // Both kernels own notes that outlive a block, so a transport stop must not
    // leave anything sounding.
    {
        const std::array input{on(0, 60, 100)};
        midi::NoteRepeat<> repeat{{{kSixteenthTicks}, 4, 50, 100, 100, 0}};
        auto out = render(
            [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
                repeat.process(in, o, constant_block(start, count));
            },
            3'000, kWholeBlock, input);
        EventLedger ledger;
        for (const auto& event : out)
            ledger.feed(event);
        REQUIRE_FALSE(ledger.balanced()); // a note is deliberately still sounding
        auto flushed = prepared_buffer();
        repeat.flush(flushed, constant_block(3'000, 512));
        for (const auto& event : flushed)
            ledger.feed({3'000, event});
        REQUIRE(ledger.balanced());
        REQUIRE(repeat.empty());
    }
    {
        // note_delay is a send: the dry note stays the player's, so the input
        // carries its authored release and only the echoes are the kernel's to
        // balance. Feeding an unreleased dry note here would assert the kernel
        // owns something it deliberately does not.
        const std::array input{on(0, 60, 100), off(7'000, 60)};
        midi::NoteDelaySpec spec{};
        spec.interval = {kSixteenthTicks};
        spec.repeats = 3;
        midi::NoteDelay<> delay{spec};
        auto out = render(
            [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
                delay.process(in, o, constant_block(start, count));
            },
            9'000, kWholeBlock, input);
        EventLedger ledger;
        for (const auto& event : out)
            ledger.feed(event);
        auto flushed = prepared_buffer();
        delay.flush(flushed, constant_block(9'000, 512));
        for (const auto& event : flushed)
            ledger.feed({9'000, event});
        REQUIRE(ledger.balanced());
        REQUIRE(delay.empty());
    }
}

TEST_CASE("every MIDI utility kernel rejects a spec it cannot honour", "[midi][parity]") {
    REQUIRE_FALSE(midi::NoteRepeat<>::valid_spec({{0}, 4, 50, 100, 100, 0}));      // no interval
    REQUIRE_FALSE(midi::NoteRepeat<>::valid_spec({{100}, 4, 0, 100, 100, 0}));     // zero gate
    REQUIRE_FALSE(midi::NoteRepeat<>::valid_spec({{100}, 4, 50, 200, 100, 0}));    // probability > 100
    REQUIRE_FALSE(midi::Humanize<>::valid_spec({-1, 0, 0}));                       // negative jitter

    midi::NoteDelaySpec delay{};
    delay.sync = midi::NoteDelaySync::Milliseconds;
    delay.milliseconds = 0;
    REQUIRE_FALSE(midi::NoteDelay<>::valid_spec(delay));

    midi::StrumSpec strum{};
    strum.window_samples = -1;
    REQUIRE_FALSE(midi::Strum<>::valid_spec(strum));

    // An invalid spec must leave the kernel refusing to run rather than
    // silently substituting a default.
    midi::NoteRepeat<> invalid{{{0}, 4, 50, 100, 100, 0}};
    REQUIRE_FALSE(invalid.valid());
    auto in = prepared_buffer();
    auto out = prepared_buffer();
    REQUIRE(in.add(midi::MidiEvent::note_on(0, 60, 100)));
    REQUIRE_FALSE(invalid.process(in, out, constant_block(0, 512)).complete);
}

TEST_CASE("chord memory returns to passthrough when its memory is cleared",
          "[midi][chord-memory][parity]") {
    const std::array<std::uint8_t, 3> captured{60, 64, 67};
    midi::ChordMemory<> memory{};
    REQUIRE(memory.learn(captured));
    REQUIRE_FALSE(memory.memory_empty());
    memory.clear_memory();
    REQUIRE(memory.memory_empty());

    const std::array input{on(0, 55, 100), off(500, 55)};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t, std::int32_t) { memory.process(in, o); }, 1'000,
        kWholeBlock, input);
    REQUIRE(out.size() == 2);
    for (std::size_t index = 0; index < out.size(); ++index)
        REQUIRE(out[index].identity() == input[index].identity());
}

TEST_CASE("note delay retrigger still ends the first attack's echoes",
          "[midi][note-delay][rt-safety]") {
    // Pressing the same key again before releasing it must not strand the
    // echoes the first press scheduled. They cannot wait for the second press's
    // release — that release describes a different note — so they take the
    // length the first note had actually been held when it was retriggered.
    const std::array input{on(0, 60, 100), on(3'000, 60, 100), off(3'500, 60)};
    midi::NoteDelaySpec spec{};
    spec.interval = {kSixteenthTicks};
    spec.repeats = 2;
    spec.velocity_decay_percent = 100;
    // Transposing the echoes puts them on their own pitches, so the ledger below
    // measures only what the kernel owns. The dry stream here is deliberately
    // unbalanced — two presses and one release is what a retrigger IS — and that
    // is the player's note, not the kernel's, so folding it in would assert the
    // opposite of the send contract.
    spec.transpose_semitones = 4;
    midi::NoteDelay<> delay{spec};
    auto out = render(
        [&](const auto& in, auto& o, std::int64_t start, std::int32_t count) {
            delay.process(in, o, constant_block(start, count));
        },
        40'000, kRaggedBlocks, input);

    EventLedger ledger;
    std::size_t echo_attacks = 0;
    for (const auto& event : out) {
        if (event.event.note() == 60)
            continue; // the dry note, which this kernel forwards and never owns
        ledger.feed(event);
        if (event.attack())
            ++echo_attacks;
    }
    // Both presses scheduled two echoes each, and every one of them ended on its
    // own without a flush.
    REQUIRE(echo_attacks == 4);
    REQUIRE(ledger.balanced());
    REQUIRE(delay.empty());

    // The lengths are the real claim. The first press was held 3'000 samples
    // before being retriggered and the second only 500, so the echoes of each
    // press must carry their OWN press's length. A kernel that simply left the
    // first press's echoes armed would hand them the second press's 500 and
    // still look perfectly balanced here.
    std::vector<std::int64_t> lengths;
    std::vector<std::int64_t> open_at;
    for (const auto& event : out) {
        if (event.event.note() != 64)
            continue; // the first echo of each press
        if (event.attack()) {
            open_at.push_back(event.sample);
        } else if (!open_at.empty()) {
            lengths.push_back(event.sample - open_at.front());
            open_at.erase(open_at.begin());
        }
    }
    REQUIRE(lengths == std::vector<std::int64_t>{3'000, 500});
}

TEST_CASE("chord memory retrigger reuses its own slot at capacity",
          "[midi][chord-memory][rt-safety]") {
    // Fill every trigger slot, then retrigger a key that already holds one. That
    // press frees a slot before it needs one, so it must sound. Taking the slot
    // before releasing the old chord makes the kernel drop a chord it had room
    // for, and only shows up once the table is actually full.
    constexpr std::size_t kSlots = 4;
    const std::array<std::uint8_t, 3> captured{60, 64, 67};
    midi::ChordMemory<kSlots> memory{};
    REQUIRE(memory.learn(captured));

    std::vector<AbsoluteMidiEvent> input;
    for (std::size_t index = 0; index < kSlots; ++index)
        input.push_back(on(static_cast<std::int64_t>(index) * 10,
                           static_cast<std::uint8_t>(50 + index * 2), 100));
    // Retrigger the first key while all four slots are occupied.
    input.push_back(on(500, 50, 100));

    auto out = render(
        [&](const auto& in, auto& o, std::int64_t, std::int32_t) { memory.process(in, o); }, 1'000,
        kWholeBlock, input);

    std::size_t retrigger_attacks = 0;
    for (const auto& event : out)
        if (event.attack() && event.sample == 500)
            ++retrigger_attacks;
    REQUIRE(retrigger_attacks == 3); // the retriggered chord sounded

    EventLedger ledger;
    for (const auto& event : out)
        ledger.feed(event);
    REQUIRE(ledger.no_orphans());
    auto flushed = prepared_buffer();
    memory.flush(flushed);
    for (const auto& event : flushed)
        ledger.feed({500, event});
    REQUIRE(ledger.balanced());
}
