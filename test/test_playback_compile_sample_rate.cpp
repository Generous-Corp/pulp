#include <pulp/playback/program_compiler.hpp>
#include <pulp/timeline/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;
using namespace pulp::timeline;

namespace {

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

constexpr TickPosition kNoteOnTick{kTicksPerQuarter};
constexpr TickPosition kNoteOffTick{2 * kTicksPerQuarter};

std::shared_ptr<const CompiledTempoMap> tempo_map_at(RationalRate rate) {
    const std::array points{TempoPoint{{0}, 120.0}};
    return std::make_shared<const CompiledTempoMap>(take(CompiledTempoMap::compile(points, rate)));
}

/// Authored purely in ticks. Nothing here mentions a sample rate, so every
/// frame position in the compiled program is produced by the compile rate
/// alone -- which is what lets the goldens below distinguish the three rates.
std::shared_ptr<const Project> tick_authored_project() {
    auto notes = take(MidiContent::create(
        {{{22}, kNoteOnTick, TickDuration{kNoteOffTick.value - kNoteOnTick.value}, 0x8000, 64, 0}}));
    auto clip = take(
        Clip::create({21}, {0}, TickDuration{4 * kTicksPerQuarter}, std::move(notes)));
    auto track = take(Track::create({20}, "notes", {std::move(clip)}));
    auto sequence = take(Sequence::create({2}, "root", TickDuration{4 * kTicksPerQuarter},
                                          {std::move(track)}));
    return std::make_shared<const Project>(take(Project::create(
        ProjectInput{.id = {1},
                     .name = "compile rate",
                     .next_item_id = 31,
                     .root_sequence_id = {2},
                     .sequences = {std::move(sequence)}})));
}

struct CompileOutcome {
    bool submitted = false;
    CompileErrorCode error = CompileErrorCode::InvalidRequest;
    std::vector<std::int64_t> note_event_frames;
    RationalRate program_rate;
};

/// Compiles the tick-authored project, stating `requested` as the render rate.
/// `map_rate` defaults to the same value; passing a different one exercises the
/// disagreement guard.
CompileOutcome compile_at(RationalRate requested, RationalRate map_rate) {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));

    ProgramCompileRequest request;
    request.project = tick_authored_project();
    request.sequence_id = {2};
    request.tempo_map = tempo_map_at(map_rate);
    request.sample_rate = requested;
    request.document_revision = 1;
    request.dirty.all = true;

    CompileOutcome outcome;
    auto submitted = compiler.submit(std::move(request));
    if (!submitted) {
        outcome.error = submitted.error().code;
        return outcome;
    }
    outcome.submitted = true;
    while (compiler.status().busy)
        executor.run_for(std::chrono::seconds(1), 64);
    REQUIRE_FALSE(compiler.status().has_error);
    REQUIRE(store.has_value());

    auto program = store.read();
    REQUIRE(program);
    outcome.program_rate = program->tempo_map().sample_rate();
    const auto* track = program->find_track({20});
    REQUIRE(track != nullptr);
    for (const auto& event : track->arrangement_note_events())
        outcome.note_event_frames.push_back(event.sample.value);
    return outcome;
}

CompileOutcome compile_at(RationalRate rate) {
    return compile_at(rate, rate);
}

} // namespace

// One quarter note at 120 BPM is half a second, so the note's on/off frames are
// rate * 0.5 and rate * 1.0. These three goldens are the whole point of stating
// the rate: they are distinct at every rate and exactly proportional to it, so a
// compile that ignored the requested rate cannot land on all three.
TEST_CASE("compiled note frames follow the requested rate", "[playback][timebase]") {
    SECTION("44.1 kHz") {
        const auto outcome = compile_at(RationalRate{44'100, 1});
        REQUIRE(outcome.submitted);
        REQUIRE(outcome.note_event_frames == std::vector<std::int64_t>{22'050, 44'100});
    }
    SECTION("48 kHz") {
        const auto outcome = compile_at(RationalRate{48'000, 1});
        REQUIRE(outcome.submitted);
        REQUIRE(outcome.note_event_frames == std::vector<std::int64_t>{24'000, 48'000});
    }
    SECTION("96 kHz") {
        const auto outcome = compile_at(RationalRate{96'000, 1});
        REQUIRE(outcome.submitted);
        REQUIRE(outcome.note_event_frames == std::vector<std::int64_t>{48'000, 96'000});
    }
}

// Guards the goldens above against being accidentally identical, or all three
// being generated at one rate: a run that produced the same geometry three times
// would satisfy each SECTION's shape but fail here.
TEST_CASE("the three compile rates produce distinct frame geometry", "[playback][timebase]") {
    const auto at_44k = compile_at(RationalRate{44'100, 1});
    const auto at_48k = compile_at(RationalRate{48'000, 1});
    const auto at_96k = compile_at(RationalRate{96'000, 1});
    REQUIRE(at_44k.submitted);
    REQUIRE(at_48k.submitted);
    REQUIRE(at_96k.submitted);

    REQUIRE(at_44k.note_event_frames.size() == 2);
    REQUIRE(at_44k.note_event_frames != at_48k.note_event_frames);
    REQUIRE(at_48k.note_event_frames != at_96k.note_event_frames);
    REQUIRE(at_44k.note_event_frames != at_96k.note_event_frames);

    // Doubling the rate doubles every frame position. This is the assertion a
    // compile that quietly used one fixed rate cannot satisfy.
    for (std::size_t i = 0; i != at_48k.note_event_frames.size(); ++i)
        REQUIRE(at_96k.note_event_frames[i] == 2 * at_48k.note_event_frames[i]);
}

TEST_CASE("the compiler rejects a rate that disagrees with the tempo map", "[playback][timebase]") {
    SECTION("stated rate differs from the map's") {
        const auto outcome = compile_at(RationalRate{44'100, 1}, RationalRate{48'000, 1});
        REQUIRE_FALSE(outcome.submitted);
        REQUIRE(outcome.error == CompileErrorCode::InvalidRequest);
    }
    SECTION("rate left unstated") {
        const auto outcome = compile_at(RationalRate{0, 1}, RationalRate{48'000, 1});
        REQUIRE_FALSE(outcome.submitted);
        REQUIRE(outcome.error == CompileErrorCode::InvalidRequest);
    }
    SECTION("an unnormalized statement of the same rate is accepted") {
        const auto outcome = compile_at(RationalRate{96'000, 2}, RationalRate{48'000, 1});
        REQUIRE(outcome.submitted);
        REQUIRE(outcome.program_rate == RationalRate{48'000, 1});
        REQUIRE(outcome.note_event_frames == std::vector<std::int64_t>{24'000, 48'000});
    }
}
