#include <pulp/playback/chord_pattern_renderer.hpp>
#include <pulp/playback/production_class.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/timeline/compile_context.hpp>
#include <pulp/timeline/schema_json.hpp>
#include <pulp/timeline/transaction.hpp>

#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timeline;
using namespace pulp::timebase;

namespace {

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    if (!result)
        std::abort();
    return std::move(result).value();
}

SchemaRegistry chord_registry() {
    SchemaRegistryBuilder builder;
    REQUIRE(register_chord_pattern_content_schema(builder));
    return take(std::move(builder).build());
}

Project project_with_harmony(std::vector<ChordScaleEvent> events) {
    SequenceInput sequence;
    sequence.id = {2};
    sequence.name = "harmony";
    sequence.chord_scale_lane = take(ChordScaleLane::create(std::move(events)));
    ProjectInput project;
    project.id = {1};
    project.name = "chord pattern";
    project.next_item_id = 3;
    project.root_sequence_id = {2};
    project.sequences.push_back(take(Sequence::create(std::move(sequence))));
    return take(Project::create(std::move(project)));
}

std::shared_ptr<const Project> playback_project(const SchemaRegistry& schemas,
                                                ChordScaleLane lane) {
    auto pattern = take(create_chord_pattern_content(
        {.seed = 1, .step = {120}, .gate = {90}, .octave = 4, .velocity = 40000}, schemas));
    auto pattern_clip = take(Clip::create({100}, {0}, {480}, std::move(pattern)));
    auto pattern_track = take(Track::create({10}, "pattern", {std::move(pattern_clip)}));
    auto midi = take(MidiContent::create({NoteEvent{{201}, {0}, {120}, 32000, 55, 0}}));
    auto midi_clip = take(Clip::create({200}, {0}, {480}, std::move(midi)));
    auto midi_track = take(Track::create({20}, "authored midi", {std::move(midi_clip)}));
    SequenceInput sequence;
    sequence.id = {2};
    sequence.name = "registered playback";
    sequence.tracks = {std::move(pattern_track), std::move(midi_track)};
    sequence.chord_scale_lane = std::move(lane);
    ProjectInput project;
    project.id = {1};
    project.name = "registered playback";
    project.next_item_id = 500;
    project.root_sequence_id = {2};
    project.sequences.push_back(take(Sequence::create(std::move(sequence))));
    return std::make_shared<const Project>(take(Project::create(std::move(project))));
}

