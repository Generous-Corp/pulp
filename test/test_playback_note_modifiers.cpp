#include <pulp/midi/block_ops.hpp>
#include <pulp/playback/note_renderer.hpp>
#include <pulp/playback/program_compiler.hpp>

#include "harness/scoped_rt_process_probe.hpp"
#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;
using namespace pulp::timeline;

namespace {

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    if (!result)
        std::abort();
    return std::move(result).value();
}

std::shared_ptr<const CompiledTempoMap> modifier_tempo_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return shared_compiled_tempo_map(points, RationalRate{48'000, 1});
}

constexpr TickDuration kLoopLength{kTicksPerQuarter * 4};

/// One note per bar-long loop, so a pass index maps one-to-one onto a decision.
std::shared_ptr<const Project> modifier_project(std::vector<NoteModifier> modifiers,
                                                std::uint64_t seed) {
    std::vector<NoteEvent> notes{
        {{30}, {0}, {kTicksPerQuarter}, 0xffff, 60, 0},
        {{31}, {kTicksPerQuarter * 2}, {kTicksPerQuarter}, 0xffff, 64, 0},
    };
    auto content = take(NoteContent::create(std::move(notes), std::move(modifiers), seed));
    auto clip = take(Clip::create({20}, {0}, kLoopLength, std::move(content)));
    auto track = take(Track::create({10}, "notes", {std::move(clip)}));
    auto sequence = take(Sequence::create({2}, "root", kLoopLength, {std::move(track)}));
    return std::make_shared<const Project>(take(
        Project::create(ProjectInput{{1}, "project", 100, {2}, {}, {std::move(sequence)}})));
}

NoteModifier chance(std::uint64_t note, std::uint16_t probability) {
    NoteModifier modifier;
    modifier.note_id = {note};
    modifier.probability = probability;
    return modifier;
}

struct ProgramHarness {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler{store, executor, std::chrono::microseconds(0)};
    std::uint64_t next_revision = 1;

    void publish(std::shared_ptr<const Project> project,
                 std::shared_ptr<const CompiledTempoMap> map) {
        ProgramCompileRequest request;
        request.project = std::move(project);
        request.sequence_id = {2};
        request.tempo_map = std::move(map);
        request.document_revision = next_revision++;
        request.dirty = {.all = true};
        REQUIRE(compiler.submit(std::move(request)));
        while (compiler.status().busy)
            executor.run_for(std::chrono::seconds(1), 64);
        REQUIRE_FALSE(compiler.status().has_error);
    }
};

/// The note-on pitches emitted per loop pass, played straight through with the
/// loop enabled so the pass index advances the way a session actually does.
std::vector<std::vector<std::uint8_t>> pitches_per_pass(ProgramHarness& programs,
                                                        const CompiledTempoMap& map,
                                                        std::size_t passes,
                                                        bool saturate_monotonic = false) {
    // The block size divides the loop exactly, so every rendered block belongs
    // to one pass and a bucket can never borrow an event from its neighbour.
    constexpr std::uint32_t kBlock = 1'000;
    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(64));
    PlaybackProgramBlockLatch latch;
    MasterTransport transport;
    MasterTransportConfig config;
    config.max_buffer_size = kBlock;
    config.initially_playing = true;
    config.loop = {true, {0}, TickPosition{kLoopLength.value}};
    REQUIRE(transport.prepare(map, config) == TransportError::None);

    const auto loop_samples = map.ticks_to_samples(TickPosition{kLoopLength.value}).value;
    REQUIRE(loop_samples % kBlock == 0);
    const auto blocks_per_pass = static_cast<std::size_t>(loop_samples / kBlock);
    std::vector<std::vector<std::uint8_t>> result;
    for (std::size_t pass = 0; pass < passes; ++pass) {
        std::vector<std::uint8_t> sounded;
        for (std::size_t block = 0; block < blocks_per_pass; ++block) {
            TransportSnapshot snapshot;
            REQUIRE(transport.begin_block(kBlock, snapshot) == TransportError::None);
            if (saturate_monotonic) {
                for (std::uint8_t range = 0; range < snapshot.range_count; ++range) {
                    snapshot.ranges[range].monotonic_start =
                        MonotonicBeat{{std::numeric_limits<std::int64_t>::max()}};
                    snapshot.ranges[range].monotonic_end =
                        MonotonicBeat{{std::numeric_limits<std::int64_t>::max()}};
                }
            }
            auto program = latch.begin_block(programs.store);
            REQUIRE(renderer.process(program, snapshot).code == NoteRenderCode::Ok);
            for (const auto& event : renderer.events())
                if (event.is_note_on())
                    sounded.push_back(event.data()[1]);
        }
        result.push_back(std::move(sounded));
    }
    return result;
}

