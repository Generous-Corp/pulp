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

TEST_CASE("a trimmed nested registered pattern is explicitly refused",
          "[playback][chord-pattern][nesting][refusal]") {
    const auto schemas = chord_registry();
    auto registry = std::make_shared<CompileContextRegistry>();
    REQUIRE_FALSE(declare_chord_pattern_renderer(*registry, schemas));
    auto pattern = take(create_chord_pattern_content(
        {.seed = 1, .step = {120}, .gate = {90}, .octave = 4, .velocity = 40000}, schemas));
    auto pattern_clip = take(Clip::create({100}, {0}, {480}, std::move(pattern)));
    auto child_track = take(Track::create({20}, "pattern", {std::move(pattern_clip)}));
    SequenceInput child;
    child.id = {3};
    child.name = "child";
    child.musical_duration = TickDuration{480};
    child.tracks = {std::move(child_track)};
    auto reference = take(Clip::create({200}, {0}, {240}, SequenceRef{{3}, TickPosition{120}}));
    auto root_track = take(Track::create({10}, "reference", {std::move(reference)}));
    SequenceInput root;
    root.id = {2};
    root.name = "root";
    root.tracks = {std::move(root_track)};
    ProjectInput project_input;
    project_input.id = {1};
    project_input.name = "trimmed registered pattern";
    project_input.next_item_id = 500;
    project_input.root_sequence_id = {2};
    project_input.sequences = {take(Sequence::create(std::move(root))),
                               take(Sequence::create(std::move(child)))};
    auto project = std::make_shared<const Project>(take(Project::create(std::move(project_input))));

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
    REQUIRE(compiler.status().has_error);
    REQUIRE(compiler.status().last_error.code ==
            CompileErrorCode::TrimmedRegisteredContentUnsupported);
    REQUIRE(compiler.status().last_error.item == ItemId{100});
    REQUIRE_FALSE(store.read());
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