std::shared_ptr<const CompiledTempoMap> test_tempo_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return shared_compiled_tempo_map(points, RationalRate{48'000, 1});
}

class InlineExecutor final : public CompileExecutor {
  public:
    bool submit(std::unique_ptr<CompileTask> task, std::chrono::steady_clock::time_point) override {
        if (!task)
            return false;
        while (task->run_slice({std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                10'000}) == CompileTaskStatus::Pending) {
        }
        return true;
    }
};

std::shared_ptr<const TrackProgram> compiled_track(const PlaybackProgramStore& store, ItemId id) {
    auto live = store.read();
    REQUIRE(live);
    for (const auto& track : live->tracks())
        if (track->id() == id)
            return track;
    FAIL("compiled track is absent");
    return {};
}

runtime::Result<ContentProgramFragment, ContentFragmentError>
ignore_fragment_quota(const RegisteredContentCompileInput& input, const void*) noexcept {
    return ContentProgramFragment::create(
        {{{0}, {30}, 1000, 60, 0}, {{30}, {30}, 1000, 64, 0}, {{60}, {30}, 1000, 67, 0}},
        input.clip_duration);
}

// One registered pattern clip in a child sequence, referenced by the root
// through the window the caller names. The windows the tests choose begin and
// end inside a step rather than on a step boundary, which is the only way a
// note straddles a cut edge and clamping becomes observable at all.
std::shared_ptr<const Project> nested_pattern_project(const SchemaRegistry& schemas,
                                                      TickPosition source_start,
                                                      TickDuration placement_duration) {
    auto pattern = take(create_chord_pattern_content(
        {.seed = 1, .step = {120}, .gate = {120}, .octave = 4, .velocity = 40000}, schemas));
    auto pattern_clip = take(Clip::create({100}, {0}, {480}, std::move(pattern)));
    auto child_track = take(Track::create({20}, "pattern", {std::move(pattern_clip)}));
    SequenceInput child;
    child.id = {3};
    child.name = "child";
    child.musical_duration = TickDuration{480};
    child.tracks = {std::move(child_track)};
    child.chord_scale_lane =
        take(ChordScaleLane::create({{{0}, ChordQuality::Major, 0, ScaleMode::Major, 0}}));
    auto reference =
        take(Clip::create({200}, {0}, placement_duration, SequenceRef{{3}, source_start}));
    auto root_track = take(Track::create({10}, "reference", {std::move(reference)}));
    SequenceInput root;
    root.id = {2};
    root.name = "root";
    root.tracks = {std::move(root_track)};
    ProjectInput project_input;
    project_input.id = {1};
    project_input.name = "nested registered pattern";
    project_input.next_item_id = 500;
    project_input.root_sequence_id = {2};
    project_input.sequences = {take(Sequence::create(std::move(root))),
                               take(Sequence::create(std::move(child)))};
    return std::make_shared<const Project>(take(Project::create(std::move(project_input))));
}

struct CompiledNote {
    ItemId id;
    TickPosition start;
    TickPosition end;
    std::uint8_t pitch = 0;
};

// Pairs each note's on and off by identity rather than by position, so the
// assertions read as statements about notes and never about event order.
std::vector<CompiledNote> fold_notes(std::span<const NoteProgramEvent> events) {
    std::vector<CompiledNote> notes;
    for (const auto& event : events) {
        if (event.kind == NoteProgramEventKind::On) {
            notes.push_back({event.note_id, event.tick, event.tick, event.pitch});
            continue;
        }
        const auto match = std::find_if(notes.begin(), notes.end(), [&](const CompiledNote& note) {
            return note.id.value == event.note_id.value;
        });
        REQUIRE(match != notes.end());
        match->end = event.tick;
    }
    std::sort(notes.begin(), notes.end(), [](const CompiledNote& lhs, const CompiledNote& rhs) {
        return lhs.id.value < rhs.id.value;
    });
    return notes;
}

struct NestedCompile {
    bool has_error = false;
    CompileErrorCode code = CompileErrorCode::InvalidStructure;
    std::vector<CompiledNote> notes;
};

NestedCompile compile_nested(const std::shared_ptr<CompileContextRegistry>& registry,
                             const std::shared_ptr<const Project>& project) {
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = project;
    request.sequence_id = {2};
    request.tempo_map = test_tempo_map();
    request.sample_rate = request.tempo_map->sample_rate();
    request.document_revision = 1;
    request.dirty.all = true;
    request.invalidation = CompileInvalidationInput::baseline(registry, project, 1);
    REQUIRE(compiler.submit(std::move(request)));
    NestedCompile result;
    result.has_error = compiler.status().has_error;
    result.code = compiler.status().last_error.code;
    if (result.has_error)
        return result;
    result.notes = fold_notes(compiled_track(store, {10})->arrangement_note_events());
    return result;
}

ContentRendererRegistration renderer_registration(const SchemaRegistry& schemas) {
    CompileContextRegistry registry;
    REQUIRE_FALSE(declare_chord_pattern_renderer(registry, schemas));
    const auto* registration =
        registry.find({kChordPatternContentType, kChordPatternContentSchemaVersion});
    REQUIRE(registration != nullptr);
    return *registration;
}

ContentProgramFragment compile_pattern(const ContentRendererRegistration& registration,
                                       const RegisteredContent& content, const Project& project,
                                       TickDuration duration, TickPosition context_start = {0},
                                       std::size_t quota = kMaximumChordPatternNotes) {
    CompileContextView context(project, {2}, registration.subscriptions);
    auto compiled = registration.compile({.content = content,
                                          .clip_id = {10},
                                          .clip_duration = duration,
                                          .context_start = context_start,
                                          .context = context,
                                          .maximum_fragment_notes = quota},
                                         registration.compile_context.get());
    REQUIRE(compiled);
    return std::move(compiled).value();
}

std::uint64_t fragment_hash(std::span<const ContentFragmentNote> notes) noexcept {
    std::uint64_t hash = 14'695'981'039'346'656'037ull;
    const auto fold = [&](std::uint64_t value) {
        hash ^= value;
        hash *= 1'099'511'628'211ull;
    };
    for (const auto& note : notes) {
        fold(static_cast<std::uint64_t>(note.start.value));
        fold(static_cast<std::uint64_t>(note.duration.value));
        fold(note.velocity);
        fold(note.pitch);
        fold(note.channel);
    }
    return hash;
}

struct QualityCase {
    ChordQuality quality;
    std::array<std::uint8_t, 4> intervals;
    std::size_t count;
};

constexpr std::array<QualityCase, 10> kQualityCases{{
    {ChordQuality::Major, {0, 4, 7, 0}, 3},
    {ChordQuality::Minor, {0, 3, 7, 0}, 3},
    {ChordQuality::Diminished, {0, 3, 6, 0}, 3},
    {ChordQuality::Augmented, {0, 4, 8, 0}, 3},
    {ChordQuality::Dominant7, {0, 4, 7, 10}, 4},
    {ChordQuality::Major7, {0, 4, 7, 11}, 4},
    {ChordQuality::Minor7, {0, 3, 7, 10}, 4},
    {ChordQuality::HalfDiminished7, {0, 3, 6, 10}, 4},
    {ChordQuality::Suspended2, {0, 2, 7, 0}, 3},
    {ChordQuality::Suspended4, {0, 5, 7, 0}, 3},
}};

} // namespace

TEST_CASE("the chord pattern schema strictly validates its fixed payload",
          "[playback][chord-pattern][schema]") {
    const auto schemas = chord_registry();
    const ChordPatternContent payload{
        .seed = 17, .step = {240}, .gate = {120}, .octave = 4, .velocity = 50000};
    const auto content = take(create_chord_pattern_content(payload, schemas));
    REQUIRE(content.schema() ==
            SchemaIdentity{kChordPatternContentType, kChordPatternContentSchemaVersion});
    REQUIRE(*content.value_as<ChordPatternContent>() == payload);
    REQUIRE(content.canonical_payload_json() ==
            R"({"gate_ticks":"120","octave":"4","seed":"17","step_ticks":"240","velocity":50000})");

    const auto* schema = schemas.find(SchemaDomain::Content, kChordPatternContentType);
    REQUIRE(schema != nullptr);
    auto encoded = take(parse_json(content.canonical_payload_json()));
    const auto decoded = schema->codec.decode(encoded->root(), schema->codec.context.get());
    REQUIRE(decoded);
    REQUIRE(*static_cast<const ChordPatternContent*>(decoded.value().get()) == payload);

    auto invalid_gate = payload;
    invalid_gate.gate = {241};
    auto invalid_step = payload;
    invalid_step.step = {0};
    auto invalid_octave = payload;
    invalid_octave.octave = 8;
    auto invalid_velocity = payload;
    invalid_velocity.velocity = 0;
    for (const auto& invalid : {invalid_gate, invalid_step, invalid_octave, invalid_velocity}) {
        const auto rejected = create_chord_pattern_content(invalid, schemas);
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == PersistenceErrorCode::InvalidSchema);
    }

    auto extra = take(parse_json(
        R"({"seed":"17","step_ticks":"240","gate_ticks":"120","octave":"4","velocity":50000,"extra":0})"));
    const auto extra_decoded = schema->codec.decode(extra->root(), schema->codec.context.get());
    REQUIRE_FALSE(extra_decoded);
    REQUIRE(extra_decoded.error().code == PersistenceErrorCode::InvalidSchema);
}

TEST_CASE("the chord pattern renderer declares an exact deterministic compiler",
          "[playback][chord-pattern][registration]") {
    const auto schemas = chord_registry();
    CompileContextRegistry registry;
    REQUIRE_FALSE(declare_chord_pattern_renderer(registry, schemas));
    REQUIRE(registry.size() == 1);
    const auto* registration =
        registry.find({kChordPatternContentType, kChordPatternContentSchemaVersion});
    REQUIRE(registration != nullptr);
    REQUIRE(registration->subscriptions.reads(CompileContextKind::ChordScale));
    REQUIRE_FALSE(registration->subscriptions.reads(CompileContextKind::Groove));
    REQUIRE(registration->maximum_fragment_notes == kMaximumChordPatternNotes);
    REQUIRE(registration->state_policy == RegisteredRendererStatePolicy::Reset);
    REQUIRE(registration->production.mode == ProductionMode::Synchronous);
    REQUIRE(registration->production.reproducibility == ReproducibilityClass::Deterministic);
    REQUIRE(registration->compile != nullptr);
    REQUIRE(registry.find({"pulp.timeline.midi", 1}) == nullptr);
}

TEST_CASE("the chord pattern emits nothing before authored harmony and is deterministic",
          "[playback][chord-pattern][compile]") {
    const auto schemas = chord_registry();
    const auto registration = renderer_registration(schemas);
    const auto content = take(create_chord_pattern_content(
        {.seed = 1, .step = {120}, .gate = {90}, .octave = 4, .velocity = 40000}, schemas));
    const auto project =
        project_with_harmony({{{240}, ChordQuality::Major7, 0, ScaleMode::Major, 0}});

    const auto first = compile_pattern(registration, content, project, {600});
    const auto second = compile_pattern(registration, content, project, {600});
    REQUIRE(first.notes().size() == second.notes().size());
    REQUIRE(std::equal(first.notes().begin(), first.notes().end(), second.notes().begin()));
    REQUIRE(first.notes().size() == 3);
    REQUIRE(first.notes()[0] == ContentFragmentNote{{240}, {90}, 40000, 71, 0});
    REQUIRE(first.notes()[1] == ContentFragmentNote{{360}, {90}, 40000, 60, 0});
    REQUIRE(first.notes()[2] == ContentFragmentNote{{480}, {90}, 40000, 64, 0});
    REQUIRE(fragment_hash(first.notes()) == 14'629'979'816'906'693'978ull);
    REQUIRE(fragment_hash(second.notes()) == 14'629'979'816'906'693'978ull);
}

TEST_CASE("an undeclared chord read is absent inside the production hook",
          "[playback][chord-pattern][compile-context]") {
    const auto schemas = chord_registry();
    auto registration = renderer_registration(schemas);
    registration.subscriptions = CompileContextSubscriptions::none();
    const auto content = take(create_chord_pattern_content(
        {.seed = 0, .step = {120}, .gate = {60}, .octave = 4, .velocity = 30000}, schemas));
    const auto project = project_with_harmony({{{0}, ChordQuality::Major, 0, ScaleMode::Major, 0}});
    const auto fragment = compile_pattern(registration, content, project, {240});
    REQUIRE(fragment.notes().empty());
}

TEST_CASE("every chord quality uses its explicit interval table",
          "[playback][chord-pattern][compile]") {
    const auto schemas = chord_registry();
    const auto registration = renderer_registration(schemas);
    for (const auto& quality : kQualityCases) {
        const auto project =
            project_with_harmony({{{0}, quality.quality, 2, ScaleMode::Chromatic, 0}});
        for (std::size_t tone = 0; tone < quality.count; ++tone) {
            const auto content = take(create_chord_pattern_content(
                {.seed = tone, .step = {120}, .gate = {60}, .octave = 4, .velocity = 0xffff},
                schemas));
            const auto fragment = compile_pattern(registration, content, project, {120});
            REQUIRE(fragment.notes().size() == 1);
            REQUIRE(fragment.notes()[0].pitch == 62 + quality.intervals[tone]);
        }
    }
}

TEST_CASE("the chord pattern compiler refuses output beyond its supplied quota",
          "[playback][chord-pattern][compile]") {
    const auto schemas = chord_registry();
    const auto registration = renderer_registration(schemas);
    const auto content = take(create_chord_pattern_content(
        {.seed = 0, .step = {120}, .gate = {60}, .octave = 4, .velocity = 0xffff}, schemas));
    const auto project = project_with_harmony({{{0}, ChordQuality::Major, 0, ScaleMode::Major, 0}});
    CompileContextView context(project, {2}, registration.subscriptions);
    const auto compiled = registration.compile({.content = content,
                                                .clip_id = {10},
                                                .clip_duration = {360},
                                                .context_start = {0},
                                                .context = context,
                                                .maximum_fragment_notes = 2},
                                               registration.compile_context.get());
    REQUIRE_FALSE(compiled);
    REQUIRE(compiled.error().code == ContentFragmentErrorCode::RendererFailed);
}

TEST_CASE("ProgramCompiler realizes registered harmony and leaves authored MIDI untouched",
          "[playback][chord-pattern][program-compiler]") {
    const auto schemas = chord_registry();
    auto registry = std::make_shared<CompileContextRegistry>();
    REQUIRE_FALSE(declare_chord_pattern_renderer(*registry, schemas));
    const auto before = playback_project(
        schemas,
        take(ChordScaleLane::create({{{0}, ChordQuality::Major, 0, ScaleMode::Major, 0}})));
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto tempo = test_tempo_map();

    ProgramCompileRequest initial;
    initial.project = before;
    initial.sequence_id = {2};
    initial.tempo_map = tempo;
    initial.sample_rate = tempo->sample_rate();
    initial.document_revision = 1;
    initial.dirty.all = true;
    initial.invalidation = CompileInvalidationInput::baseline(registry, before, 1);
    REQUIRE(compiler.submit(std::move(initial)));
    REQUIRE_FALSE(compiler.status().has_error);
    const auto pattern_before = compiled_track(store, {10});
    const auto midi_before = compiled_track(store, {20});
    REQUIRE(pattern_before->state_policy() == RendererStatePolicy::Stateless);
    REQUIRE(midi_before->state_policy() == RendererStatePolicy::CarryByItemId);
    const auto pattern_events = pattern_before->arrangement_note_events();
    REQUIRE(pattern_events.size() == 8);
    REQUIRE(pattern_before->generated_id_start() == 500);
    REQUIRE(pattern_before->generated_id_count() == 4);
    for (std::size_t note = 0; note < 4; ++note) {
        const auto expected_id = ItemId{500 + note};
        REQUIRE(pattern_events[note * 2].note_id == expected_id);
        REQUIRE(pattern_events[note * 2 + 1].note_id == expected_id);
        REQUIRE(pattern_events[note * 2].kind == NoteProgramEventKind::On);
        REQUIRE(pattern_events[note * 2 + 1].kind == NoteProgramEventKind::Off);
        for (std::size_t earlier = 0; earlier < note; ++earlier)
            REQUIRE(pattern_events[earlier * 2].note_id != expected_id);
    }
    REQUIRE(pattern_events[0].pitch == 64);
    REQUIRE(pattern_events[2].pitch == 67);
    REQUIRE(pattern_events[4].pitch == 60);
    REQUIRE(pattern_events[6].pitch == 64);
    REQUIRE(midi_before->arrangement_note_events().size() == 2);
    REQUIRE(midi_before->arrangement_note_events()[0].pitch == 55);
    REQUIRE(track_production_declaration(*pattern_before).reproducibility ==
            ReproducibilityClass::Deterministic);

    Transaction transaction;
    transaction.id = {{1}, 1};
    const auto minor =
        take(ChordScaleLane::create({{{0}, ChordQuality::Minor, 2, ScaleMode::Dorian, 2}}));
    transaction.commands.push_back(
        {{{1}, 1}, SetChordScaleLane{{2}, before->find_sequence({2})->chord_scale_lane(), minor}});
    const auto reduced = take(reduce_transaction(*before, transaction));
    const auto after = std::make_shared<const Project>(reduced.project);
    const CommitResult committed{after, DocumentRevision{2}, reduced.dirty, {}, before};

    ProgramCompileRequest changed;
    changed.project = after;
    changed.sequence_id = {2};
    changed.tempo_map = tempo;
    changed.sample_rate = tempo->sample_rate();
    changed.document_revision = 2;
    changed.invalidation = CompileInvalidationInput{registry, committed};
    REQUIRE(compiler.submit(std::move(changed)));
    REQUIRE_FALSE(compiler.status().has_error);
    const auto pattern_after = compiled_track(store, {10});
    const auto midi_after = compiled_track(store, {20});
    REQUIRE(pattern_after != pattern_before);
    REQUIRE(midi_after == midi_before);
    REQUIRE(pattern_after->arrangement_note_events()[0].pitch == 65);
    REQUIRE(midi_after->arrangement_note_events()[0].pitch == 55);

    Transaction removal;
    removal.id = {{1}, 2};
    removal.commands.push_back({{{1}, 1}, RemoveClip{{2}, {10}, {100}}});
    const auto removed = take(reduce_transaction(*after, removal));
    const auto without_pattern = std::make_shared<const Project>(removed.project);
    const CommitResult removed_commit{
        without_pattern, DocumentRevision{3}, removed.dirty, {}, after};
    ProgramCompileRequest removal_request;
    removal_request.project = without_pattern;
    removal_request.sequence_id = {2};
    removal_request.tempo_map = tempo;
    removal_request.sample_rate = tempo->sample_rate();
    removal_request.document_revision = 3;
    removal_request.invalidation = CompileInvalidationInput{registry, removed_commit};
    REQUIRE(compiler.submit(std::move(removal_request)));
    REQUIRE_FALSE(compiler.status().has_error);
    const auto pattern_removed = compiled_track(store, {10});
    REQUIRE(pattern_removed != pattern_after);
    REQUIRE(pattern_removed->arrangement_note_events().empty());
    REQUIRE(pattern_removed->requested_state_policy() == RendererStatePolicy::CarryByItemId);
    REQUIRE(pattern_removed->state_policy() == RendererStatePolicy::CarryByItemId);
}

TEST_CASE("an untrimmed nested registered pattern compiles its whole authored extent",
          "[playback][chord-pattern][nesting]") {
    const auto schemas = chord_registry();
    auto registry = std::make_shared<CompileContextRegistry>();
    REQUIRE_FALSE(declare_chord_pattern_renderer(*registry, schemas));
    const auto whole = compile_nested(registry, nested_pattern_project(schemas, {0}, {480}));
    REQUIRE_FALSE(whole.has_error);
    REQUIRE(whole.notes.size() == 4);
    const std::array expected_starts{0, 120, 240, 360};
    const std::array expected_ends{120, 240, 360, 480};
    const std::array<std::uint8_t, 4> expected_pitches{64, 67, 60, 64};
    for (std::size_t index = 0; index < whole.notes.size(); ++index) {
        REQUIRE(whole.notes[index].start.value == expected_starts[index]);
        REQUIRE(whole.notes[index].end.value == expected_ends[index]);
        REQUIRE(whole.notes[index].pitch == expected_pitches[index]);
    }
}

TEST_CASE("a trimmed nested registered pattern is windowed and keeps its authored phase",
          "[playback][chord-pattern][nesting]") {
    const auto schemas = chord_registry();
    auto registry = std::make_shared<CompileContextRegistry>();
    REQUIRE_FALSE(declare_chord_pattern_renderer(*registry, schemas));
    const auto whole = compile_nested(registry, nested_pattern_project(schemas, {0}, {480}));
    REQUIRE_FALSE(whole.has_error);
    // Child ticks [60, 300) of a 480-tick authored clip, placed at root tick 0.
    // Both edges fall mid-step, so one authored step straddles each of them.
    const auto window = compile_nested(registry, nested_pattern_project(schemas, {60}, {240}));
    REQUIRE_FALSE(window.has_error);

    // Three of the four authored steps reach the window: one straddling each
    // edge and one wholly inside. The fourth begins past the window and is
    // dropped, yet still spends its generated identity, so the retained three
    // keep the ordinals they were generated with.
    REQUIRE(window.notes.size() == 3);
    REQUIRE(window.notes[1].id.value == window.notes[0].id.value + 1);
    REQUIRE(window.notes[2].id.value == window.notes[0].id.value + 2);

    // Cut to the left edge, interior, cut to the right edge — the same
    // clamp-and-drop a trim applies to authored note content.
    REQUIRE(window.notes[0].start.value == 0);
    REQUIRE(window.notes[0].end.value == 60);
    REQUIRE(window.notes[1].start.value == 60);
    REQUIRE(window.notes[1].end.value == 180);
    REQUIRE(window.notes[2].start.value == 180);
    REQUIRE(window.notes[2].end.value == 240);

    // The pitches are the authored steps 0, 1 and 2, so the pattern kept the
    // phase its author wrote. A renderer restarted at the window boundary would
    // emit two notes here, beginning at step 0 again.
    for (std::size_t index = 0; index < window.notes.size(); ++index)
        REQUIRE(window.notes[index].pitch == whole.notes[index].pitch);
}

TEST_CASE("a trimmed nested registered fragment is bounded by the authored extent",
          "[playback][chord-pattern][nesting][quota]") {
    const auto schemas = chord_registry();
    // The authored clip generates four steps and the retained window would hold
    // two. Generation happens over the authored extent, so a ceiling that the
    // window alone would clear still refuses: the bound is charged against what
    // the hook is asked to produce, not against what survives the window.
    auto registration = renderer_registration(schemas);
    registration.maximum_fragment_notes = 3;
    auto registry = std::make_shared<CompileContextRegistry>();
    REQUIRE_FALSE(registry->declare(std::move(registration), schemas));
    const auto window = compile_nested(registry, nested_pattern_project(schemas, {60}, {240}));
    REQUIRE(window.has_error);
    REQUIRE(window.code == CompileErrorCode::RegisteredContentCompileFailed);
}

TEST_CASE("a count-changing fragment rekeys every downstream generated range",
          "[playback][chord-pattern][program-compiler][generated-ids]") {
    const auto schemas = chord_registry();
    auto registry = std::make_shared<CompileContextRegistry>();
    REQUIRE_FALSE(declare_chord_pattern_renderer(*registry, schemas));
    const auto pattern_clip = [&](ItemId id, TickDuration duration, std::uint64_t seed) {
        auto content = take(create_chord_pattern_content(
            {.seed = seed, .step = {120}, .gate = {90}, .octave = 4, .velocity = 32000}, schemas));
        return take(Clip::create(id, {0}, duration, std::move(content)));
    };
    auto first = take(Track::create({10}, "first", {pattern_clip({100}, {480}, 0)}));
    auto second = take(Track::create({20}, "second", {pattern_clip({200}, {240}, 1)}));
    SequenceInput sequence;
    sequence.id = {2};
    sequence.name = "generated ranges";
    sequence.tracks = {std::move(first), std::move(second)};
    sequence.chord_scale_lane =
        take(ChordScaleLane::create({{{0}, ChordQuality::Major, 0, ScaleMode::Major, 0}}));
    ProjectInput input;
    input.id = {1};
    input.name = "generated ranges";
    input.next_item_id = 500;
    input.root_sequence_id = {2};
    input.sequences = {take(Sequence::create(std::move(sequence)))};
    auto before = std::make_shared<const Project>(take(Project::create(std::move(input))));

    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest initial;
    initial.project = before;
    initial.sequence_id = {2};
    initial.tempo_map = test_tempo_map();
    initial.sample_rate = initial.tempo_map->sample_rate();
    initial.document_revision = 1;
    initial.dirty.all = true;
    initial.invalidation = CompileInvalidationInput::baseline(registry, before, 1);
    REQUIRE(compiler.submit(std::move(initial)));
    const auto first_before = compiled_track(store, {10});
    const auto second_before = compiled_track(store, {20});
    REQUIRE(first_before->generated_id_start() == 500);
    REQUIRE(first_before->generated_id_count() == 4);
    REQUIRE(second_before->generated_id_start() == 504);
    REQUIRE(second_before->generated_id_count() == 2);

    Transaction resize;
    resize.id = {{1}, 1};
    const auto* authored = before->find_sequence({2})->find_track({10})->find_clip({100});
    REQUIRE(authored != nullptr);
    resize.commands.push_back({{{1}, 1},
                               MoveClip{{2},
                                        {10},
                                        {100},
                                        authored->time_range(),
                                        ClipTimeRange{MusicalTimeRange{{0}, {240}}}}});
    const auto reduced = take(reduce_transaction(*before, resize));
    auto after = std::make_shared<const Project>(reduced.project);
    const CommitResult committed{after, DocumentRevision{2}, reduced.dirty, {}, before};
    ProgramCompileRequest changed;
    changed.project = after;
    changed.sequence_id = {2};
    changed.tempo_map = test_tempo_map();
    changed.sample_rate = changed.tempo_map->sample_rate();
    changed.document_revision = 2;
    changed.invalidation = CompileInvalidationInput{registry, committed};
    REQUIRE(compiler.submit(std::move(changed)));
    REQUIRE_FALSE(compiler.status().has_error);

    const auto first_after = compiled_track(store, {10});
    const auto second_after = compiled_track(store, {20});
    REQUIRE(first_after != first_before);
    REQUIRE(second_after != second_before);
    REQUIRE(first_after->generated_id_start() == 500);
    REQUIRE(first_after->generated_id_count() == 2);
    REQUIRE(second_after->generated_id_start() == 502);
    REQUIRE(second_after->generated_id_count() == 2);
    const auto events = second_after->arrangement_note_events();
    REQUIRE(events.size() == 4);
    REQUIRE(events[0].note_id == ItemId{502});
    REQUIRE(events[1].note_id == ItemId{502});
    REQUIRE(events[2].note_id == ItemId{503});
    REQUIRE(events[3].note_id == ItemId{503});
}

TEST_CASE("ProgramCompiler refuses unresolved and quota-violating registered content",
          "[playback][chord-pattern][program-compiler][refusal]") {
    const auto schemas = chord_registry();
    const auto project = playback_project(
        schemas,
        take(ChordScaleLane::create({{{0}, ChordQuality::Major, 0, ScaleMode::Major, 0}})));
    const auto tempo = test_tempo_map();

    SECTION("unresolved") {
        auto empty_registry = std::make_shared<CompileContextRegistry>();
        PlaybackProgramStore store;
        InlineExecutor executor;
        PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
        ProgramCompileRequest request;
        request.project = project;
        request.sequence_id = {2};
        request.tempo_map = tempo;
        request.sample_rate = tempo->sample_rate();
        request.document_revision = 1;
        request.dirty.all = true;
        request.invalidation = CompileInvalidationInput::baseline(empty_registry, project, 1);
        REQUIRE(compiler.submit(std::move(request)));
        REQUIRE(compiler.status().has_error);
        REQUIRE(compiler.status().last_error.code == CompileErrorCode::UnresolvedRegisteredContent);
        REQUIRE(compiler.status().last_error.item == ItemId{100});
        REQUIRE_FALSE(store.read());
    }

    SECTION("hook ignores quota") {
        CompileContextRegistry declared;
        REQUIRE_FALSE(declare_chord_pattern_renderer(declared, schemas));
        auto registration =
            *declared.find({kChordPatternContentType, kChordPatternContentSchemaVersion});
        registration.maximum_fragment_notes = 2;
        registration.compile = ignore_fragment_quota;
        CompileContextRegistry strict;
        REQUIRE_FALSE(strict.declare(std::move(registration), schemas));
        auto registry = std::make_shared<CompileContextRegistry>(std::move(strict));
        PlaybackProgramStore store;
        InlineExecutor executor;
        PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
        ProgramCompileRequest request;
        request.project = project;
        request.sequence_id = {2};
        request.tempo_map = tempo;
        request.sample_rate = tempo->sample_rate();
        request.document_revision = 1;
        request.dirty.all = true;
        request.invalidation = CompileInvalidationInput::baseline(registry, project, 1);
        REQUIRE(compiler.submit(std::move(request)));
        REQUIRE(compiler.status().has_error);
        const auto error = compiler.status().last_error;
        REQUIRE(error.code == CompileErrorCode::RegisteredContentFragmentQuotaExceeded);
        REQUIRE(error.item == ItemId{100});
        REQUIRE(error.actual == 3);
        REQUIRE(error.limit == 2);
        REQUIRE_FALSE(store.read());
    }
}

TEST_CASE("registered production aggregates the weakest active content declaration",
          "[playback][chord-pattern][production]") {
    const auto schemas = chord_registry();
    CompileContextRegistry declared;
    REQUIRE_FALSE(declare_chord_pattern_renderer(declared, schemas));
    auto registration =
        *declared.find({kChordPatternContentType, kChordPatternContentSchemaVersion});
    registration.production.reproducibility = ReproducibilityClass::BestEffort;
    CompileContextRegistry weakened;
    REQUIRE_FALSE(weakened.declare(std::move(registration), schemas));
    auto registry = std::make_shared<CompileContextRegistry>(std::move(weakened));
    const auto project = playback_project(
        schemas,
        take(ChordScaleLane::create({{{0}, ChordQuality::Major, 0, ScaleMode::Major, 0}})));
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = project;
    request.sequence_id = {2};
    request.tempo_map = test_tempo_map();
    request.sample_rate = request.tempo_map->sample_rate();
    request.document_revision = 1;
    request.dirty.all = true;
    request.invalidation = CompileInvalidationInput::baseline(registry, project, 1);
    REQUIRE(compiler.submit(std::move(request)));
    REQUIRE_FALSE(compiler.status().has_error);
    REQUIRE(track_production_declaration(*compiled_track(store, {10})).reproducibility ==
            ReproducibilityClass::BestEffort);
    REQUIRE(track_production_declaration(*compiled_track(store, {20})).reproducibility ==
            ReproducibilityClass::Deterministic);
    auto live = store.read();
    REQUIRE(live);
    REQUIRE(program_reproducibility(*live) == ReproducibilityClass::BestEffort);
}