std::vector<std::vector<std::uint8_t>> pitches_for_seed(std::uint64_t seed,
                                                        std::vector<NoteModifier> modifiers,
                                                        std::size_t passes) {
    const auto map = modifier_tempo_map();
    ProgramHarness programs;
    programs.publish(modifier_project(std::move(modifiers), seed), map);
    return pitches_per_pass(programs, *map, passes);
}

std::size_t count_pitch(const std::vector<std::vector<std::uint8_t>>& passes, std::uint8_t pitch) {
    std::size_t total = 0;
    for (const auto& pass : passes)
        for (const auto value : pass)
            if (value == pitch)
                ++total;
    return total;
}

} // namespace

TEST_CASE("Conditional passes keep advancing after the monotonic clock saturates",
          "[playback][note-modifier][determinism][transport]") {
    const auto map = modifier_tempo_map();
    NoteModifier every_third = chance(30, note_probability_certain);
    every_third.condition = NoteConditionKind::EveryNth;
    every_third.condition_period = 3;
    ProgramHarness programs;
    programs.publish(modifier_project({every_third}, 0), map);

    constexpr std::size_t kPasses = 8;
    const auto sounded = pitches_per_pass(programs, *map, kPasses, true);
    for (std::size_t pass = 0; pass < kPasses; ++pass)
        REQUIRE(count_pitch({sounded[pass]}, 60) == (pass % 3 == 0 ? 1u : 0u));
}

TEST_CASE("A probabilistic note replays identically for one seed and differs for another",
          "[playback][note-modifier][determinism]") {
    constexpr std::size_t kPasses = 24;
    const auto modifiers = std::vector<NoteModifier>{chance(30, note_probability_certain / 2)};

    // Two independent engines, same document and same transport trace: the
    // sounding decisions must agree pass for pass.
    const auto first = pitches_for_seed(0xA11CE, modifiers, kPasses);
    const auto second = pitches_for_seed(0xA11CE, modifiers, kPasses);
    REQUIRE(first == second);

    // The unmodified note sounds on every pass, which keeps the comparison from
    // passing vacuously on a run that emitted nothing at all.
    REQUIRE(count_pitch(first, 64) == kPasses);

    // The modified note is genuinely deciding: it neither always sounds nor
    // never sounds across the sweep.
    const auto sounded = count_pitch(first, 60);
    REQUIRE(sounded > 0);
    REQUIRE(sounded < kPasses);

    // A different seed produces a different pattern of passes.
    const auto other = pitches_for_seed(0xA11CF, modifiers, kPasses);
    REQUIRE(other != first);
    REQUIRE(count_pitch(other, 64) == kPasses);
}

TEST_CASE("Probability zero never sounds and certainty always sounds",
          "[playback][note-modifier][determinism]") {
    constexpr std::size_t kPasses = 16;

    const auto silent = pitches_for_seed(7, {chance(30, 0)}, kPasses);
    REQUIRE(count_pitch(silent, 60) == 0);
    REQUIRE(count_pitch(silent, 64) == kPasses);

    // Certain probability paired with a ratchet, since a bare certainty is a
    // neutral modifier the document model refuses to store.
    NoteModifier certain = chance(30, note_probability_certain);
    certain.ratchet_count = 2;
    const auto certain_passes = pitches_for_seed(7, {certain}, kPasses);
    REQUIRE(count_pitch(certain_passes, 60) == kPasses * 2);
}

TEST_CASE("A pass condition selects the passes a note plays on",
          "[playback][note-modifier][determinism]") {
    constexpr std::size_t kPasses = 12;

    NoteModifier first_only = chance(30, note_probability_certain);
    first_only.condition = NoteConditionKind::First;
    const auto once = pitches_for_seed(3, {first_only}, kPasses);
    REQUIRE(count_pitch(once, 60) == 1);
    REQUIRE(once[0].size() == 2);
    for (std::size_t pass = 1; pass < kPasses; ++pass)
        REQUIRE(count_pitch({once[pass]}, 60) == 0);

    NoteModifier every_third = chance(30, note_probability_certain);
    every_third.condition = NoteConditionKind::EveryNth;
    every_third.condition_period = 3;
    every_third.condition_offset = 0;
    const auto thirds = pitches_for_seed(3, {every_third}, kPasses);
    for (std::size_t pass = 0; pass < kPasses; ++pass)
        REQUIRE(count_pitch({thirds[pass]}, 60) == (pass % 3 == 0 ? 1u : 0u));
}

TEST_CASE("An explicit seek re-anchors conditional note passes",
          "[playback][note-modifier][transport]") {
    const auto map = modifier_tempo_map();
    NoteModifier first_only = chance(30, note_probability_certain);
    first_only.condition = NoteConditionKind::First;
    ProgramHarness programs;
    programs.publish(modifier_project({first_only}, 0), map);

    const auto loop_samples =
        map->ticks_to_samples(TickPosition{kLoopLength.value}).value;
    REQUIRE(loop_samples > 0);
    REQUIRE(loop_samples <= std::numeric_limits<std::uint32_t>::max());
    const auto block_frames = static_cast<std::uint32_t>(loop_samples);

    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(64));
    PlaybackProgramBlockLatch latch;
    MasterTransport transport;
    MasterTransportConfig config;
    config.max_buffer_size = block_frames;
    config.initially_playing = true;
    config.loop = {true, {0}, TickPosition{kLoopLength.value}};
    REQUIRE(transport.prepare(*map, config) == TransportError::None);

    for (int pass = 0; pass < 2; ++pass) {
        TransportSnapshot snapshot;
        REQUIRE(transport.begin_block(block_frames, snapshot) == TransportError::None);
        REQUIRE(renderer.process(latch.begin_block(programs.store), snapshot).code ==
                NoteRenderCode::Ok);
    }

    REQUIRE(transport.seek({0}) == TransportError::None);
    TransportSnapshot after_seek;
    REQUIRE(transport.begin_block(512, after_seek) == TransportError::None);
    REQUIRE(after_seek.reset_requested);
    REQUIRE(renderer.process(latch.begin_block(programs.store), after_seek).code ==
            NoteRenderCode::Ok);
    bool first_note_sounded = false;
    for (const auto& event : renderer.events())
        first_note_sounded |= event.is_note_on() && event.data()[1] == 60;
    REQUIRE(first_note_sounded);
}

TEST_CASE("Program adoption preserves the current conditional note pass",
          "[playback][note-modifier][transport]") {
    const auto map = modifier_tempo_map();
    NoteModifier first_only = chance(30, note_probability_certain);
    first_only.condition = NoteConditionKind::First;
    ProgramHarness programs;
    programs.publish(modifier_project({first_only}, 0), map);

    const auto loop_samples =
        map->ticks_to_samples(TickPosition{kLoopLength.value}).value;
    REQUIRE(loop_samples > 0);
    REQUIRE(loop_samples <= std::numeric_limits<std::uint32_t>::max());
    const auto block_frames = static_cast<std::uint32_t>(loop_samples);

    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(64));
    PlaybackProgramBlockLatch latch;
    MasterTransport transport;
    MasterTransportConfig config;
    config.max_buffer_size = block_frames;
    config.initially_playing = true;
    config.loop = {true, {0}, TickPosition{kLoopLength.value}};
    REQUIRE(transport.prepare(*map, config) == TransportError::None);

    TransportSnapshot first_pass;
    REQUIRE(transport.begin_block(block_frames, first_pass) == TransportError::None);
    REQUIRE(renderer.process(latch.begin_block(programs.store), first_pass).code ==
            NoteRenderCode::Ok);
    REQUIRE_FALSE(renderer.events().empty());
    REQUIRE(renderer.events()[0].is_note_on());
    REQUIRE(renderer.events()[0].data()[1] == 60);

    programs.publish(modifier_project({first_only}, 0), map);
    TransportSnapshot after_adoption;
    REQUIRE(transport.begin_block(block_frames, after_adoption) == TransportError::None);
    const auto adopted =
        renderer.process(latch.begin_block(programs.store), after_adoption);
    REQUIRE(adopted.code == NoteRenderCode::Ok);
    REQUIRE(adopted.adoption == ShellAdoptionResult::Adopted);
    for (const auto& event : renderer.events())
        REQUIRE_FALSE((event.is_note_on() && event.data()[1] == 60));
}

TEST_CASE("A renderer attached mid-loop uses the transport's current conditional pass",
          "[playback][note-modifier][transport]") {
    const auto map = modifier_tempo_map();
    NoteModifier first_only = chance(30, note_probability_certain);
    first_only.condition = NoteConditionKind::First;
    ProgramHarness programs;
    programs.publish(modifier_project({first_only}, 0), map);

    const auto loop_samples =
        map->ticks_to_samples(TickPosition{kLoopLength.value}).value;
    REQUIRE(loop_samples > 0);
    REQUIRE(loop_samples <= std::numeric_limits<std::uint32_t>::max());
    const auto block_frames = static_cast<std::uint32_t>(loop_samples);

    PlaybackProgramBlockLatch latch;
    MasterTransport transport;
    MasterTransportConfig config;
    config.max_buffer_size = block_frames;
    config.initially_playing = true;
    config.loop = {true, {0}, TickPosition{kLoopLength.value}};
    REQUIRE(transport.prepare(*map, config) == TransportError::None);

    TransportSnapshot skipped_first_pass;
    REQUIRE(transport.begin_block(block_frames, skipped_first_pass) == TransportError::None);
    REQUIRE(skipped_first_pass.ranges[0].loop_pass_index == 0);

    TransportSnapshot current_pass;
    REQUIRE(transport.begin_block(block_frames, current_pass) == TransportError::None);
    REQUIRE(current_pass.ranges[0].loop_pass_index == 1);
    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(64));
    REQUIRE(renderer.process(latch.begin_block(programs.store), current_pass).code ==
            NoteRenderCode::Ok);
    for (const auto& event : renderer.events())
        REQUIRE_FALSE((event.is_note_on() && event.data()[1] == 60));
}

TEST_CASE("Non-musical transport metadata preserves the conditional note pass",
          "[playback][note-modifier][transport]") {
    const auto map = modifier_tempo_map();
    NoteModifier first_only = chance(30, note_probability_certain);
    first_only.condition = NoteConditionKind::First;
    ProgramHarness programs;
    programs.publish(modifier_project({first_only}, 0), map);

    const auto loop_samples =
        map->ticks_to_samples(TickPosition{kLoopLength.value}).value;
    REQUIRE(loop_samples > 0);
    REQUIRE(loop_samples <= std::numeric_limits<std::uint32_t>::max());
    const auto block_frames = static_cast<std::uint32_t>(loop_samples);

    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(64));
    PlaybackProgramBlockLatch latch;
    MasterTransport transport;
    MasterTransportConfig config;
    config.max_buffer_size = block_frames;
    config.initially_playing = true;
    config.loop = {true, {0}, TickPosition{kLoopLength.value}};
    REQUIRE(transport.prepare(*map, config) == TransportError::None);

    TransportSnapshot first_pass;
    REQUIRE(transport.begin_block(block_frames, first_pass) == TransportError::None);
    REQUIRE(renderer.process(latch.begin_block(programs.store), first_pass).code ==
            NoteRenderCode::Ok);

    TransportSnapshot metadata_change;
    REQUIRE(transport.begin_block(block_frames, metadata_change) ==
            TransportError::None);
    metadata_change.transport_changed = true;
    REQUIRE(renderer.process(latch.begin_block(programs.store), metadata_change).code ==
            NoteRenderCode::Ok);
    for (const auto& event : renderer.events())
        REQUIRE_FALSE((event.is_note_on() && event.data()[1] == 60));
}

TEST_CASE("Changing a loop flushes notes before reanchoring conditional passes",
          "[playback][note-modifier][transport]") {
    const auto map = modifier_tempo_map();
    ProgramHarness programs;
    programs.publish(modifier_project({}, 0), map);

    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(64));
    PlaybackProgramBlockLatch latch;
    MasterTransport transport;
    MasterTransportConfig config;
    config.max_buffer_size = 1'000;
    config.initially_playing = true;
    config.loop = {true, {0}, TickPosition{kLoopLength.value}};
    REQUIRE(transport.prepare(*map, config) == TransportError::None);

    TransportSnapshot opening;
    REQUIRE(transport.begin_block(1'000, opening) == TransportError::None);
    REQUIRE(renderer.process(latch.begin_block(programs.store), opening).code ==
            NoteRenderCode::Ok);
    REQUIRE(renderer.has_active_notes());

    REQUIRE(transport.set_loop(
                {true, {0}, TickPosition{kLoopLength.value / 2}}) ==
            TransportError::None);
    TransportSnapshot changed;
    REQUIRE(transport.begin_block(1'000, changed) == TransportError::None);
    REQUIRE_FALSE(changed.transport_changed);
    REQUIRE(renderer.process(latch.begin_block(programs.store), changed).code ==
            NoteRenderCode::Ok);
    REQUIRE_FALSE(renderer.events().empty());
    REQUIRE_FALSE(renderer.events()[0].is_note_on());
    REQUIRE(renderer.events()[0].sample_offset == 0);
    REQUIRE_FALSE(renderer.has_active_notes());
}

TEST_CASE("Scrub windows keep conditional notes on pass zero",
          "[playback][note-modifier][transport]") {
    const auto map = modifier_tempo_map();
    NoteModifier first_only = chance(30, note_probability_certain);
    first_only.condition = NoteConditionKind::First;
    ProgramHarness programs;
    programs.publish(modifier_project({first_only}, 0), map);

    const auto loop_samples =
        map->ticks_to_samples(TickPosition{kLoopLength.value}).value;
    REQUIRE(loop_samples > 0);
    REQUIRE(loop_samples <= std::numeric_limits<std::uint32_t>::max());
    const auto block_frames = static_cast<std::uint32_t>(loop_samples);

    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(64));
    PlaybackProgramBlockLatch latch;
    MasterTransport transport;
    MasterTransportConfig config;
    config.max_buffer_size = block_frames;
    config.loop = {true, {0}, TickPosition{kLoopLength.value}};
    REQUIRE(transport.prepare(*map, config) == TransportError::None);
    REQUIRE(transport.begin_scrub(block_frames, {0}) == TransportError::None);

    for (int window = 0; window < 2; ++window) {
        TransportSnapshot snapshot;
        REQUIRE(transport.begin_block(block_frames, snapshot) ==
                TransportError::None);
        REQUIRE(snapshot.scrubbing);
        REQUIRE(renderer.process(latch.begin_block(programs.store), snapshot).code ==
                NoteRenderCode::Ok);
        bool sounded = false;
        for (const auto& event : renderer.events())
            sounded |= event.is_note_on() && event.data()[1] == 60;
        REQUIRE(sounded);
    }
}

TEST_CASE("A ratchet subdivides a note into retriggers that fill its own span",
          "[playback][note-modifier]") {
    const auto map = modifier_tempo_map();
    NoteModifier ratchet = chance(30, note_probability_certain);
    ratchet.ratchet_count = 4;
    ProgramHarness programs;
    programs.publish(modifier_project({ratchet}, 0), map);

    const auto program = programs.store.read();
    REQUIRE(program);
    const auto events = program->find_track({10})->arrangement_note_events();

    std::vector<std::pair<std::int64_t, std::int64_t>> spans;
    std::int64_t open = 0;
    for (const auto& event : events) {
        if (event.note_id != ItemId{30})
            continue;
        if (event.kind == NoteProgramEventKind::On)
            open = event.sample.value;
        else
            spans.push_back({open, event.sample.value});
    }
    REQUIRE(spans.size() == 4);

    // The subdivisions tile the authored span exactly: they abut, the first
    // starts where the note starts, and the last ends where the note ends.
    const auto note_start = map.get()->ticks_to_samples({0}).value;
    const auto note_end = map.get()->ticks_to_samples({kTicksPerQuarter}).value;
    REQUIRE(spans.front().first == note_start);
    REQUIRE(spans.back().second == note_end);
    for (std::size_t index = 1; index < spans.size(); ++index)
        REQUIRE(spans[index].first == spans[index - 1].second);
    for (const auto& span : spans)
        REQUIRE(span.second > span.first);

    // The unmodified sibling note is untouched by its neighbour's ratchet.
    std::size_t sibling_events = 0;
    for (const auto& event : events)
        if (event.note_id == ItemId{31})
            ++sibling_events;
    REQUIRE(sibling_events == 2);
}

TEST_CASE("A ratchet that would collapse below one rendered sample is refused",
          "[playback][note-modifier]") {
    const auto map = modifier_tempo_map();
    NoteModifier ratchet = chance(30, note_probability_certain);
    ratchet.ratchet_count = note_ratchet_maximum;

    // A note one tick long cannot be split, so the compiler must refuse rather
    // than emit a note-on with no matching off.
    auto content = take(NoteContent::create({{{30}, {0}, {1}, 0xffff, 60, 0}}, {ratchet}, 0));
    auto clip = take(Clip::create({20}, {0}, kLoopLength, std::move(content)));
    auto track = take(Track::create({10}, "notes", {std::move(clip)}));
    auto sequence = take(Sequence::create({2}, "root", kLoopLength, {std::move(track)}));
    auto project = std::make_shared<const Project>(take(
        Project::create(ProjectInput{{1}, "project", 100, {2}, {}, {std::move(sequence)}})));

    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler{store, executor, std::chrono::microseconds(0)};
    ProgramCompileRequest request;
    request.project = project;
    request.sequence_id = {2};
    request.tempo_map = map;
    request.document_revision = 1;
    request.dirty = {.all = true};
    REQUIRE(compiler.submit(std::move(request)));
    while (compiler.status().busy)
        executor.run_for(std::chrono::seconds(1), 64);
    REQUIRE(compiler.status().has_error);
    REQUIRE(compiler.status().last_error.code == CompileErrorCode::InvalidStructure);
}

TEST_CASE("Ratchet expansion respects the compiled event budget",
          "[playback][note-modifier][capacity]") {
    const auto map = modifier_tempo_map();
    NoteModifier ratchet = chance(30, note_probability_certain);
    ratchet.ratchet_count = 4;
    auto content = take(NoteContent::create(
        {{{30}, {0}, {kTicksPerQuarter}, 0xffff, 60, 0}}, {ratchet}, 0));
    auto clip = take(Clip::create({20}, {0}, kLoopLength, std::move(content)));
    auto track = take(Track::create({10}, "notes", {std::move(clip)}));
    auto sequence = take(Sequence::create({2}, "root", kLoopLength, {std::move(track)}));
    auto project = std::make_shared<const Project>(take(
        Project::create(ProjectInput{{1}, "project", 100, {2}, {}, {std::move(sequence)}})));

    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler{store, executor, std::chrono::microseconds(0)};
    ProgramCompileRequest request;
    request.project = std::move(project);
    request.sequence_id = {2};
    request.tempo_map = map;
    request.document_revision = 1;
    request.dirty = {.all = true};
    request.maximum_note_events_per_track = 0;
    auto rejected = compiler.submit(request);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == CompileErrorCode::InvalidRequest);

    request.maximum_note_events_per_track = 7;
    REQUIRE(compiler.submit(std::move(request)));
    while (compiler.status().busy)
        executor.run_for(std::chrono::seconds(1), 64);
    REQUIRE(compiler.status().has_error);
    REQUIRE(compiler.status().last_error.code ==
            CompileErrorCode::NoteProgramCapacityExceeded);
}

TEST_CASE("Modifier evaluation allocates nothing on the audio thread",
          "[playback][note-modifier][rt-safety]") {
    const auto map = modifier_tempo_map();
    NoteModifier ratchet = chance(30, note_probability_certain / 2);
    ratchet.ratchet_count = 3;
    ProgramHarness programs;
    programs.publish(modifier_project({ratchet, chance(31, note_probability_certain / 4)}, 42),
                     map);

    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(64));
    PlaybackProgramBlockLatch latch;
    MasterTransport transport;
    MasterTransportConfig config;
    config.max_buffer_size = 512;
    config.initially_playing = true;
    config.loop = {true, {0}, TickPosition{kLoopLength.value}};
    REQUIRE(transport.prepare(*map, config) == TransportError::None);

    // Wrap the loop several times so the probe covers both the gate that admits
    // a note and the gate that rejects one, plus the wrap block itself.
    std::size_t allocations = 1;
    std::size_t note_ons = 0;
    {
        test::ScopedRtProcessProbe probe;
        for (int block = 0; block < 64; ++block) {
            TransportSnapshot snapshot;
            REQUIRE(transport.begin_block(512, snapshot) == TransportError::None);
            auto program = latch.begin_block(programs.store);
            REQUIRE(renderer.process(program, snapshot).code == NoteRenderCode::Ok);
            for (const auto& event : renderer.events())
                if (event.is_note_on())
                    ++note_ons;
        }
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
    // The probe is only meaningful over a run that actually gated notes.
    REQUIRE(note_ons > 0);
}
